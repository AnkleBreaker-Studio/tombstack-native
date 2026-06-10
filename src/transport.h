#ifndef TOMBSTONE_SRC_TRANSPORT_H
#define TOMBSTONE_SRC_TRANSPORT_H

#include <string>
#include <vector>

namespace tombstone {

class SdkLog;

/** Outcome of one HTTP exchange. */
struct HttpResponse {
    /** True when no HTTP status exists (DNS failure, TLS error, timeout...). */
    bool transport_error{true};
    long status{0};
    std::string body;
};

/**
 * Blocking HTTPS transport over the libcurl easy API. One instance is shared
 * by the worker thread; each call uses its own curl handle, so the class is
 * stateless apart from curl's process-global init (reference counted).
 * Never throws across its boundary — failures come back as transport_error.
 */
class Transport {
public:
    explicit Transport(SdkLog &sdk_log);
    ~Transport();

    Transport(const Transport &) = delete;
    Transport &operator=(const Transport &) = delete;
    Transport(Transport &&) = delete;
    Transport &operator=(Transport &&) = delete;

    /** POST a JSON ingest body with `Authorization: Bearer <token>`. */
    HttpResponse post_json(const std::string &url, const std::string &token,
                           const std::string &body, long timeout_seconds);

    /**
     * PUT raw text to a presigned URL (session-log upload). Sends
     * `Content-Type: text/plain` and deliberately NO Authorization header —
     * the game token must never leak to the storage host.
     */
    HttpResponse put_text(const std::string &url, const std::vector<char> &bytes,
                          long timeout_seconds);

private:
    SdkLog &sdk_log_;
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_TRANSPORT_H
