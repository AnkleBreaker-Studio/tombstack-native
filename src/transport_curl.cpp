#include "transport.h"

#include "sdk_log.h"
#include "signature.h"

#include <curl/curl.h>

#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string_view>

namespace tombstone {

namespace {

// Process-global curl init, reference counted so multiple init/shutdown
// cycles stay balanced. curl_global_init is not thread-safe -> mutex.
std::mutex g_curl_global_mutex;
int g_curl_global_refs = 0;

void curl_global_acquire() {
    const std::lock_guard<std::mutex> lock(g_curl_global_mutex);
    if (g_curl_global_refs == 0) {
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    ++g_curl_global_refs;
}

void curl_global_release() {
    const std::lock_guard<std::mutex> lock(g_curl_global_mutex);
    if (g_curl_global_refs > 0 && --g_curl_global_refs == 0) {
        curl_global_cleanup();
    }
}

extern "C" size_t tombstone_curl_write(char *data, size_t size, size_t count, void *out) {
    auto *body = static_cast<std::string *>(out);
    const size_t total = size * count;
    try {
        body->append(data, total);
    } catch (...) {
        return 0;  // signal failure to curl
    }
    return total;
}

/** Minimal header capture: keep only the Retry-After value (case-insensitive
 *  name match; the pure parser handles OWS/CRLF trimming and the seconds form).
 *  Everything else is ignored — the SDK has no other header interest. */
extern "C" size_t tombstone_curl_header(char *data, size_t size, size_t count, void *out) {
    auto *retry_after = static_cast<std::string *>(out);
    const size_t total = size * count;
    try {
        const std::string_view line{data, total};
        constexpr std::string_view name{"retry-after:"};
        if (line.size() > name.size()) {
            bool matches = true;
            for (std::size_t i = 0; i < name.size(); ++i) {
                const char c = line[i];
                const char lower =
                    (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                if (lower != name[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                retry_after->assign(line.substr(name.size()));
            }
        }
    } catch (...) {
        return 0;  // signal failure to curl
    }
    return total;
}

struct CurlHandleDeleter {
    void operator()(CURL *handle) const noexcept { curl_easy_cleanup(handle); }
};
using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

struct CurlSlistDeleter {
    void operator()(curl_slist *list) const noexcept { curl_slist_free_all(list); }
};
using CurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

CurlSlist append_header(CurlSlist list, const char *header) {
    curl_slist *next = curl_slist_append(list.get(), header);
    if (next != nullptr) {
        (void)list.release();  // ownership moved into the grown list
        return CurlSlist{next};
    }
    return list;
}

HttpResponse perform(CURL *handle, SdkLog &sdk_log) {
    std::string body;
    std::string retry_after;
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, tombstone_curl_write);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, tombstone_curl_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &retry_after);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    // Assert TLS verification explicitly rather than inheriting the integrator's libcurl defaults:
    // this SDK ships a Bearer ingest token to a remote endpoint and must not accept an untrusted or
    // MITM'd cert even if the host build flipped the defaults. (libcurl defaults to these today.)
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);

    const CURLcode code = curl_easy_perform(handle);
    HttpResponse response;
    if (code != CURLE_OK) {
        sdk_log.debug(std::string{"http transport error: "} + curl_easy_strerror(code));
        return response;  // transport_error = true
    }
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    response.transport_error = false;
    response.status = status;
    response.body = std::move(body);
    response.retry_after = std::move(retry_after);
    return response;
}

}  // namespace

Transport::Transport(SdkLog &sdk_log) : sdk_log_(sdk_log) { curl_global_acquire(); }

Transport::~Transport() { curl_global_release(); }

HttpResponse Transport::post_json(const std::string &url, const std::string &token,
                                  const std::string &body, long timeout_seconds, bool sign) {
    try {
        const CurlHandle handle{curl_easy_init()};
        if (!handle) {
            return HttpResponse{};
        }
        CurlSlist headers{};
        headers = append_header(std::move(headers), "Content-Type: application/json");
        const std::string auth = "Authorization: Bearer " + token;
        headers = append_header(std::move(headers), auth.c_str());

        // S3: sign ingest POSTs at send time over the raw body. Fail-soft — if the
        // header cannot be built we send unsigned (the server accepts unsigned during
        // the signing rollout). Computed here, on the worker thread, off the game path.
        std::string signature_header;
        if (sign) {
            try {
                const auto unix_seconds = static_cast<std::int64_t>(std::time(nullptr));
                signature_header =
                    "X-Tombstone-Signature: " +
                    build_ingest_signature_header(token, body, unix_seconds);
                headers = append_header(std::move(headers), signature_header.c_str());
            } catch (...) {
                sdk_log_.debug("request signing failed; sending unsigned");
            }
        }

        curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_POST, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(body.size()));
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, timeout_seconds);
        return perform(handle.get(), sdk_log_);
    } catch (...) {
        return HttpResponse{};
    }
}

HttpResponse Transport::put_text(const std::string &url, const std::vector<char> &bytes,
                                 long timeout_seconds) {
    try {
        const CurlHandle handle{curl_easy_init()};
        if (!handle) {
            return HttpResponse{};
        }
        CurlSlist headers{};
        headers = append_header(std::move(headers), "Content-Type: text/plain");
        // No Authorization header: presigned URLs are self-authorizing and the
        // game token must never reach the storage host.

        curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_CUSTOMREQUEST, "PUT");
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, bytes.data());
        curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(bytes.size()));
        curl_easy_setopt(handle.get(), CURLOPT_HTTPHEADER, headers.get());
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, timeout_seconds);
        return perform(handle.get(), sdk_log_);
    } catch (...) {
        return HttpResponse{};
    }
}

}  // namespace tombstone
