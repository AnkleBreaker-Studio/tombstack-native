#ifndef TOMBSTONE_TESTS_FAKE_TRANSPORT_H
#define TOMBSTONE_TESTS_FAKE_TRANSPORT_H

// In-process stand-in for src/transport_curl.cpp, linked into tombstone_client_tests
// instead of it, so the worker, the client and the C ABI can be exercised without a
// network or libcurl. Records every request and answers with scripted responses.
// Thread-safe: the SDK's worker thread calls it while the test thread inspects it.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fake_transport {

struct Request {
    std::string url;
    std::string body;
    bool is_log_upload{false};
    bool signed_body{false};
    std::int64_t sign_unix_seconds{0};
};

struct Response {
    bool transport_error{false};
    long status{202};
    std::string body{R"({"success":true,"data":{}})"};
    std::string retry_after;
    std::string date;
    /** A black-holed network: the request hangs until Transport::abort_inflight()
     *  (or 30 s), then fails as a transport error, as an aborted libcurl transfer does. */
    bool hang_until_abort{false};
};

/** Forget every request and scripted response; the default answer becomes a plain 202. */
void reset();
/** Answer used when no scripted response matches. */
void set_default(Response response);
/** One-shot answer for the next request whose URL ends with `url_suffix` (FIFO per suffix). */
void push(std::string url_suffix, Response response);
/** Snapshot of every request made so far, in order. */
std::vector<Request> requests();
/** Number of requests whose URL ends with `url_suffix`. */
std::size_t count_suffix(std::string_view url_suffix);
/** Wait until at least `count` requests were made; false on timeout. */
bool wait_for_requests(std::size_t count, std::chrono::milliseconds timeout);

/** IMF-fixdate (`Thu, 26 Nov 2026 12:00:00 GMT`) for `unix_seconds`, as a server's Date. */
std::string http_date(std::int64_t unix_seconds);

}  // namespace fake_transport

#endif  // TOMBSTONE_TESTS_FAKE_TRANSPORT_H
