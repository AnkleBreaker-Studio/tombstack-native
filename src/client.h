#ifndef TOMBSTONE_SRC_CLIENT_H
#define TOMBSTONE_SRC_CLIENT_H

#include <tombstone/tombstone.h>

#include "breadcrumb_ring.h"
#include "dedupe.h"
#include "sdk_log.h"
#include "session_log.h"
#include "session_marker.h"
#include "sidecar_queue.h"
#include "worker.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace tombstone {

class Transport;

/**
 * The process-wide SDK instance behind the C ABI. Owns every subsystem and
 * all configuration copies. Lifecycle: construct -> init() once -> capture
 * calls from any thread -> destructor (clean shutdown). The C layer maps one
 * tombstone_init/tombstone_shutdown pair to one Client instance, so
 * use-after-shutdown can never reach a half-dead object.
 */
class Client {
public:
    Client() = default;
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;
    Client(Client &&) = delete;
    Client &operator=(Client &&) = delete;

    tombstone_result init(const tombstone_options &options);

    tombstone_result set_user(const char *user_id, const char *steam_id);
    tombstone_result set_consent(bool granted);
    tombstone_result add_breadcrumb(tombstone_level level, const char *message);
    tombstone_result track_event(const char *name, const char *const *keys,
                                 const char *const *values, std::size_t count);
    tombstone_result report_crash(const char *signature, const char *stack_hint,
                                  const char *stack_trace, bool attach_log);
    tombstone_result report_bug(const char *category, const char *message, bool attach_log);
    tombstone_result log_line(tombstone_level level, const char *line);
    tombstone_result flush(int timeout_ms);

private:
    // --- lifecycle helpers (client.cpp) ---
    void configure_storage();
    void drain_sidecars();
    void start_session_tracking();
    void report_unclean_shutdown(const SessionMarkerData &previous);
    void heartbeat_loop();
    void stop_heartbeat();

    // --- capture helpers (client_reports.cpp) ---
    bool capture_allowed() const noexcept { return initialized_ && consent_; }
    void record_breadcrumb(std::string_view level, std::string_view message);
    void enqueue_ingest(const char *path, std::string body, Durability durability,
                        SidecarKind kind, bool request_log, bool log_from_previous);
    std::string current_user_id() const;
    std::string current_steam_id() const;

    // diagnostics + storage (order matters: sdk_log_ first, it is referenced)
    SdkLog sdk_log_;
    SessionLog session_log_{sdk_log_};
    SessionMarker marker_{sdk_log_};
    SidecarQueue sidecars_{sdk_log_};
    BreadcrumbRing breadcrumbs_;
    DedupeWindow dedupe_;

    // transport + delivery (created during init; Worker needs the token)
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<Worker> worker_;

    // configuration copies
    std::string endpoint_;
    std::string token_;
    std::string build_version_;
    std::string os_;
    std::string arch_;
    std::string session_id_;
    std::filesystem::path data_dir_;
    std::chrono::seconds heartbeat_interval_{60};
    bool heartbeats_enabled_{true};
    bool session_log_enabled_{true};
    bool unclean_detection_enabled_{true};
    bool storage_available_{false};

    // state
    std::atomic<bool> initialized_{false};
    std::atomic<bool> consent_{true};
    std::mutex session_tracking_mutex_;
    bool session_tracking_started_{false};
    std::optional<SessionMarkerData> previous_marker_;
    bool had_previous_log_{false};
    bool has_restored_crash_{false};

    mutable std::mutex user_mutex_;
    std::string user_id_;
    std::string steam_id_;

    // heartbeat timer thread
    std::thread heartbeat_thread_;
    std::mutex heartbeat_mutex_;
    std::condition_variable heartbeat_cv_;
    bool heartbeat_stop_{false};
};

}  // namespace tombstone

#endif  // TOMBSTONE_SRC_CLIENT_H
