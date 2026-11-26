#include "transport.h"

#include "sdk_log.h"
#include "signature.h"

#include <curl/curl.h>

#include <atomic>
#include <cstdint>
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

/** The two response headers the SDK reads; everything else is ignored. */
struct HeaderCapture {
    std::string retry_after;
    std::string date;
};

/** Case-insensitive match of a raw header line against `lower_name` ("name:"). */
bool header_named(std::string_view line, std::string_view lower_name) noexcept {
    if (line.size() <= lower_name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lower_name.size(); ++i) {
        const char c = line[i];
        const char lower = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (lower != lower_name[i]) {
            return false;
        }
    }
    return true;
}

/** Minimal header capture: keep the Retry-After and Date values (the pure
 *  parsers handle OWS/CRLF trimming). An interim 100-continue response carries
 *  neither, so the final response's values win. */
extern "C" size_t tombstone_curl_header(char *data, size_t size, size_t count, void *out) {
    auto *capture = static_cast<HeaderCapture *>(out);
    const size_t total = size * count;
    try {
        const std::string_view line{data, total};
        constexpr std::string_view retry_after_name{"retry-after:"};
        constexpr std::string_view date_name{"date:"};
        if (header_named(line, retry_after_name)) {
            capture->retry_after.assign(line.substr(retry_after_name.size()));
        } else if (header_named(line, date_name)) {
            capture->date.assign(line.substr(date_name.size()));
        }
    } catch (...) {
        return 0;  // signal failure to curl
    }
    return total;
}

/** Progress callback: libcurl calls it frequently while a transfer runs (about
 *  once a second when idle, during connection setup too) and aborts the
 *  transfer when it returns nonzero — how shutdown cancels an in-flight request. */
extern "C" int tombstone_curl_progress(void *aborting, curl_off_t /*dltotal*/,
                                       curl_off_t /*dlnow*/, curl_off_t /*ultotal*/,
                                       curl_off_t /*ulnow*/) {
    return static_cast<const std::atomic<bool> *>(aborting)->load() ? 1 : 0;
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

HttpResponse perform(CURL *handle, SdkLog &sdk_log, std::atomic<bool> &aborting) {
    std::string body;
    HeaderCapture headers;
    if (aborting.load()) {
        return HttpResponse{};  // shutting down: fail at once (transport_error = true)
    }
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, tombstone_curl_write);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, tombstone_curl_header);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &headers);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L);
    // A black-holed network must not hold a request (or the quit) for the whole
    // transfer timeout: connection setup is bounded, and shutdown aborts the rest.
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, Transport::connect_timeout_seconds);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, tombstone_curl_progress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &aborting);
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
    response.retry_after = std::move(headers.retry_after);
    response.date = std::move(headers.date);
    return response;
}

}  // namespace

Transport::Transport(SdkLog &sdk_log) : sdk_log_(sdk_log) { curl_global_acquire(); }

Transport::~Transport() { curl_global_release(); }

void Transport::abort_inflight() noexcept { aborting_.store(true); }

HttpResponse Transport::post_json(const std::string &url, const std::string &token,
                                  const std::string &body, long timeout_seconds, bool sign,
                                  std::int64_t sign_unix_seconds) {
    try {
        const CurlHandle handle{curl_easy_init()};
        if (!handle) {
            return HttpResponse{};
        }
        CurlSlist headers{};
        headers = append_header(std::move(headers), "Content-Type: application/json");
        const std::string auth = "Authorization: Bearer " + token;
        headers = append_header(std::move(headers), auth.c_str());

        // S3: sign ingest POSTs at send time over the raw body, with the worker's
        // server-corrected clock. Fail-soft — if the header cannot be built we send
        // unsigned (the server accepts unsigned during the signing rollout).
        // Computed here, on the worker thread, off the game path.
        std::string signature_header;
        if (sign) {
            try {
                signature_header =
                    "X-Tombstack-Signature: " +
                    build_ingest_signature_header(token, body, sign_unix_seconds);
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
        return perform(handle.get(), sdk_log_, aborting_);
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
        return perform(handle.get(), sdk_log_, aborting_);
    } catch (...) {
        return HttpResponse{};
    }
}

HttpResponse Transport::post_log(const std::string &url,
                                 const std::vector<std::pair<std::string, std::string>> &fields,
                                 const std::vector<char> &bytes, long timeout_seconds) {
    try {
        const CurlHandle handle{curl_easy_init()};
        if (!handle || fields.empty()) return HttpResponse{};
        const std::unique_ptr<curl_mime, decltype(&curl_mime_free)> form{
            curl_mime_init(handle.get()), curl_mime_free};
        if (!form) return HttpResponse{};
        for (const auto &field : fields) {
            auto *part = curl_mime_addpart(form.get());
            if (!part || curl_mime_name(part, field.first.c_str()) != CURLE_OK ||
                curl_mime_data(part, field.second.data(), field.second.size()) != CURLE_OK)
                return HttpResponse{};
        }
        // S3 requires the file last; use an explicit size to preserve embedded NUL bytes.
        auto *file = curl_mime_addpart(form.get());
        if (!file || curl_mime_name(file, "file") != CURLE_OK ||
            curl_mime_filename(file, "session.log") != CURLE_OK ||
            curl_mime_type(file, "text/plain") != CURLE_OK ||
            curl_mime_data(file, bytes.data(), bytes.size()) != CURLE_OK) return HttpResponse{};
        curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
        curl_easy_setopt(handle.get(), CURLOPT_MIMEPOST, form.get());
        curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, timeout_seconds);
        return perform(handle.get(), sdk_log_, aborting_);
    } catch (...) {
        return HttpResponse{};
    }
}

}  // namespace tombstone
