#include "preinit.h"

namespace tombstone {

void PreInitStore::add_breadcrumb(int level, std::string message) {
    if (breadcrumbs_.size() >= max_breadcrumbs) {
        breadcrumbs_.pop_front();  // drop-oldest: keep the most recent trail
    }
    breadcrumbs_.push_back(Crumb{level, std::move(message)});
}

bool PreInitStore::add_event(std::string name,
                             std::vector<std::pair<std::string, std::string>> attributes) {
    if (tracks_.size() >= max_tracks) {
        return false;  // drop-newest: the combined event+metric cap is hard
    }
    Track track;
    track.is_metric = false;
    track.name = std::move(name);
    track.attributes = std::move(attributes);
    tracks_.push_back(std::move(track));
    return true;
}

bool PreInitStore::add_metric(std::string name, double value, std::string unit) {
    if (tracks_.size() >= max_tracks) {
        return false;  // drop-newest, shared cap with events
    }
    Track track;
    track.is_metric = true;
    track.name = std::move(name);
    track.value = value;
    track.unit = std::move(unit);
    tracks_.push_back(std::move(track));
    return true;
}

void PreInitStore::set_user(std::string user_id, std::string steam_id) {
    has_user_ = true;
    user_id_ = std::move(user_id);
    steam_id_ = std::move(steam_id);
}

void PreInitStore::set_environment(std::string environment) {
    has_environment_ = true;
    environment_ = std::move(environment);
}

void PreInitStore::set_user_metadata(std::vector<std::pair<std::string, std::string>> entries) {
    has_user_metadata_ = true;  // an empty vector is a remembered CLEAR, not "unset"
    user_metadata_ = std::move(entries);
}

void PreInitStore::set_consent(bool granted) {
    has_consent_ = true;
    consent_ = granted;
}

void PreInitStore::request_start_session() { start_session_requested_ = true; }

void PreInitStore::clear() {
    breadcrumbs_.clear();
    tracks_.clear();
    has_user_ = false;
    user_id_.clear();
    steam_id_.clear();
    has_environment_ = false;
    environment_.clear();
    has_user_metadata_ = false;
    user_metadata_.clear();
    has_consent_ = false;
    consent_ = true;
    start_session_requested_ = false;
}

}  // namespace tombstone
