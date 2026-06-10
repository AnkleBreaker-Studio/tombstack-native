/*
 * Tombstone Native SDK — minimal integration (pure C99).
 *
 * Build: part of the CMake tree (TOMBSTONE_BUILD_EXAMPLES=ON).
 * Run:   TOMBSTONE_ENDPOINT / TOMBSTONE_TOKEN env vars select a real tenant;
 *        without them the demo uses placeholders (uploads will fail offline
 *        and land in the sidecar queue — which is also fine to observe).
 */
#include <tombstone/tombstone.h>

#include <stdio.h>
#include <stdlib.h>

static void on_sdk_log(int level, const char *message, void *user_data) {
    (void)user_data;
    /* The SDK itself never prints; this callback is the only diagnostics tap. */
    printf("[tombstone:%d] %s\n", level, message);
}

int main(void) {
    const char *endpoint = getenv("TOMBSTONE_ENDPOINT");
    const char *token = getenv("TOMBSTONE_TOKEN");

    tombstone_options options;
    tombstone_options_init(&options);
    options.endpoint = endpoint != NULL ? endpoint : "https://tombstone.invalid";
    options.token = token != NULL ? token : "tmb_demo_token";
    options.build_version = "1.0.0-example";
    options.log_callback = on_sdk_log;

    tombstone_result result = tombstone_init(&options);
    if (result != TOMBSTONE_OK) {
        fprintf(stderr, "tombstone_init failed: %d\n", (int)result);
        return 1;
    }
    printf("SDK %s initialized\n", tombstone_version());

    tombstone_set_user("player-42", NULL);

    /* Breadcrumbs ride along with future crash and bug reports. */
    tombstone_add_breadcrumb(TOMBSTONE_LEVEL_INFO, "main menu loaded");
    tombstone_log_line(TOMBSTONE_LEVEL_INFO, "starting level 3");

    /* A named analytics event with flat string attributes. */
    {
        const char *keys[] = {"level", "difficulty"};
        const char *values[] = {"3", "hard"};
        tombstone_track_event("level_start", keys, values, 2);
    }

    /* Report a crash the engine caught (signature NULL -> derived). */
    tombstone_report_crash(NULL, "NullReference in Enemy::Update",
                           "Enemy::Update()\nWorld::Tick()\nmain()",
                           /*attach_log=*/1);

    /* Give the background worker a moment to deliver, then shut down. */
    result = tombstone_flush(5000);
    printf("flush: %s\n", result == TOMBSTONE_OK ? "drained" : "pending (offline?)");

    tombstone_shutdown();
    return 0;
}
