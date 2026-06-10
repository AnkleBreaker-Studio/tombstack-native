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

std::string unique_file_name() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return std::to_string(ms) + "-" + random_hex8() + ".json";
}

}  // namespace

SidecarQueue::SidecarQueue(SdkLog &sdk_log) noexcept : sdk_log_(sdk_log) {}

void SidecarQueue::configure(const fs::path &data_dir) {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_dir_ = data_dir / "pending";
    try {
        for (const SidecarKind kind :
             {SidecarKind::crash, SidecarKind::bug_report, SidecarKind::event}) {
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
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!configured_) {
        return {};
    }
    try {
        const fs::path dir = kind_dir(kind);
        if (count_files(dir) >= max_files_per_kind) {
            sdk_log_.warn("sidecar queue full; payload not persisted");
            return {};
        }
        const fs::path file = dir / unique_file_name();
        std::ofstream out{file, std::ios::binary | std::ios::trunc};
        if (!out) {
            sdk_log_.warn("could not persist payload (open failed)");
            return {};
        }
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.close();
        return file;
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"could not persist payload: "} + e.what());
        return {};
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

std::vector<SidecarRecord> SidecarQueue::scan() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SidecarRecord> out;
    if (!configured_) {
        return out;
    }
    for (const SidecarKind kind :
         {SidecarKind::crash, SidecarKind::bug_report, SidecarKind::event}) {
        try {
            std::error_code ec;
            for (const auto &entry : fs::directory_iterator{kind_dir(kind), ec}) {
                if (!entry.is_regular_file(ec) || entry.path().extension() != ".json") {
                    continue;
                }
                std::ifstream in{entry.path(), std::ios::binary};
                if (!in) {
                    continue;
                }
                std::string body{std::istreambuf_iterator<char>{in},
                                 std::istreambuf_iterator<char>{}};
                if (!body.empty()) {
                    out.push_back(SidecarRecord{kind, entry.path(), std::move(body)});
                }
            }
        } catch (const std::exception &e) {
            sdk_log_.warn(std::string{"sidecar scan failed: "} + e.what());
        }
    }
    return out;
}

}  // namespace tombstone
