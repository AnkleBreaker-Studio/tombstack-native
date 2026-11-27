#ifndef TOMBSTONE_SRC_TRANSPORT_H
#define TOMBSTONE_SRC_TRANSPORT_H

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace tombstone {

class SdkLog;

/** Outcome of one HTTP exchange. */
struct HttpResponse {
    /** True when no HTTP status exists (DNS failure, TLS error, timeout...). */
    bool transport_error{true};
    long status{0};
    std::string body;
    /** Raw value of the response's Retry-After header ("" when absent). The
     *  worker parses the seconds form on 429/503 and raises its backoff to it
     *  (capped — see retry_after.h). Untrimmed; the parser tolerates OWS/CRLF. */
    std::string retry_after;
    /** Raw value of the response's Date header ("" when absent). The worker
     *  learns the server's clock from it (server_clock.h) so signed requests
     *  carry a timestamp the server accepts. Untrimmed. */
    std::string date;
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

    /**
     * POST a JSON body with `Authorization: Bearer <token>`. When `sign` is set
     * (ingest endpoints only — never pull/editor), an `X-Tombstack-Signature`
     * header is computed at send time over the raw body keyed by `token` (S3),
     * stamped with `sign_unix_seconds`: the caller passes its server-corrected
     * clock (ServerClock), never the raw device clock. Signing is fail-soft:
     * any signing error sends the request unsigned (the server accepts
     * unsigned ingest during the rollout).
     */
    HttpResponse post_json(const std::string &url, const std::string &token,
                           const std::string &body, long timeout_seconds, bool sign,
                           std::int64_t sign_unix_seconds);

    /**
     * PUT raw text to a presigned URL (session-log upload). Sends
     * `Content-Type: text/plain` and deliberately NO Authorization header —
     * the game token must never leak to the storage host.
     */
    HttpResponse put_text(const std::string &url, const std::vector<char> &bytes,
                          long timeout_seconds);

    /** Append signed policy fields before the file; never send the ingest token to storage. */
    HttpResponse post_log(const std::string &url,
                          const std::vector<std::pair<std::string, std::string>> &fields,
                          const std::vector<char> &bytes, long timeout_seconds);

    /**
     * Abort the request in progress, and fail any later one at once (shutdown):
     * quitting the game must never wait out a network timeout. One-way and
     * thread-safe; the aborted request comes back as a transport error.
     */
    void abort_inflight() noexcept;

    /** Connection setup (DNS, TCP, TLS) gives up after this long. */
    static constexpr long connect_timeout_seconds = 5;

private:
    SdkLog &sdk_log_;
    std::atomic<bool> aborting_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_TRANSPORT_H
