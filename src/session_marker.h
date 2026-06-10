#ifndef TOMBSTONE_SRC_SESSION_MARKER_H
#define TOMBSTONE_SRC_SESSION_MARKER_H

#include <filesystem>
#include <optional>
#include <string>

namespace tombstone {

class SdkLog;

/** Contents of `<data_dir>/session.lock` (the dirty-session marker). */
struct SessionMarkerData {
    std::string session_id;
    std::string started_at_iso;
    std::string build_version;
    std::string os;
    std::string arch;
};

/**
 * Unclean-shutdown ("crashed last run") marker. Written at session start,
 * deleted on clean shutdown — a marker found at the NEXT init means the
 * previous session died hard (crash, OOM kill, force quit). Every filesystem
 * op is wrapped: a locked or missing file degrades to "no marker", never to
 * an exception in game code.
 */
class SessionMarker {
public:
    explicit SessionMarker(SdkLog &sdk_log) noexcept;

    void configure(const std::filesystem::path &data_dir);

    /** Read AND delete the previous run's marker; nullopt = clean (or unknown). */
    std::optional<SessionMarkerData> take_previous();

    /** Write this session's marker (start of the detection window). */
    void write(const SessionMarkerData &data);

    /** Delete the marker — clean shutdown, or clearing a stale one when detection is off. */
    void remove();

private:
    SdkLog &sdk_log_;
    std::filesystem::path marker_path_;
    bool configured_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_SESSION_MARKER_H
