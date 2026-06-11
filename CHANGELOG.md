# Changelog

All notable changes to the Tombstone Native SDK.

## [0.2.0] - 2026-06-11

### Added

- **Multiplayer correlation context**: three new C ABI calls —
  `tombstone_set_match_context(handle, server_id, match_id)`,
  `tombstone_start_match(handle, out_match_id, out_cap)` (mints a 32-char match
  id, marks `role="server"`), and `tombstone_end_match(handle)` (clears the
  cached match id; role + server id are retained). The cached context is
  mutex-guarded like the rest of `Client` state and never throws across the ABI.
- **Correlation stamping on the wire**: crash, bug-report, and event bodies now
  carry the optional `role` / `serverId` / `matchId` / `sessionId` dimensions
  (keys identical to the server Zod schema and the Unity SDK). Empty dimensions
  are omitted per the existing optional-omission rule (never an empty-string
  enum), so a plain client body is byte-identical to the 0.1.x wire shape.
  `sessionId` is stamped from the live session id, making
  server<->match<->session linking exact. Byte-exact builder tests added in
  `tests/test_payloads.cpp`.

### Fixed

- `tombstone_version()` now reports the real SDK version (was pinned to "0.1.0").

## [0.1.1] - 2026-06-10

### Fixed

- **Breadcrumb purge on consent revoke**: `tombstone_set_consent(0)` now clears the
  buffered breadcrumb ring on a true→false transition (`Client::set_consent` calls
  the previously-unused `BreadcrumbRing::clear()`), matching the Unity SDK. A
  pre-revoke trail can no longer attach to a crash captured after consent is
  re-granted. Extended `tests/test_breadcrumb_ring.cpp` to cover clear-after-wrap
  reset and ring reuse.

## [0.1.0] - 2026-06-10

First public cut: capture-local telemetry client with full wire-protocol
parity with the Tombstone Unity SDK (no OS-level crash capture yet — that is
Phase 2 via a sentry-native/Crashpad fork).

### Added

- Pure C99 public API (`include/tombstone/tombstone.h`): init/shutdown,
  set_user, set_consent, add_breadcrumb, track_event, report_crash,
  report_bug, log_line, flush — every call returns `tombstone_result`,
  nothing throws across the ABI.
- Dependency-free JSON writer mirroring the server's Zod schemas byte-for-byte
  (optionals omitted, explicit `log` boolean, client-side length clamps,
  UTF-8-safe truncation).
- libcurl HTTPS transport: bearer-authenticated ingest POSTs + presigned S3
  log PUTs (no auth header on PUTs).
- Background worker thread: bounded drop-oldest queue, exponential backoff
  (2s→32s, 5 attempts), poison-4xx drop / 429+5xx retry classification.
- Offline sidecar queue (`data_dir/pending/{crashes,bug-reports,events}/*.json`,
  raw ingest bodies, uploader-compatible), drained on next init; write-ahead
  durability for crashes and bug reports.
- Breadcrumb ring (64 slots, preallocated, thread-safe).
- Per-signature crash dedupe (≤1 report/min, suppressed repeats become a
  counter breadcrumb).
- Rolling session log (512 KB cap, front-trim, `session.log` →
  `previous-session.log` rotation) with presigned upload on crash/bug reports.
- Unclean-shutdown detection (`session.lock` marker) with a synthetic
  `unclean-shutdown` crash report carrying the previous session's log.
- Session heartbeat timer thread (15–600 s interval, ephemeral delivery).
- Consent gate (GDPR/store policy): nothing recorded or sent while off.
- CMake build (shared + optional static, install rules, warnings-as-errors),
  C examples, dependency-free unit tests wired into CTest, GitHub Actions CI
  matrix (Windows MSVC / Linux GCC / macOS Clang).
