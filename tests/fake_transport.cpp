#include "fake_transport.h"

#include "clock.h"
#include "sdk_log.h"
#include "transport.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <utility>

namespace fake_transport {

namespace {

struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<Request> requests;
    std::map<std::string, std::deque<Response>> scripted;
    Response fallback;
};

State &state() {
    static State instance;
    return instance;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           value.substr(value.size() - suffix.size()) == suffix;
}

/** Record `request` and return the scripted (else default) answer for its URL. */
Response answer(Request request) {
    State &s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    Response response = s.fallback;
    for (auto &[suffix, queue] : s.scripted) {
        if (!queue.empty() && ends_with(request.url, suffix)) {
            response = queue.front();
            queue.pop_front();
            break;
        }
    }
    s.requests.push_back(std::move(request));
    lock.unlock();
    s.changed.notify_all();
    return response;
}

std::mutex g_abort_mutex;
std::condition_variable g_abort_cv;

/** Hang like a black-holed request until `aborting` is raised (bounded), then fail. */
tombstone::HttpResponse hang_until_abort(const std::atomic<bool> &aborting) {
    std::unique_lock<std::mutex> lock(g_abort_mutex);
    g_abort_cv.wait_for(lock, std::chrono::seconds{30}, [&] { return aborting.load(); });
    return tombstone::HttpResponse{};  // transport_error = true
}

tombstone::HttpResponse to_http(const Response &response) {
    tombstone::HttpResponse out;
    out.transport_error = response.transport_error;
    out.status = response.transport_error ? 0 : response.status;
    out.body = response.transport_error ? std::string{} : response.body;
    out.retry_after = response.retry_after;
    out.date = response.date;
    return out;
}

}  // namespace

void reset() {
    State &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.requests.clear();
    s.scripted.clear();
    s.fallback = Response{};
}

void set_default(Response response) {
    State &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.fallback = std::move(response);
}

void push(std::string url_suffix, Response response) {
    State &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    s.scripted[std::move(url_suffix)].push_back(std::move(response));
}

std::vector<Request> requests() {
    State &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    return s.requests;
}

std::size_t count_suffix(std::string_view url_suffix) {
    State &s = state();
    const std::lock_guard<std::mutex> lock(s.mutex);
    std::size_t count = 0;
    for (const Request &request : s.requests) {
        if (ends_with(request.url, url_suffix)) {
            ++count;
        }
    }
    return count;
}

bool wait_for_requests(std::size_t count, std::chrono::milliseconds timeout) {
    State &s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.changed.wait_for(lock, timeout, [&] { return s.requests.size() >= count; });
}

std::string http_date(std::int64_t unix_seconds) {
    static constexpr const char *days[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    static constexpr const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                             "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    // "YYYY-MM-DDTHH:MM:SS.mmmZ" -> "Www, DD Mon YYYY HH:MM:SS GMT" (1970-01-01 was a Thursday).
    const std::string iso = tombstone::format_iso8601_utc_ms(unix_seconds * 1000);
    const std::int64_t day_index = unix_seconds / 86400;
    const int month = std::stoi(iso.substr(5, 2));
    return std::string{days[day_index % 7]} + ", " + iso.substr(8, 2) + " " + months[month - 1] +
           " " + iso.substr(0, 4) + " " + iso.substr(11, 8) + " GMT";
}

}  // namespace fake_transport

namespace tombstone {

Transport::Transport(SdkLog &sdk_log) : sdk_log_(sdk_log) {}

Transport::~Transport() = default;

void Transport::abort_inflight() noexcept {
    aborting_.store(true);
    try {
        const std::lock_guard<std::mutex> lock(fake_transport::g_abort_mutex);
    } catch (...) {
    }
    fake_transport::g_abort_cv.notify_all();
}

HttpResponse Transport::post_json(const std::string &url, const std::string & /*token*/,
                                  const std::string &body, long /*timeout_seconds*/, bool sign,
                                  std::int64_t sign_unix_seconds) {
    sdk_log_.debug("fake transport: POST " + url);  // keeps the shared member in use (clang)
    if (aborting_.load()) {
        return HttpResponse{};  // shutting down: fail at once, like the libcurl transport
    }
    fake_transport::Request request;
    request.url = url;
    request.body = body;
    request.signed_body = sign;
    request.sign_unix_seconds = sign_unix_seconds;
    const fake_transport::Response response = fake_transport::answer(std::move(request));
    if (response.hang_until_abort) {
        return fake_transport::hang_until_abort(aborting_);
    }
    return fake_transport::to_http(response);
}

HttpResponse Transport::put_text(const std::string &url, const std::vector<char> &bytes,
                                 long /*timeout_seconds*/) {
    if (aborting_.load()) {
        return HttpResponse{};
    }
    fake_transport::Request request;
    request.url = url;
    request.body.assign(bytes.begin(), bytes.end());
    request.is_log_upload = true;
    return fake_transport::to_http(fake_transport::answer(std::move(request)));
}

HttpResponse Transport::post_log(const std::string &url,
                                 const std::vector<std::pair<std::string, std::string>> & /*fields*/,
                                 const std::vector<char> &bytes, long /*timeout_seconds*/) {
    if (aborting_.load()) {
        return HttpResponse{};
    }
    fake_transport::Request request;
    request.url = url;
    request.body.assign(bytes.begin(), bytes.end());
    request.is_log_upload = true;
    return fake_transport::to_http(fake_transport::answer(std::move(request)));
}

}  // namespace tombstone
