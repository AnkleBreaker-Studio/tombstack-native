#ifndef TOMBSTONE_SRC_PREINIT_H
#define TOMBSTONE_SRC_PREINIT_H

#include <cstddef>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace tombstone {

/**
 * Bounded pre-init capture buffer (Unity 0.9.8 parity): before tombstone_init
 * the public API BUFFERS instead of failing silent, and tombstone_init replays
 * the store in order AFTER the options are applied — so an explicit pre-init
 * Set* beats the matching init option (the Unity "clobber fix": a pre-init
 * tombstone_set_environment must not be clobbered by options.environment).
 *
 * Bounds: breadcrumbs are drop-OLDEST (a trail's most recent entries are the
 * valuable ones); events + metrics share one drop-NEWEST buffer (matching the
 * Unity pre-init track buffer); identity / environment / metadata / consent
 * are last-write-wins; the start-session request is a one-way latch.
 *
 * Pure and unsynchronized by design: the C ABI layer serializes every access
 * under its own mutex, and the unit tests drive the store directly.
 */
class PreInitStore {
public:
    static constexpr std::size_t max_breadcrumbs = 64;  // drop-oldest
    static constexpr std::size_t max_tracks = 128;      // events+metrics combined, drop-newest

    struct Crumb {
        int level{0};  // a tombstone_level value
        std::string message;
    };

    /** One buffered track_event / track_metric call, replayed in insertion
     *  order (their relative order is preserved across the two kinds). */
    struct Track {
        bool is_metric{false};
        std::string name;
        std::vector<std::pair<std::string, std::string>> attributes;  // events only
        double value{0.0};                                            // metrics only
        std::string unit;                                             // metrics only
    };

    /** Buffer one breadcrumb; the OLDEST entry is dropped at capacity. */
    void add_breadcrumb(int level, std::string message);

    /** Buffer one event/metric. Returns false (dropped, drop-newest) once the
     *  combined cap is reached — a pre-init burst must not grow unbounded. */
    bool add_event(std::string name, std::vector<std::pair<std::string, std::string>> attributes);
    bool add_metric(std::string name, double value, std::string unit);

    // Last-write-wins state, replayed BEFORE the capture replay so replayed
    // items are stamped (environment/user) and gated (consent) correctly.
    void set_user(std::string user_id, std::string steam_id);
    void set_environment(std::string environment);
    void set_user_metadata(std::vector<std::pair<std::string, std::string>> entries);
    void set_consent(bool granted);

    /** Remember a pre-init tombstone_start_session(); the latch survives into
     *  init even when options.auto_start_session is 0. */
    void request_start_session();

    const std::deque<Crumb> &breadcrumbs() const noexcept { return breadcrumbs_; }
    const std::deque<Track> &tracks() const noexcept { return tracks_; }
    bool has_user() const noexcept { return has_user_; }
    const std::string &user_id() const noexcept { return user_id_; }
    const std::string &steam_id() const noexcept { return steam_id_; }
    bool has_environment() const noexcept { return has_environment_; }
    const std::string &environment() const noexcept { return environment_; }
    bool has_user_metadata() const noexcept { return has_user_metadata_; }
    const std::vector<std::pair<std::string, std::string>> &user_metadata() const noexcept {
        return user_metadata_;
    }
    bool has_consent() const noexcept { return has_consent_; }
    bool consent() const noexcept { return consent_; }
    bool start_session_requested() const noexcept { return start_session_requested_; }

    /** Drop everything (called once the store has been replayed into a client). */
    void clear();

private:
    std::deque<Crumb> breadcrumbs_;
    std::deque<Track> tracks_;
    bool has_user_{false};
    std::string user_id_;
    std::string steam_id_;
    bool has_environment_{false};
    std::string environment_;
    bool has_user_metadata_{false};
    std::vector<std::pair<std::string, std::string>> user_metadata_;
    bool has_consent_{false};
    bool consent_{true};
    bool start_session_requested_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_PREINIT_H
