#include "sdk_log.h"
#include "session_marker.h"
#include "test_framework.h"

#include <fstream>

namespace fs = std::filesystem;
using tombstone::SdkLog;
using tombstone::SessionMarker;
using tombstone::SessionMarkerData;

TEST_CASE("session_marker", "write then take round-trips every field") {
    ttest::TempDir dir;
    SdkLog log;
    SessionMarker marker{log};
    marker.configure(dir.path());

    SessionMarkerData data;
    data.session_id = "a1b2c3d4e5f6";
    data.started_at_iso = "2026-01-02T03:04:05.678Z";
    data.build_version = "1.4.2";
    data.os = "windows";
    data.arch = "x64";
    marker.write(data);
    CHECK(fs::exists(dir.path() / "session.lock"));

    const auto restored = marker.take_previous();
    CHECK(restored.has_value());
    CHECK_EQ(restored->session_id, data.session_id);
    CHECK_EQ(restored->started_at_iso, data.started_at_iso);
    CHECK_EQ(restored->build_version, data.build_version);
    CHECK_EQ(restored->os, data.os);
    CHECK_EQ(restored->arch, data.arch);
}

TEST_CASE("session_marker", "take consumes the marker (second take is empty)") {
    ttest::TempDir dir;
    SdkLog log;
    SessionMarker marker{log};
    marker.configure(dir.path());

    marker.write(SessionMarkerData{"sid", "t", "1.0.0", "linux", "x64"});
    CHECK(marker.take_previous().has_value());
    CHECK(!fs::exists(dir.path() / "session.lock"));
    CHECK(!marker.take_previous().has_value());
}

TEST_CASE("session_marker", "no marker means a clean previous session") {
    ttest::TempDir dir;
    SdkLog log;
    SessionMarker marker{log};
    marker.configure(dir.path());
    CHECK(!marker.take_previous().has_value());
}

TEST_CASE("session_marker", "remove deletes the marker (clean shutdown)") {
    ttest::TempDir dir;
    SdkLog log;
    SessionMarker marker{log};
    marker.configure(dir.path());

    marker.write(SessionMarkerData{"sid", "t", "1.0.0", "macos", "arm64"});
    CHECK(fs::exists(dir.path() / "session.lock"));
    marker.remove();
    CHECK(!fs::exists(dir.path() / "session.lock"));
}

TEST_CASE("session_marker", "garbage marker degrades to no marker") {
    ttest::TempDir dir;
    SdkLog log;
    SessionMarker marker{log};
    marker.configure(dir.path());
    {
        std::ofstream garbage{dir.path() / "session.lock", std::ios::binary};
        garbage << "not json at all";
    }
    CHECK(!marker.take_previous().has_value());
    // ...and the unreadable file was still consumed.
    CHECK(!fs::exists(dir.path() / "session.lock"));
}
