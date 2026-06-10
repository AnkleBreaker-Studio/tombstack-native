#include "session_log.h"

#include "sdk_log.h"

#include <fstream>

namespace fs = std::filesystem;

namespace tombstone {

namespace {
constexpr const char *current_log_name = "session.log";
constexpr const char *previous_log_name = "previous-session.log";
}  // namespace

SessionLog::SessionLog(SdkLog &sdk_log) noexcept : sdk_log_(sdk_log) {}

void SessionLog::configure(const fs::path &data_dir) {
    const std::lock_guard<std::mutex> lock(file_mutex_);
    current_path_ = data_dir / current_log_name;
    previous_path_ = data_dir / previous_log_name;
    configured_ = true;
}

bool SessionLog::rotate_for_new_session() {
    const std::lock_guard<std::mutex> lock(file_mutex_);
    if (!configured_) {
        return false;
    }
    try {
        std::error_code ec;
        fs::remove(previous_path_, ec);  // stale previous logs are deleted either way
        if (fs::exists(current_path_, ec)) {
            fs::rename(current_path_, previous_path_, ec);
            if (ec) {
                sdk_log_.warn("session log rotation failed: " + ec.message());
            }
        }
        file_bytes_ = 0;
        return fs::exists(previous_path_, ec);
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"session log rotation failed: "} + e.what());
        return false;
    }
}

void SessionLog::append(std::string_view ts_iso, std::string_view level,
                        std::string_view message) {
    if (message.empty()) {
        return;
    }
    const std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_.size() >= max_pending_bytes) {
        ++dropped_lines_;  // flush stalled — count, never grow unbounded
        return;
    }
    pending_.append(ts_iso);
    pending_ += " [";
    pending_.append(level);
    pending_ += "] ";
    pending_.append(message);
    if (message.back() != '\n') {
        pending_ += '\n';
    }
}

void SessionLog::flush_now() {
    const std::string chunk = take_pending();
    if (!chunk.empty()) {
        write_chunk(chunk);
    }
}

std::vector<char> SessionLog::read_current() {
    flush_now();
    const std::lock_guard<std::mutex> lock(file_mutex_);
    return read_file(current_path_);
}

std::vector<char> SessionLog::read_previous() {
    const std::lock_guard<std::mutex> lock(file_mutex_);
    return read_file(previous_path_);
}

std::string SessionLog::take_pending() {
    const std::lock_guard<std::mutex> lock(pending_mutex_);
    if (pending_.empty()) {
        return {};
    }
    if (dropped_lines_ > 0) {
        pending_ += "[tombstone] session log buffer overflow: " +
                    std::to_string(dropped_lines_) + " lines dropped\n";
        dropped_lines_ = 0;
    }
    std::string chunk;
    chunk.swap(pending_);
    return chunk;
}

void SessionLog::write_chunk(const std::string &chunk) {
    const std::lock_guard<std::mutex> lock(file_mutex_);
    if (!configured_) {
        return;
    }
    try {
        std::ofstream out{current_path_, std::ios::binary | std::ios::app};
        if (!out) {
            sdk_log_.warn("session log unavailable (cannot open for append)");
            return;
        }
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        out.close();
        file_bytes_ += chunk.size();
        if (file_bytes_ > max_log_bytes) {
            trim_from_front();
        }
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"session log write failed: "} + e.what());
    }
}

/** Keep the newest ~256 KB, aligned to a whole line. Rare (cap-crossing) path. */
void SessionLog::trim_from_front() {
    const std::vector<char> all = read_file(current_path_);
    if (all.size() <= trimmed_log_bytes) {
        file_bytes_ = all.size();
        return;
    }
    std::size_t start = all.size() - trimmed_log_bytes;
    while (start < all.size() && all[start] != '\n') {
        ++start;
    }
    if (start < all.size()) {
        ++start;
    }
    std::ofstream out{current_path_, std::ios::binary | std::ios::trunc};
    if (!out) {
        return;
    }
    out.write(all.data() + start, static_cast<std::streamsize>(all.size() - start));
    file_bytes_ = all.size() - start;
}

std::vector<char> SessionLog::read_file(const fs::path &path) {
    try {
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            return {};
        }
        return std::vector<char>{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"session log read failed: "} + e.what());
        return {};
    }
}

}  // namespace tombstone
