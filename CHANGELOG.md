# Changelog

All notable changes to the Tombstone Native SDK.

## [0.4.0] - 2026-06-11

### Added

- **Log-pull control plane** — two new C ABI calls:
  `tombstone_request_player_logs(handle, target_type, target_value, reason)`
  queues a server-side log pull, and
  `tombstone_on_anomalous_disconnect(handle, user_id, reason)` is the
  auto-pull-after-weird-disconnect convenience (forwards with
  `target_type="userId"`, defaulting `reason` to "anomalous disconnect"). Both go
  through the no-throw `with_client` wrapper and POST the create body
  `{ targetType, targetValue, reason }` (mirrors the server
  `pullRequestCreateSchema`, clamped 32/128/280) to `/api/v1/pull-requests` with
  the client token. The endpoint is WRITE-scoped server-side; a 403 for an
  ingest-only token is classified as poison and logged via the diagnostics
  callback — never fatal.
- **Heartbeat-ack honouring (client command channel)** — the heartbeat 202 ack
  now carries `data.pendingRequests`. The worker hands the 2xx body to the client
  via a new ack handler; the client parses `pendingRequests` (extended
  `json_scan` with `find_pending_requests`) and, for each request that targets
  THIS client (by userId/sessionId/matchId/serverId, matched by the pure
  `pull_request_targets_client` — a mirror of the server `heartbeatMatchesRequest`)
  AND only while consent is granted, POSTs
  `/api/v1/pull-requests/{requestId}/fulfill` with its asserted identity
  (`build_pull_fulfill_json`, empty dimensions omitted). The fulfil sets the
  request-log flag so the existing presigned-log chase uploads the current
  rolling session log off the worker thread; the time-sensitive fulfil is
  retried in-session but never persisted across launches. Backward-safe: a
  non-targeted or non-consented client uploads nothing, and the additive ack
  fields are ignored by older builds.
- Byte-exact builder + matcher unit tests (`build_pull_request_json`,
  `build_pull_fulfill_json`, `pull_request_targets_client`) in
  `tests/test_payloads.cpp` and ack-parse tests (`find_pending_requests`) in
  `tests/test_json_scan.cpp`, both wired into the existing CTest suites.

## [0.3.0] - 2026-06-11

### Added

- **`tombstone_track_metric(handle, name, value, unit)`**: a new C ABI call for
  numeric metric samples (tick rate, RTT, memory, ...). Routed through the
  no-throw `with_client` wrapper; stamps the cached correlation context
  (role/serverId/matchId/sessionId) plus buildVersion/os/arch/occurredAtIso onto
  each sample. `value` must be finite (NaN/Infinity is dropped at the boundary);
  `unit` is an optional short label, omitted when NULL/empty. The metric item
  JSON mirrors the server `metricSchema`
  (`{ name, value, unit?, occurredAtIso, buildVersion, os, arch, userId?, role?,
  serverId?, matchId?, sessionId? }`, `value` a JSON number, empty optionals
  omitted).
- **Event & metric batching (spec section 16)**: events and metrics now
  accumulate into a bounded, preallocated, drop-oldest buffer (cap 256) and are
  flushed as a `{ "sentAtIso", "items": [...] }` envelope when any trigger fires
  — count (50), age (10 s), or quit/pre-shutdown — and POSTed to
  `/api/v1/ingest/events:batch` / `/api/v1/ingest/metrics:batch`. Each item keeps
  its own `occurredAtIso`; only the envelope carries the flush time. Draining and
  sending happen on the existing worker thread (zero main-thread I/O, spec
  section 15); capture calls are an O(1) enqueue. Batch sends reuse the worker's
  exponential backoff and are fail-soft (dropped on terminal failure rather than
  mis-persisted as single-item sidecars). Crashes, bug reports, and heartbeats
  remain individual and write-ahead durable.
- New `Batch` buffer type with byte-exact builder tests (`build_metric_json`,
  `build_batch_envelope`) in `tests/test_payloads.cpp` and buffer-behavior tests
  (count/age/forced triggers, drop-oldest) in `tests/test_batch.cpp`, both wired
  into CTest.

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
