# Tombstone Native SDK

Standalone C/C++ crash & telemetry client for **any** game engine — Unreal,
Godot, custom engines, tools, servers. Ships as a shared library
(`tombstone.dll` / `libtombstone.so` / `libtombstone.dylib`) with a pure C99
header, so anything that can call C can use it.

It speaks the exact same wire protocol as the [Tombstone](https://github.com/AnkleBreaker-Studio)
Unity SDK: crash reports, player bug reports, analytics events, session
heartbeats, breadcrumb trails, rolling session logs, and unclean-shutdown
detection — all against your studio's Tombstone ingestion endpoint with a
per-game SDK token (`tmb_...`).

## Quick integration (10 lines of C)

```c
#include <tombstone/tombstone.h>

tombstone_options opt;
tombstone_options_init(&opt);
opt.endpoint = "https://your-tenant.example.com";
opt.token = "tmb_...";            /* per-game SDK token */
opt.build_version = "1.4.2";
tombstone_init(&opt);
tombstone_add_breadcrumb(TOMBSTONE_LEVEL_INFO, "boot complete");
tombstone_report_crash(NULL, "Access violation in Renderer", stack_text, 1);
```

Call `tombstone_flush(timeout_ms)` before exit and `tombstone_shutdown()` on
clean quit. That's the whole integration; everything else is automatic.

## Automatic vs manual

| Automatic (after `tombstone_init`) | Manual (one-line calls) |
|---|---|
| Session heartbeats (CCU / crash-rate denominator) | `tombstone_report_crash` — report a caught crash |
| Offline sidecar queue: failed payloads persist and drain on next init | `tombstone_report_bug` — player bug report |
| Unclean-shutdown detection (`session.lock` marker): a hard-killed previous run becomes a synthetic `unclean-shutdown` crash report with the previous session's log attached | `tombstone_track_event` — analytics event |
| Rolling session log (512 KB cap, rotates to `previous-session.log`) | `tombstone_log_line` — mirror your engine's log hook |
| Per-signature crash dedupe (≤ 1 report/min; repeats become a counter breadcrumb) | `tombstone_add_breadcrumb` — manual trail marker |
| Exponential-backoff retries, write-ahead durability for crashes & bugs | `tombstone_set_user` / `tombstone_set_consent` |
| Deployment environment / fleet labels / device specs ride the relevant payloads once set | `tombstone_set_environment` — tag prod/staging/dev |
| Dedicated servers register in the Fleet even without matches | `tombstone_mark_dedicated_server` — server identity + fleet labels |
| Player-profile Hardware view (specs even on non-crashing sessions) | `tombstone_set_device` — hand the SDK your CPU/GPU/RAM/display |

Multiplayer & fleet: `tombstone_set_match_context` / `tombstone_start_match` / `tombstone_end_match`
correlate a match; `tombstone_mark_dedicated_server(server_id, region, hostname)` marks a dedicated
server (flips role to `server`) without needing a match; `tombstone_set_server_info(region,
hostname)` sets fleet labels. `tombstone_set_environment("staging")` tags every payload;
`tombstone_set_device(&specs)` attaches hardware specs (the SDK never probes — you hand it what
your engine already knows) to crashes, bug reports, and the session's first heartbeat.

Every call returns a `tombstone_result`; nothing ever throws across the ABI,
and the SDK never writes to stdout/stderr (wire a `log_callback` to see its
diagnostics).

## Using the DLL from any engine

- **Unreal**: wrap the header in a thin module (`THIRD_PARTY_INCLUDES_START`),
  add the import library to `PublicAdditionalLibraries`, call `tombstone_init`
  from your `GameInstance::Init` and hook `GLog` into `tombstone_log_line`.
- **Godot (GDExtension / C++ modules)**: link the shared library and call the
  C API directly; hook `OS::get_singleton()` logging.
- **Custom engines**: link, init, pipe your logger into `tombstone_log_line`,
  call `tombstone_report_crash` from your existing top-level exception filter.
- **C#/other runtimes**: standard P/Invoke / FFI against the C ABI.

See [docs/INTEGRATION.md](docs/INTEGRATION.md) for per-engine notes and
[docs/WIRE-PROTOCOL.md](docs/WIRE-PROTOCOL.md) for the exact JSON contract.

## Offline story: sidecars + uploader

Crash and bug payloads are **write-ahead**: persisted to
`<data_dir>/pending/{crashes,bug-reports,events}/*.json` *before* the first
upload attempt. Each file is the raw ingest body — no envelope — which makes
the queue compatible with the standalone uploader from the Tombstone monorepo:

```bash
TOMBSTONE_BASE_URL=https://host TOMBSTONE_TOKEN=tmb_... \
    node tools/uploader/upload.mjs <data_dir>/pending/crashes
```

The SDK also drains the queue itself on the next `tombstone_init`. Delivery
classification matches the uploader: 2xx delete, 429/5xx/network keep,
other 4xx drop (poison).

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Requires a C++17 compiler. libcurl is found via `find_package(CURL)` with a
pinned FetchContent fallback (Schannel on Windows, SecureTransport on macOS,
OpenSSL on Linux). Options: `TOMBSTONE_BUILD_STATIC`,
`TOMBSTONE_BUILD_EXAMPLES`, `TOMBSTONE_BUILD_TESTS`.

## Scope & roadmap

**v0.x reports crashes you hand it** (plus everything above). It deliberately
does **not** install signal/SEH handlers or write minidumps — async-signal-safe
minidump capture is not something to hand-roll. **Phase 2** adds OS-level
crash capture by forking/configuring **sentry-native (Crashpad backend)** per
the monorepo's locked plan (`docs/NATIVE-CAPTURE.md` there): the handler
writes a minidump + sidecar at crash time, and this SDK's existing sidecar
queue uploads on the next launch through the already-live presigned-S3 flow.

## License

MIT © 2026 AnkleBreaker Studio — see [LICENSE](LICENSE) and
[THIRD_PARTY.md](THIRD_PARTY.md).
