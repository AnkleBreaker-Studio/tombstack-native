# Changelog

All notable changes to the Tombstone Native SDK.

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
