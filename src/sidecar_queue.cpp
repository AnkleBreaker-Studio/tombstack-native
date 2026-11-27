#include "sidecar_queue.h"

#include "sdk_log.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace tombstone {

namespace {

std::string random_hex8() {
    static std::mt19937_64 engine{std::random_device{}()};
    static std::mutex engine_mutex;
    std::uint64_t value = 0;
    {
        const std::lock_guard<std::mutex> lock(engine_mutex);
        value = engine();
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(8, '0');
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = digits[(value >> (i * 4U)) & 0xFU];
    }
    return out;
}

std::int64_t now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string unique_file_name() { return std::to_string(now_ms()) + "-" + random_hex8() + ".json"; }

/** A write interrupted this long ago (crash, full disk) left its temporary file behind. */
constexpr std::chrono::hours stale_temp_age{1};

bool stale_temp_file(const fs::directory_entry &entry) {
    std::error_code ec;
    const fs::file_time_type written = fs::last_write_time(entry.path(), ec);
    return !ec && fs::file_time_type::clock::now() - written > stale_temp_age;
}

}  // namespace

bool SidecarQueue::expired(std::string_view file_name, std::int64_t now_ms_value) noexcept {
    std::int64_t stamp = 0;
    std::size_t digits = 0;
    while (digits < file_name.size() && file_name[digits] >= '0' && file_name[digits] <= '9') {
        if (digits == 15) {
            return false;  // not a millisecond stamp this SDK writes
        }
        stamp = stamp * 10 + (file_name[digits] - '0');
        ++digits;
    }
    if (digits == 0 || digits >= file_name.size() || file_name[digits] != '-') {
        return false;
    }
    const auto max_age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_age).count();
    return now_ms_value - stamp > max_age_ms;
}

SidecarQueue::SidecarQueue(SdkLog &sdk_log) noexcept : sdk_log_(sdk_log) {}

void SidecarQueue::configure(const fs::path &data_dir) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_dir_ = data_dir / "pending";
    try {
        for (const SidecarKind kind :
             kAllSidecarKinds) {
            fs::create_directories(pending_dir_ / kind_dir_name(kind));
        }
        configured_ = true;
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"sidecar queue unavailable: "} + e.what());
    }
}

const char *SidecarQueue::kind_dir_name(SidecarKind kind) noexcept {
    switch (kind) {
    case SidecarKind::crash:
        return "crashes";
    case SidecarKind::bug_report:
        return "bug-reports";
    case SidecarKind::event_batch:
        return "events-batch";
    case SidecarKind::metric_batch:
        return "metrics-batch";
    case SidecarKind::event:
    default:
        return "events";
    }
}

fs::path SidecarQueue::kind_dir(SidecarKind kind) const {
    return pending_dir_ / kind_dir_name(kind);
}

std::size_t SidecarQueue::count_files(const fs::path &dir) const {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator{dir, ec}) {
        if (entry.is_regular_file(ec)) {
            ++count;
        }
    }
    return count;
}

fs::path SidecarQueue::write(SidecarKind kind, const std::string &body) {
    fs::path file;
    std::string problem;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            return {};
        }
        problem = write_locked(kind, body, file);
    }
    if (!problem.empty()) {
        // Outside the lock: the diagnostics callback must never run while it is held.
        sdk_log_.warn(problem);
        return {};
    }
    return file;
}

std::string SidecarQueue::write_locked(SidecarKind kind, const std::string &body, fs::path &out) {
    try {
        const fs::path dir = kind_dir(kind);
        if (count_files(dir) >= max_files_per_kind) {
            return "sidecar queue full; payload not persisted";
        }
        const fs::path file = dir / unique_file_name();
        fs::path temp = file;
        temp += ".tmp";  // never scanned: only a complete file is renamed to *.json
        std::error_code ec;
        {
            std::ofstream stream{temp, std::ios::binary | std::ios::trunc};
            if (!stream) {
                return "could not persist payload (open failed)";
            }
            stream.write(body.data(), static_cast<std::streamsize>(body.size()));
            stream.close();
            if (!stream) {
                fs::remove(temp, ec);
                return "could not persist payload (write failed)";
            }
        }
        fs::rename(temp, file, ec);
        if (ec) {
            fs::remove(temp, ec);
            return "could not persist payload (rename failed)";
        }
        out = file;
        return {};
    } catch (const std::exception &e) {
        return std::string{"could not persist payload: "} + e.what();
    }
}

void SidecarQueue::remove(const fs::path &path) noexcept {
    if (path.empty()) {
        return;
    }
    try {
        std::error_code ec;
        fs::remove(path, ec);
    } catch (...) {
        // best-effort; a leftover file retries next launch and the server
        // de-duplicates by ULID
    }
}

std::size_t SidecarQueue::pending_count() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        return 0;
    }
    std::size_t total = 0;
    for (const SidecarKind kind :
         kAllSidecarKinds) {
        total += count_files(kind_dir(kind));
    }
    return total;
}

void SidecarQueue::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        return;
    }
    for (const SidecarKind kind :
         kAllSidecarKinds) {
        try {
            std::vector<fs::path> files;
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator{kind_dir(kind), ec}) {
                if (entry.is_regular_file(ec)) {
                    files.push_back(entry.path());
                }
            }
            for (const fs::path &file : files) {
                fs::remove(file, ec);
            }
        } catch (const std::exception &e) {
            sdk_log_.warn(std::string{"could not delete pending payloads: "} + e.what());
        }
    }
}

std::vector<SidecarRecord> SidecarQueue::scan() {
    std::vector<SidecarRecord> out;
    std::size_t expired_count = 0;
    std::string problem;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!configured_) {
            return out;
        }
        const std::int64_t now = now_ms();
        for (const SidecarKind kind : kAllSidecarKinds) {
            try {
                scan_kind_locked(kind, now, out, expired_count);
            } catch (const std::exception &e) {
                problem = std::string{"sidecar scan failed: "} + e.what();
            }
        }
    }
    if (expired_count > 0) {
        sdk_log_.warn(std::to_string(expired_count) +
                      " pending payload(s) older than 90 days deleted unsent");
    }
    if (!problem.empty()) {
        sdk_log_.warn(problem);
    }
    return out;
}

void SidecarQueue::scan_kind_locked(SidecarKind kind, std::int64_t now, std::vector<SidecarRecord> &out,
                                    std::size_t &expired_count) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator{kind_dir(kind), ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const fs::path &path = entry.path();
        if (path.extension() == ".tmp") {
            if (stale_temp_file(entry)) {
                fs::remove(path, ec);  // an interrupted write: never a payload to send
            }
            continue;
        }
        if (path.extension() != ".json") {
            continue;
        }
        if (expired(path.filename().string(), now)) {
            fs::remove(path, ec);
            ++expired_count;
            continue;
        }
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            continue;
        }
        std::string body{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        if (!body.empty()) {
            out.push_back(SidecarRecord{kind, path, std::move(body)});
        }
    }
}

}  // namespace tombstone
