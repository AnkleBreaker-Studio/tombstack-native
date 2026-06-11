#include <tombstone/tombstone.h>

#include "client.h"

#include <memory>
#include <mutex>

namespace {

constexpr const char *sdk_version = "0.5.0";

// The process-wide client. Entry points snapshot the shared_ptr under the
// mutex and call outside it, so a long flush() neither blocks other calls nor
// races a concurrent shutdown (the instance stays alive until the call ends).
std::mutex g_client_mutex;
std::shared_ptr<tombstone::Client> g_client;

std::shared_ptr<tombstone::Client> client_snapshot() {
    const std::lock_guard<std::mutex> lock(g_client_mutex);
    return g_client;
}

/** Run `action` against the live client with the ABI no-throw guarantee. */
template <typename Action>
tombstone_result with_client(Action &&action) noexcept {
    try {
        const std::shared_ptr<tombstone::Client> client = client_snapshot();
        if (!client) {
            return TOMBSTONE_ERROR_NOT_INITIALIZED;
        }
        return action(*client);
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

}  // namespace

extern "C" {

tombstone_result tombstone_options_init(tombstone_options *options) {
    if (options == nullptr) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    options->endpoint = nullptr;
    options->token = nullptr;
    options->build_version = nullptr;
    options->data_dir = nullptr;
    options->heartbeat_interval_s = 0;  // 0 -> default (60s)
    options->enable_heartbeats = 1;
    options->enable_session_log = 1;
    options->enable_unclean_shutdown_detection = 1;
    options->consent_granted = 1;
    options->enable_rtt_metric = 1;
    options->log_callback = nullptr;
    options->log_callback_user_data = nullptr;
    return TOMBSTONE_OK;
}

tombstone_result tombstone_init(const tombstone_options *options) {
    try {
        if (options == nullptr) {
            return TOMBSTONE_ERROR_INVALID_ARGUMENT;
        }
        std::shared_ptr<tombstone::Client> fresh;
        {
            const std::lock_guard<std::mutex> lock(g_client_mutex);
            if (g_client) {
                return TOMBSTONE_ERROR_ALREADY_INITIALIZED;
            }
            fresh = std::make_shared<tombstone::Client>();
            const tombstone_result result = fresh->init(*options);
            if (result != TOMBSTONE_OK) {
                return result;  // fresh is destroyed; nothing was published
            }
            g_client = std::move(fresh);
        }
        return TOMBSTONE_OK;
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

tombstone_result tombstone_shutdown(void) {
    try {
        std::shared_ptr<tombstone::Client> retired;
        {
            const std::lock_guard<std::mutex> lock(g_client_mutex);
            retired.swap(g_client);
        }
        if (!retired) {
            return TOMBSTONE_OK;  // idempotent
        }
        retired.reset();  // ~Client: flush log, persist queue, remove marker
        return TOMBSTONE_OK;
    } catch (...) {
        return TOMBSTONE_ERROR_INTERNAL;
    }
}

tombstone_result tombstone_set_user(const char *user_id, const char *steam_id) {
    return with_client(
        [&](tombstone::Client &client) { return client.set_user(user_id, steam_id); });
}

tombstone_result tombstone_set_consent(int granted) {
    return with_client(
        [&](tombstone::Client &client) { return client.set_consent(granted != 0); });
}

void tombstone_set_match_context(tombstone_handle * /*handle*/, const char *server_id,
                                 const char *match_id) {
    (void)with_client([&](tombstone::Client &client) {
        return client.set_match_context(server_id, match_id);
    });
}

tombstone_result tombstone_start_match(tombstone_handle * /*handle*/, char *out_match_id,
                                       size_t out_cap) {
    return with_client([&](tombstone::Client &client) {
        return client.start_match(out_match_id, out_cap);
    });
}

void tombstone_end_match(tombstone_handle * /*handle*/) {
    (void)with_client([&](tombstone::Client &client) { return client.end_match(); });
}

tombstone_result tombstone_add_breadcrumb(tombstone_level level, const char *message) {
    return with_client(
        [&](tombstone::Client &client) { return client.add_breadcrumb(level, message); });
}

tombstone_result tombstone_track_event(const char *name, const char *const *keys,
                                       const char *const *values, size_t count) {
    return with_client([&](tombstone::Client &client) {
        return client.track_event(name, keys, values, count);
    });
}

void tombstone_track_metric(tombstone_handle * /*handle*/, const char *name, double value,
                            const char *unit) {
    (void)with_client([&](tombstone::Client &client) {
        return client.track_metric(name, value, unit);
    });
}

tombstone_result tombstone_report_crash(const char *signature, const char *stack_hint,
                                        const char *stack_trace, int attach_log) {
    return with_client([&](tombstone::Client &client) {
        return client.report_crash(signature, stack_hint, stack_trace, attach_log != 0);
    });
}

tombstone_result tombstone_report_bug(const char *category, const char *message,
                                      int attach_log) {
    return with_client([&](tombstone::Client &client) {
        return client.report_bug(category, message, attach_log != 0);
    });
}

tombstone_result tombstone_log_line(tombstone_level level, const char *line) {
    return with_client([&](tombstone::Client &client) { return client.log_line(level, line); });
}

void tombstone_request_player_logs(tombstone_handle * /*handle*/, const char *target_type,
                                   const char *target_value, const char *reason) {
    (void)with_client([&](tombstone::Client &client) {
        return client.request_player_logs(target_type, target_value, reason);
    });
}

void tombstone_on_anomalous_disconnect(tombstone_handle * /*handle*/, const char *user_id,
                                       const char *reason) {
    (void)with_client([&](tombstone::Client &client) {
        const char *effective_reason =
            (reason != nullptr && reason[0] != '\0') ? reason : "anomalous disconnect";
        return client.request_player_logs("userId", user_id, effective_reason);
    });
}

void tombstone_set_sample_rate(tombstone_handle * /*handle*/, const char *name, double rate) {
    (void)with_client([&](tombstone::Client &client) {
        client.set_sample_rate(name, rate);
        return TOMBSTONE_OK;
    });
}

void tombstone_set_level(tombstone_handle * /*handle*/, const char *level_name) {
    (void)with_client([&](tombstone::Client &client) { return client.set_level(level_name); });
}

tombstone_result tombstone_diagnostics(tombstone_handle * /*handle*/,
                                       tombstone_diagnostics_t *out) {
    if (out == nullptr) {
        return TOMBSTONE_ERROR_INVALID_ARGUMENT;
    }
    *out = tombstone_diagnostics_t{};   // zero the snapshot first (also covers the not-init path)
    out->last_flush_age_seconds = -1.0;  // "no flush yet" sentinel
    return with_client([&](tombstone::Client &client) { return client.diagnostics(out); });
}

tombstone_result tombstone_flush(int timeout_ms) {
    return with_client([&](tombstone::Client &client) { return client.flush(timeout_ms); });
}

const char *tombstone_version(void) { return sdk_version; }

}  // extern "C"
