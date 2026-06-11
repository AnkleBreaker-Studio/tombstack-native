#ifndef TOMBSTONE_SRC_SIDECAR_QUEUE_H
#define TOMBSTONE_SRC_SIDECAR_QUEUE_H

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace tombstone {

class SdkLog;

/** Which ingest endpoint a sidecar belongs to (one subdirectory each). */
enum class SidecarKind { crash, bug_report, event };

/** One restored sidecar: the raw ingest body plus its backing file. */
struct SidecarRecord {
    SidecarKind kind;
    std::filesystem::path path;
    std::string body;
};

/**
 * Offline durability queue. Each pending payload is one file whose content is
 * the RAW ingest body, segregated per endpoint:
 *
 *   <data_dir>/pending/crashes/<stamp>-<hex>.json
 *   <data_dir>/pending/bug-reports/<stamp>-<hex>.json
 *   <data_dir>/pending/events/<stamp>-<hex>.json
 *
 * This is exactly the format the standalone uploader consumes — pointing
 * `tools/uploader/upload.mjs` at `pending/crashes` posts the files unchanged.
 * The SDK itself drains all three directories on the next init. Bounded per
 * kind; fail-silent (a full disk degrades to "not persisted", never throws).
 */
class SidecarQueue {
public:
    static constexpr std::size_t max_files_per_kind = 64;

    explicit SidecarQueue(SdkLog &sdk_log) noexcept;

    /** Create the pending/ subdirectories. Must run before any other call. */
    void configure(const std::filesystem::path &data_dir);

    /** Subdirectory name for a kind ("crashes" | "bug-reports" | "events"). */
    static const char *kind_dir_name(SidecarKind kind) noexcept;

    /**
     * Persist one payload. Returns the backing file path, or an empty path
     * when persistence failed or the kind's directory is at capacity.
     */
    std::filesystem::path write(SidecarKind kind, const std::string &body);

    /** Delete a delivered (or poison) payload's backing file. Best-effort. */
    void remove(const std::filesystem::path &path) noexcept;

    /** All pending sidecars across the three kinds (offline drain at init). */
    std::vector<SidecarRecord> scan();

    /** Count of pending sidecar files across the three kinds (diagnostics, K3). */
    std::size_t pending_count();

private:
    std::filesystem::path kind_dir(SidecarKind kind) const;
    std::size_t count_files(const std::filesystem::path &dir) const;

    SdkLog &sdk_log_;
    std::mutex mutex_;
    std::filesystem::path pending_dir_;
    bool configured_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SIDECAR_QUEUE_H
