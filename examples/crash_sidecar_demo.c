/*
 * Tombstone Native SDK — offline sidecar demonstration (pure C99).
 *
 * Points the SDK at an unreachable endpoint on purpose: the write-ahead crash
 * report cannot be delivered, so it survives as a sidecar file:
 *
 *     <data_dir>/pending/crashes/<stamp>-<hex>.json
 *
 * The file content is the raw /api/v1/ingest/crashes body. Drain it later
 * with the SDK itself (next tombstone_init against a reachable endpoint) or
 * with the standalone uploader from the Tombstone monorepo:
 *
 *     TOMBSTONE_BASE_URL=https://host TOMBSTONE_TOKEN=tmb_... \
 *         node tools/uploader/upload.mjs <data_dir>/pending/crashes
 */
#include <tombstone/tombstone.h>

#include <stdio.h>

int main(void) {
    tombstone_options options;
    tombstone_options_init(&options);
    options.endpoint = "https://tombstone.invalid"; /* deliberately unreachable */
    options.token = "tmb_demo_token";
    options.build_version = "1.0.0-sidecar-demo";
    options.data_dir = "tombstone-demo-data";
    options.enable_heartbeats = 0; /* keep the demo quiet */

    if (tombstone_init(&options) != TOMBSTONE_OK) {
        fprintf(stderr, "tombstone_init failed\n");
        return 1;
    }

    tombstone_add_breadcrumb(TOMBSTONE_LEVEL_WARN, "physics solver diverging");
    tombstone_report_crash("demo-signature", "Access violation in PhysicsStep",
                           "PhysicsStep()\nFixedUpdate()", /*attach_log=*/0);

    /* Write-ahead durability: the sidecar exists BEFORE the first (doomed)
     * upload attempt — quit immediately and the report still survives. */
    tombstone_flush(1500);
    tombstone_shutdown();

    printf("Look in tombstone-demo-data/pending/crashes/ for the sidecar JSON.\n");
    printf("Re-run against a real endpoint (or use tools/uploader) to drain it.\n");
    return 0;
}
