#include "json_scan.h"

namespace tombstone {

namespace {

/** Index just past `"key"` followed by optional whitespace and `:`; npos if absent. */
std::size_t find_value_start(std::string_view json, std::string_view key) {
    const std::string quoted = '"' + std::string{key} + '"';
    std::size_t at = 0;
    while ((at = json.find(quoted, at)) != std::string_view::npos) {
        std::size_t cursor = at + quoted.size();
        while (cursor < json.size() &&
               (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\n' ||
                json[cursor] == '\r')) {
            ++cursor;
        }
        if (cursor < json.size() && json[cursor] == ':') {
            ++cursor;
            while (cursor < json.size() &&
                   (json[cursor] == ' ' || json[cursor] == '\t' || json[cursor] == '\n' ||
                    json[cursor] == '\r')) {
                ++cursor;
            }
            return cursor;
        }
        at += quoted.size();
    }
    return std::string_view::npos;
}

void append_utf8(std::string &out, unsigned code_point) {
    if (code_point < 0x80U) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800U) {
        out += static_cast<char>(0xC0U | (code_point >> 6U));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    } else {
        out += static_cast<char>(0xE0U | (code_point >> 12U));
        out += static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code_point & 0x3FU));
    }
}

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** Read and unescape the JSON string starting at the opening quote. */
std::optional<std::string> read_string(std::string_view json, std::size_t at) {
    if (at >= json.size() || json[at] != '"') {
        return std::nullopt;
    }
    std::string out;
    std::size_t cursor = at + 1;
    while (cursor < json.size()) {
        const char c = json[cursor];
        if (c == '"') {
            return out;
        }
        if (c != '\\') {
            out += c;
            ++cursor;
            continue;
        }
        if (cursor + 1 >= json.size()) {
            return std::nullopt;
        }
        const char escape = json[cursor + 1];
        cursor += 2;
        switch (escape) {
        case '"':
            out += '"';
            break;
        case '\\':
            out += '\\';
            break;
        case '/':
            out += '/';
            break;
        case 'n':
            out += '\n';
            break;
        case 'r':
            out += '\r';
            break;
        case 't':
            out += '\t';
            break;
        case 'b':
            out += '\b';
            break;
        case 'f':
            out += '\f';
            break;
        case 'u': {
            if (cursor + 4 > json.size()) {
                return std::nullopt;
            }
            unsigned code_point = 0;
            for (std::size_t i = 0; i < 4; ++i) {
                const int digit = hex_value(json[cursor + i]);
                if (digit < 0) {
                    return std::nullopt;
                }
                code_point = code_point * 16U + static_cast<unsigned>(digit);
            }
            cursor += 4;
            append_utf8(out, code_point);
            break;
        }
        default:
            return std::nullopt;  // invalid escape — refuse rather than guess
        }
    }
    return std::nullopt;  // unterminated
}

}  // namespace

std::optional<std::string> find_string_field(std::string_view json, std::string_view key) {
    const std::size_t at = find_value_start(json, key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    return read_string(json, at);
}

std::optional<bool> find_bool_field(std::string_view json, std::string_view key) {
    const std::size_t at = find_value_start(json, key);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    if (json.compare(at, 4, "true") == 0) {
        return true;
    }
    if (json.compare(at, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::optional<long long> find_int_field(std::string_view json, std::string_view key) {
    const std::size_t at = find_value_start(json, key);
    if (at == std::string_view::npos || at >= json.size()) {
        return std::nullopt;
    }
    std::size_t cursor = at;
    bool negative = false;
    if (json[cursor] == '-') {
        negative = true;
        ++cursor;
    }
    constexpr std::size_t max_digits = 18;  // fits in int64 without overflow
    long long value = 0;
    std::size_t digits = 0;
    while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9' &&
           digits < max_digits) {
        value = value * 10 + (json[cursor] - '0');
        ++cursor;
        ++digits;
    }
    if (digits == 0) {
        return std::nullopt;  // no integer token (absent, or a non-numeric value)
    }
    // Reject a fractional/exponent value: only a clean integer round-trips safely.
    if (cursor < json.size() && (json[cursor] == '.' || json[cursor] == 'e' ||
                                 json[cursor] == 'E' ||
                                 (json[cursor] >= '0' && json[cursor] <= '9'))) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<std::string> find_log_upload_url(std::string_view response_body) {
    const std::size_t at = find_value_start(response_body, "logUpload");
    if (at == std::string_view::npos || at >= response_body.size() ||
        response_body[at] != '{') {
        return std::nullopt;
    }
    // Scan only inside the logUpload object (up to its matching closing brace).
    std::size_t depth = 0;
    std::size_t end = at;
    bool in_string = false;
    for (; end < response_body.size(); ++end) {
        const char c = response_body[end];
        if (in_string) {
            if (c == '\\') {
                ++end;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                ++end;
                break;
            }
        }
    }
    return find_string_field(response_body.substr(at, end - at), "url");
}

std::vector<PendingPullRequest> find_pending_requests(std::string_view response_body) {
    std::vector<PendingPullRequest> out;
    const std::size_t at = find_value_start(response_body, "pendingRequests");
    if (at == std::string_view::npos || at >= response_body.size() || response_body[at] != '[') {
        return out;
    }
    // Walk the array, isolating each top-level object so per-object scalar scans
    // (find_string_field) cannot bleed into a sibling. Bounded — a malformed or
    // oversized list is truncated rather than trusted.
    constexpr std::size_t max_requests = 32;
    std::size_t depth = 0;
    bool in_string = false;
    std::size_t object_start = std::string_view::npos;
    for (std::size_t i = at; i < response_body.size(); ++i) {
        const char c = response_body[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // skip the escaped char
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth == 0) {
                object_start = i;
            }
            ++depth;
        } else if (c == '}') {
            if (depth > 0) {
                --depth;
                if (depth == 0 && object_start != std::string_view::npos) {
                    const std::string_view object =
                        response_body.substr(object_start, i - object_start + 1);
                    PendingPullRequest request;
                    request.request_id = find_string_field(object, "requestId").value_or("");
                    request.target_type = find_string_field(object, "targetType").value_or("");
                    request.target_value = find_string_field(object, "targetValue").value_or("");
                    request.fulfill_nonce = find_string_field(object, "fulfillNonce").value_or("");
                    request.nonce_expiry = find_int_field(object, "nonceExpiry").value_or(0);
                    out.push_back(std::move(request));
                    object_start = std::string_view::npos;
                    if (out.size() >= max_requests) {
                        break;
                    }
                }
            }
        } else if (c == ']' && depth == 0) {
            break;  // end of the pendingRequests array
        }
    }
    return out;
}

}  // namespace tombstone
