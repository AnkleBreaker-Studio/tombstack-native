#include "session_marker.h"

#include "json_scan.h"
#include "json_writer.h"
#include "sdk_log.h"

#include <fstream>

namespace fs = std::filesystem;

namespace tombstone {

namespace {
constexpr const char *marker_name = "session.lock";
}

SessionMarker::SessionMarker(SdkLog &sdk_log) noexcept : sdk_log_(sdk_log) {}

void SessionMarker::configure(const fs::path &data_dir) {
    marker_path_ = data_dir / marker_name;
    configured_ = true;
}

std::optional<SessionMarkerData> SessionMarker::take_previous() {
    if (!configured_) {
        return std::nullopt;
    }
    try {
        std::error_code ec;
        if (!fs::exists(marker_path_, ec)) {
            return std::nullopt;
        }
        std::string text;
        {
            std::ifstream in{marker_path_, std::ios::binary};
            if (!in) {
                return std::nullopt;
            }
            text.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
        }
        fs::remove(marker_path_, ec);

        SessionMarkerData data;
        data.session_id = find_string_field(text, "sessionId").value_or("");
        if (data.session_id.empty()) {
            return std::nullopt;
        }
        data.started_at_iso = find_string_field(text, "startedAtIso").value_or("");
        data.build_version = find_string_field(text, "buildVersion").value_or("");
        data.os = find_string_field(text, "os").value_or("");
        data.arch = find_string_field(text, "arch").value_or("");
        return data;
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"could not read previous session marker: "} + e.what());
        return std::nullopt;
    }
}

void SessionMarker::write(const SessionMarkerData &data) {
    if (!configured_) {
        return;
    }
    try {
        JsonWriter json;
        json.begin_object();
        json.string_field("sessionId", data.session_id);
        json.string_field("startedAtIso", data.started_at_iso);
        json.string_field("buildVersion", data.build_version);
        json.string_field("os", data.os);
        json.string_field("arch", data.arch);
        json.end_object();

        std::ofstream out{marker_path_, std::ios::binary | std::ios::trunc};
        if (!out) {
            sdk_log_.warn("could not write session marker (open failed)");
            return;
        }
        out << json.str();
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"could not write session marker: "} + e.what());
    }
}

void SessionMarker::remove() {
    if (!configured_) {
        return;
    }
    try {
        std::error_code ec;
        fs::remove(marker_path_, ec);
    } catch (const std::exception &e) {
        sdk_log_.warn(std::string{"could not clear session marker: "} + e.what());
    }
}

}  // namespace tombstone
