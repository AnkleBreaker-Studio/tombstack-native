#include "json_writer.h"

#include <clocale>
#include <cstdio>
#include <cstring>

namespace tombstone {

std::string json_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8] = {};
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned>(static_cast<unsigned char>(c)));
                out += buffer;
            } else {
                out += c;
            }
            break;
        }
    }
    return out;
}

std::string_view utf8_safe_truncate(std::string_view value, std::size_t max_bytes) noexcept {
    if (value.size() <= max_bytes) {
        return value;
    }
    std::size_t end = max_bytes;
    // Back off past UTF-8 continuation bytes (10xxxxxx) so no sequence splits.
    while (end > 0 && (static_cast<unsigned char>(value[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return value.substr(0, end);
}

JsonWriter::JsonWriter() { out_.reserve(256); }

void JsonWriter::begin_object() {
    element_prefix();
    out_ += '{';
    push_scope();
}

void JsonWriter::begin_object(std::string_view name) {
    name_prefix(name);
    out_ += '{';
    push_scope();
}

void JsonWriter::begin_array(std::string_view name) {
    name_prefix(name);
    out_ += '[';
    push_scope();
}

void JsonWriter::end_object() {
    out_ += '}';
    pop_scope();
}

void JsonWriter::end_array() {
    out_ += ']';
    pop_scope();
}

void JsonWriter::string_field(std::string_view name, std::string_view value) {
    name_prefix(name);
    out_ += '"';
    out_ += json_escape(value);
    out_ += '"';
}

void JsonWriter::bool_field(std::string_view name, bool value) {
    name_prefix(name);
    out_ += value ? "true" : "false";
}

void JsonWriter::number_field(std::string_view name, double value) {
    name_prefix(name);
    char buffer[40] = {};
    // %.17g is the shortest round-trip precision for an IEEE-754 double; the
    // caller guarantees finiteness, so "inf"/"nan" never reach the wire.
    const int n = std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    // snprintf honors the process-global LC_NUMERIC: a host game that called
    // setlocale(LC_NUMERIC, "de_DE") makes %g print a decimal COMMA — invalid
    // JSON that poisons the ENTIRE payload this field rides on (the server
    // rejects the whole heartbeat, silently losing CCU/metadata for every
    // player in that locale). The Unity SDK guards the same hazard with
    // CultureInfo.InvariantCulture — normalize the locale's decimal separator
    // back to '.' here. %g never emits digit grouping, so the separator is
    // the only locale-dependent byte sequence in the output.
    const char *dp = std::localeconv()->decimal_point;
    if (n > 0 && dp != nullptr && dp[0] != '\0' && !(dp[0] == '.' && dp[1] == '\0')) {
        const size_t len = static_cast<size_t>(n) < sizeof(buffer) ? static_cast<size_t>(n) : sizeof(buffer) - 1;
        std::string s(buffer, len);
        const size_t pos = s.find(dp);
        if (pos != std::string::npos) s.replace(pos, std::strlen(dp), ".");
        out_ += s;
        return;
    }
    out_ += buffer;
}

void JsonWriter::int_field(std::string_view name, long long value) {
    name_prefix(name);
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "%lld", value);
    out_ += buffer;
}

void JsonWriter::raw_element(std::string_view raw) {
    element_prefix();
    out_.append(raw.data(), raw.size());
}

void JsonWriter::raw_field(std::string_view name, std::string_view raw) {
    name_prefix(name);
    out_.append(raw.data(), raw.size());
}

void JsonWriter::element_prefix() {
    if (!needs_comma_.empty()) {
        if (needs_comma_.back()) {
            out_ += ',';
        }
        needs_comma_.back() = true;
    }
}

void JsonWriter::name_prefix(std::string_view name) {
    element_prefix();
    out_ += '"';
    out_ += json_escape(name);
    out_ += "\":";
}

void JsonWriter::push_scope() { needs_comma_.push_back(false); }

void JsonWriter::pop_scope() {
    if (!needs_comma_.empty()) {
        needs_comma_.pop_back();
    }
}

}  // namespace tombstone
