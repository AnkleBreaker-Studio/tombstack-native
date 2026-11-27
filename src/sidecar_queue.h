#ifndef TOMBSTONE_SRC_SIDECAR_QUEUE_H
#define TOMBSTONE_SRC_SIDECAR_QUEUE_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace tombstone {

class SdkLog;

/** Which ingest endpoint a sidecar belongs to (one subdirectory each). `event` is a
 *  single event (POST /ingest/events, written by older SDKs); `event_batch` and
 *  `metric_batch` are whole `{sentAtIso, items}` envelopes for the batch endpoints. */
enum class SidecarKind { crash, bug_report, event, event_batch, metric_batch };

/** Every kind, in drain order. */
inline constexpr SidecarKind kAllSidecarKinds[] = {SidecarKind::crash, SidecarKind::bug_report,
                                                   SidecarKind::event, SidecarKind::event_batch,
                                                   SidecarKind::metric_batch};

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
 *   <data_dir>/pending/events-batch/<stamp>-<hex>.json   (an events:batch envelope)
 *   <data_dir>/pending/metrics-batch/<stamp>-<hex>.json  (a metrics:batch envelope)
 *
 * This is exactly the format the standalone uploader consumes — pointing
 * `tools/uploader/upload.mjs` at `pending/crashes` posts the files unchanged.
 * The SDK itself drains every directory on the next init (with consent). Bounded
 * per kind; fail-silent (a full disk degrades to "not persisted", never throws).
 * A file is written under a temporary name and renamed into place, so a crash or
 * a full disk mid-write never leaves a truncated payload to send. A payload no
 * launch delivered within max_age is deleted unsent at the next scan.
 */
class SidecarQueue {
public:
    static constexpr std::size_t max_files_per_kind = 64;
    /** Unity and Godot SDK parity: a payload kept through refusals (a refused token,
     *  a spending cap) cannot stay on the device, retried at every launch, forever. */
    static constexpr std::chrono::hours max_age{24 * 90};

    /** A payload file named "<ms since epoch>-<hex>.json" older than max_age at
     *  `now_ms`. A name without that stamp never expires. Pure. */
    static bool expired(std::string_view file_name, std::int64_t now_ms) noexcept;

    explicit SidecarQueue(SdkLog &sdk_log) noexcept;

    /** Create the pending/ subdirectories. Must run before any other call. */
    void configure(const std::filesystem::path &data_dir);

    /** Subdirectory name for a kind ("crashes" | "bug-reports" | "events" |
     *  "events-batch" | "metrics-batch"). */
    static const char *kind_dir_name(SidecarKind kind) noexcept;

    /**
     * Persist one payload. Returns the backing file path, or an empty path
     * when persistence failed or the kind's directory is at capacity.
     */
    std::filesystem::path write(SidecarKind kind, const std::string &body);

    /** Delete a delivered (or poison) payload's backing file. Best-effort. */
    void remove(const std::filesystem::path &path) noexcept;

    /** All pending sidecars across every kind (offline drain at init). Payloads past
     *  max_age, and temporary files an interrupted write left, are deleted instead. */
    std::vector<SidecarRecord> scan();

    /** Delete every pending payload of every kind (consent revoked). Best-effort. */
    void clear();

    /** Count of pending sidecar files across every kind (diagnostics, K3). */
    std::size_t pending_count();

private:
    std::filesystem::path kind_dir(SidecarKind kind) const;
    std::size_t count_files(const std::filesystem::path &dir) const;
    /** Under mutex_: write `body` to a new file of `kind`. "" on success, else why not. */
    std::string write_locked(SidecarKind kind, const std::string &body, std::filesystem::path &out);
    /** Under mutex_: read the payloads of one kind into `out`; count what expired. */
    void scan_kind_locked(SidecarKind kind, std::int64_t now_ms, std::vector<SidecarRecord> &out,
                          std::size_t &expired_count);

    SdkLog &sdk_log_;
    std::mutex mutex_;
    std::filesystem::path pending_dir_;
    bool configured_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SIDECAR_QUEUE_H
