# Changelog

All notable changes to the Tombstack Native SDK (the `tombstone_*` C ABI and
the `tombstone` library name are stable — Tombstack is the product name).

## [0.10.0] - 2026-11-27

> **Upgrade note:** no API or ABI change, so a 0.9.x integration builds and runs unchanged.
> What changes for a game that updates:
> - With `data_dir` unset, the game keeps its files in a folder of its own
>   (`Tombstone/game-<16 hex>` under the platform default). Reports a 0.9.x build left pending
>   in the old shared folder are not read or sent; the player identifier is derived again from
>   the same inputs, so it normally stays the same on the same machine.
> - A spending-cap stop (402), a refused SDK token or a 404 (usually a wrong `endpoint`) no
>   longer deletes crash and bug reports: they wait on disk and are sent once the server accepts
>   again. Nothing else is sent meanwhile. Only a report the server rejects as malformed or too
>   large (400, 413, 422) is deleted.
> - Withdrawing consent discards everything not yet delivered, and with consent off at init
>   nothing is read, stored or sent until it is granted.

### Fixed

- Games that leave `data_dir` unset no longer share one folder on a player's machine. Each game
  now keeps its pending reports, logs, crash dump and player identifier in a folder of its own
  under the platform default (`Tombstone/game-<16 hex>`), and only ever sends them with its own
  token. A `data_dir` you pass that is already used under another SDK token is not shared
  either: the SDK uses its own subfolder inside it. Files earlier versions left in the shared
  folder are not read or sent. The player identifier is derived again for the new folder and
  stays the same on the same machine; a game that changes its SDK token starts new ones.
- Withdrawing consent (`tombstone_set_consent(0)`) now discards everything captured but not yet
  delivered: queued and pending crash and bug reports (earlier sessions' included), buffered
  events and metrics, breadcrumbs, the session logs and the unclean-shutdown marker. Nothing
  captured before the withdrawal is sent afterwards, even if consent is granted again. Before,
  buffered and queued data kept being sent after the withdrawal.
- With consent off at init, reports left pending by earlier sessions now wait on disk until
  consent is granted. Before, they were sent at init.
- With consent off, the SDK no longer reads the machine identifier, stores the device-derived
  player identifier or installs the native crash handler: this now happens once consent is
  granted. Withdrawing consent deletes the stored identifier and stops crash dumps from being
  written.
- A `tombstone_set_consent(0)` made before `tombstone_init` now applies from the start of init.
  Before, init still sent the previous session's crash report and started heartbeats before the
  refusal took effect. Identity, environment and metadata set before init are likewise in place
  before the first heartbeat.
- Running two processes of the same game on one machine (a second instance, a launcher
  relaunch, a client next to a dedicated server) no longer files a false "unclean shutdown"
  for the first one, no longer sends its pending reports twice, and no longer leaves the first
  one's real crash undetected after the second quits. The first process owns the session
  tracking, session log and crash dump; the others send their own reports only.
- Events and metrics that could not be delivered were lost when the player was offline for more
  than about a minute or quit while offline, although the header said they were kept. They are
  now kept on disk (`pending/events-batch`, `pending/metrics-batch`) and sent at a later launch,
  with their original timestamps.
- Android games are now reported as `android` and iOS games as `ios`, instead of `linux` and
  `macos`, so the dashboard's OS filter shows them apart. On Android, where no default folder
  is writable, the SDK now logs an error when `data_dir` is missing instead of silently running
  without storage; the integration notes describe the Android `data_dir` and what to call when
  a mobile app goes to the background.
- On Windows, a player whose user folder name is outside the system's ANSI code page (for
  example a Chinese name on a Western-language system), or a `data_dir` with non-ASCII
  characters, left the SDK without any storage: no offline queue, no unclean-shutdown
  detection and no session log. Paths are now read as Unicode, and `data_dir` is taken as
  UTF-8 on every platform (a Windows path in the ANSI code page, accepted before, still
  works).
- Crash reports from the native crash handler now carry their precise grouping key, so crashes
  at different places in the same library and build appear as separate issues instead of one.
- A device number outside the server's range in `tombstone_set_device` (RAM in KB instead of
  MB, a refresh rate in mHz, more than 4096 cores) made the server refuse every crash and bug
  report from that build, which were then deleted. Such a number is now left out of the
  report, which arrives. The accepted ranges are listed in the header.
- The `tombstone.rtt_ms` self-metric (on by default) recorded one row per successful upload —
  every heartbeat and every event batch — and on the free plan these rows used up most of the
  per-session metric allowance, so the game's own events were refused. It is now sampled at
  most once every 10 minutes. Set `enable_rtt_metric = 0` to turn it off.
- The header and integration notes now state that no SDK call, `tombstone_report_crash`
  included, may be made from a signal handler or an exception filter: it can hang the crashing
  game instead of reporting the crash. Earlier notes suggested calling it from a top-level
  exception filter or Unreal's crash path. Report errors the game survives from a normal
  thread; a crash the process cannot survive is reported at the next launch.
- With `enable_native_crash_handler` on, a game that shuts the SDK down and initializes it
  again in the same process (an Android activity restart, an editor play session) no longer
  loses the native crashes that follow: they are dumped to the current data directory again.
- With `enable_native_crash_handler` on (Linux/Android), a fault that a handler installed
  earlier resolves — a Mono or CoreCLR runtime turning a null dereference or a division by
  zero into a managed exception, for example — no longer ends the game, and is not reported
  as a crash. A fault nothing resolves still ends the process with its real signal and is
  reported as before. An alternate signal stack the engine or runtime registered is now kept
  instead of replaced.
- Quitting while the network does not answer (captive portal, firewall, server not
  responding) could freeze the game for up to about 30 s inside `tombstone_shutdown` or at
  process exit. Shutdown now cancels the request in progress after its 2 s delivery window,
  so it returns in about 3 s at most; what could not be delivered is kept for the next launch.
  Connecting to the server also gives up after 5 s instead of the whole request timeout.
- A device whose clock is more than five minutes off lost everything it sent: the server refused
  the request and the SDK deleted the crash or bug report. The SDK now takes the server's time from
  every response and uses it when signing, retrying at once with the corrected time, so these
  devices report normally after their first request. A refusal of this kind never deletes a report.
- Crash and bug reports whose text holds bytes that are not valid UTF-8 (a Windows path or a
  system error message in a local code page, in the stack hint, a log line or a breadcrumb) were
  refused by the server and then deleted. Each invalid sequence is now sent as the Unicode
  replacement character, so these reports arrive. Reports of this kind that an earlier
  version left waiting on disk are cleaned the same way when they are sent.
- Pending reports are now written under a temporary name and renamed once complete, so a crash
  or a full disk during the write no longer leaves a truncated report that the server refuses.
  A pending report or batch that no launch could deliver within 90 days is deleted unsent, so a
  report kept through refusals is not retried at every launch forever.
- When a studio reaches the spending cap it set, the server refuses everything with 402 until a
  time it names. The SDK deleted the crash or bug report that got this answer and kept sending
  heartbeats, events and metrics into the refusal until the end of the period. It now sends
  nothing until that time (checking again at least every hour, so raising the cap resumes a
  running game within the hour), keeps crash and bug reports and sends them afterwards, and
  drops the heartbeats, events and metrics captured during the stop, which the server would
  not store.
- A refused SDK token (401 `invalid_api_key`), a 403 or a 404 (usually a wrong `endpoint`)
  made the SDK delete every crash and bug report it tried to send while it kept sending. Any
  refusal of this kind now stops sending for the rest of the launch and keeps crash and bug
  reports on disk for the next one, as the Unity and Godot SDKs and the CLI uploader do. Only a
  payload the server rejects as malformed or too large (400, 413, 422) is still deleted.
- On macOS, reading the host identifier at `tombstone_init` could wait without limit; it now
  gives up after 2 s, and the player identifier then derives from a random source as on a
  machine without one. On iOS the SDK reads no host identifier (an app may not): the
  identifier derives from a random source kept in `data_dir`. The SDK also builds with
  MinGW-w64.
- The header said every entry point returns a `tombstone_result`; eight return `void`. It now
  says so, and its top comment summarises how the SDK handles server refusals.

### Added

- CI cross-compiles the SDK code and unit tests for Android (arm64-v8a, armeabi-v7a, x86_64)
  and iOS, and runs the unit tests on an iOS simulator. The new CMake option
  `TOMBSTONE_BUILD_SDK=OFF` builds only the unit tests, without libcurl.

## [0.9.4] - 2026-09-18

- Fix a copied range-loop variable in the new batch tests. GCC/Clang treat it as an error under the SDK's warnings-as-errors policy, so the default Linux/macOS build of 0.9.3 failed even though Windows compiled. Runtime transport behaviour is unchanged from 0.9.3.

## [0.9.3] - 2026-09-18

- Split event and metric envelopes at 512 KiB using encoded byte sizes. Snapshot drains preserve item order and flush all ready envelopes; an individually invalid oversized item is isolated.
- Upload session logs using the server's signed multipart POST fields, with the file last and no ingest Authorization header. Preserve policy fields across retries; retain compatibility with explicit legacy PUT descriptors.
- Correct the ingest signature header to X-Tombstack-Signature so the server verifies the HMAC.
- Update the source fallback to curl 8.22.0 with a SHA-256 pin. Requires CMake 3.18+; the fallback uses Schannel on Windows and OpenSSL development libraries on Linux/macOS. System libcurl remains supported.
- Add real HTTP transport tests and an opt-in live storage round-trip check. The C ABI and library names are unchanged.

Validation: Windows x64 MSVC shared/static libraries, examples and all 20 CTest suites; real production signed ingestion, S3 log upload and byte-identical authenticated read-back using a test tenant. Linux/macOS execution was not available locally.

## [0.9.2] - 2026-08-25

### Fixed — heartbeat ceiling lowered to 240s (behaviour change; no API/ABI change)

- **`options.heartbeat_interval_s` is now clamped to `[15, 240]`, was `[15, 600]`.** The ceiling is a
  BILLING constant, not a preference. The server merges consecutive beats into one live session only
  while the gap stays under its 300s session window, so a 600s cadence shattered a continuously-online
  player into isolated instants: concurrent sessions stopped overlapping, monthly **peak CCU** read low,
  and the studio was invoiced on the low figure. An integrator raising the interval to cut telemetry
  cost — explicitly permitted by the header's own comment — silently reduced their own bill.

  This is bit-for-bit the defect the Unity SDK shipped in 0.19.3. The Tombstack repo's
  `tests/heartbeat-cadence.test.ts` was written to stop it recurring, but read the C# source **only**,
  so this SDK kept 600 with a green suite. That guard now parses the ceiling out of this repo's public
  header as well, and asserts all three SDKs (Unity, Godot, native) agree.

  A caller that passed a value above 240 gets 240 instead. Nothing else about the cadence changes; the
  default (60s) and the floor (15s) are untouched.

## [0.9.1] - 2026-07-20

### Hardening (production audit; no API/ABI change)

- **Native crash handler — bounds-checked ELF NOTE parsing.** `read_build_id` now validates the note
  name + descriptor against the segment end before reading, and guards forward progress, so a
  truncated/hostile `PT_NOTE` can't drive an out-of-bounds read at handler install.
- **Native crash handler — atomic re-entry guard.** The double-fault guard is now
  `std::atomic_flag::test_and_set` (async-signal-safe AND cross-thread) instead of a
  read-then-write `volatile sig_atomic_t`, so two threads faulting at once can't both write the dump.
- **TLS verification asserted.** The libcurl transport now sets `CURLOPT_SSL_VERIFYPEER`/`VERIFYHOST`
  explicitly instead of inheriting the integrator's defaults.
- **Breadcrumb memory scrub on consent revoke / GDPR reset.** `BreadcrumbRing::clear()` now clears the
  slot strings, not just the head/count, so pre-revoke breadcrumb text doesn't linger in process memory.
- **Version string fixed** — `tombstone_version()` now reports the real version (was stuck at `0.8.0`);
  header docblock updated to describe the shipped opt-in native handler.

## [0.9.0] - 2026-07-20

### Added — in-process native crash handler (EXPERIMENTAL, opt-in; Linux/Android)

- **`options.enable_native_crash_handler`** (default **0**/off). When enabled on a POSIX/ELF
  target, the SDK installs an **async-signal-safe** handler for `SIGSEGV`, `SIGABRT`, `SIGBUS`,
  `SIGILL`, `SIGFPE`, `SIGTRAP`. On a fatal signal it writes a minimal dump — the faulting `PC`
  (and `LR` on ARM) resolved to **`(module, build-id, offset)`** frames — to
  `<data_dir>/native-crash.dump`, then turns it into a real crash report at the **next launch**
  (through the existing sidecar/report pipeline). Grouping signature is derived from the native
  frames; the report carries `crashType` (`native_crash`/`signal`) and `osSignal`.
- **No on-device symbolication.** Frames are addresses only; symbolication happens server-side
  against uploaded symbols (the `libil2cpp.so` pipeline). The handler does `write()` + fixed
  arithmetic only — no `malloc`, no locks, no managed callbacks — with a pre-opened dump fd, a
  pre-captured module table (`dl_iterate_phdr` + `NT_GNU_BUILD_ID`), a pre-allocated `sigaltstack`
  (survives stack-overflow SIGSEGV), and a re-entrance/double-fault guard.
- **Handler chaining.** The previously-installed disposition for each signal is saved and invoked
  after the dump (both `SA_SIGINFO` and legacy forms), so a co-resident engine handler (Unity /
  UGS Cloud Diagnostics) still runs; the process is then re-raised to the default action so it
  dies with the true signal. A CI integration test faults a real child process and asserts all
  three: dump written, chained handler ran, died with `SIGSEGV`.
- **Fail-soft + scoped.** `install()` returns false on unsupported platforms (Windows SEH and
  iOS/Mach are tracked follow-ups) or any setup failure — the SDK keeps the existing
  unclean-shutdown heuristic. When a native dump exists it supersedes the generic
  `unclean-shutdown` report for that session; a restored managed crash still wins over both.

## [0.8.0] - 2026-07-10

### Added — device identity: the SDK never sends an anonymous user (Unity 0.16 parity)

- **Device-derived provisional user id** — at init the SDK acquires a persistent
  `"dev_" + 16 lowercase hex` id and every payload carries it as `userId` until the game sets a
  real id, so sessions, crashes, and telemetry are attributable from the very first beat. The id
  is `FNV-1a-64(ingestToken + "|" + machineId)`: **the raw machine id never goes on the wire**,
  and the salt makes the SAME physical machine yield a DIFFERENT id per game (cross-tenant
  unlinkability). Machine sources: Windows `MachineGuid` (HKLM Cryptography key, 64-bit registry
  view), Linux `/etc/machine-id`, macOS host UUID (`gethostuuid`); when none is readable the SDK
  derives from a random source instead (still well-formed, persisted when storage allows). The id
  is persisted to `<data_dir>/identity` (same directory as the session marker) and **the file wins
  over a re-derive** on later launches, so the id survives an ingest-token rotation; a corrupt
  file fails a plausibility gate (`dev_` prefix, ≤ 40 chars, `[a-z0-9_-]`) and re-derives.
- **Same-session identity upgrade (`priorUserId`)** — when `tombstone_set_user(real_id)` is called
  while the effective id is still the provisional one, the session id is KEPT and the provisional
  id is stamped once as `priorUserId` on heartbeats — carried until a beat delivering it gets its
  2xx ack (commit-on-ack like the device/metadata carries: a dropped beat re-carries) — and on
  crash/bug bodies while pending, so the server merges the session's pre-login rows under the
  real player. Setting a NULL/empty user (logout) reverts to the provisional id and clears the
  pending marker — logged-out telemetry never merges into anyone.
- New pure core unit `src/device_identity.{h,cpp}` (FNV-1a-64, id derivation, plausibility
  validation) with a `device_identity` CTest suite pinned to the published FNV vectors, plus
  byte-exact `priorUserId` present/absent/clamp payload tests.

Wire-compatible: the steady state (a set user, or a session that never authenticates) adds no new
fields beyond the now-always-present `userId`; `priorUserId` (server schemas already accept it)
appears only during the one-shot upgrade window.

## [0.7.1] - 2026-07-09

### Fixed — locale-independent JSON number formatting
`JsonWriter::number_field` used `snprintf("%.17g")`, which honors the process-global
`LC_NUMERIC`: a host game calling `setlocale(LC_NUMERIC, "de_DE")` (common in localized
titles) made doubles print a decimal **comma** — invalid JSON that poisoned the entire
payload the number rode on (the server rejected the whole heartbeat, silently losing
CCU/metadata/frame stats for every player in that locale). The writer now normalizes the
locale's decimal separator back to `.` (the Unity SDK's `CultureInfo.InvariantCulture`
guard, ported), with regression tests covering comma-decimal locales.

## [0.7.0] - 2026-07-09

> **ABI note — recompile required:** the `tombstone_options` struct grew a new
> field (`auto_start_session`) in 0.7.0. Consumers **must recompile against the
> 0.7 header**; a binary compiled against a 0.6 header must not be run against
> a 0.7 library.

### Added — Unity-parity pass (StartSession gate, metadata, frame stats, Retry-After, pre-init)

- **StartSession first-beat gate (Unity 0.15.0 parity)** — new option
  `tombstone_options.auto_start_session` (default 1 = today's behavior) + new API
  `tombstone_result tombstone_start_session(void)`, a one-way idempotent latch. With
  `auto_start_session = 0`, the heartbeat loop sends NOTHING and event/metric batch drains are
  HELD (items keep buffering, bounded) until `tombstone_start_session()` — so the first beat no
  longer registers an anonymous "production" session before the game sets identity / environment /
  metadata. Crash and bug reports are exempt (their write-ahead path never waits). Calling
  `tombstone_start_session()` before init is remembered and survives into init. The first beat
  after the latch is kicked out promptly (not up to one interval late). NOTE: the options struct
  grew a field — consumers must recompile against the 0.7 header.
- **Per-user metadata (Unity parity)** — new
  `tombstone_set_user_metadata(keys, values, count)`: REPLACE semantics (NULL/0 clears), clamped
  to the server contract (≤ 16 pairs, key ≤ 64, value ≤ 512, mirroring `user-metadata.ts`).
  Rides HEARTBEATS as the `metadata` object with change detection: a beat carries the map only
  when it differs from the last map a heartbeat ACKED (commit-on-2xx, M1), a clear sends `{}`
  once, and a `tombstone_set_user` identity change resets the baseline / drops the map so one
  player's attributes never bleed onto the next (epoch-guarded against stale acks, M2). The
  reserved key `displayName` becomes the player's dashboard label.
- **Frame stats (Unity 0.11 parity)** — new `void tombstone_report_frame(double frame_ms)`:
  an allocation-free, spinlock-guarded accumulator fed once per frame. Each heartbeat drains
  the interval's window onto the beat as `fpsAvg` / `slowFramePct` (> 33.4 ms — a missed
  30 FPS budget) / `hitchCount` (> 250 ms — a visible hitch) / `worstFrameMs`, then resets;
  all values clamped to the `heartbeat-schema.ts` maxima (1000 / 100 / 1e6 / 1e7). Omitted
  entirely when no frame was sampled (dedicated servers stay clean).
- **Retry-After honoring** — the worker now captures the `Retry-After` response header (minimal
  libcurl header callback) and, on 429/503, raises its exponential backoff to
  `max(backoff, retry_after)` capped at **300 s** (seconds form only; the HTTP-date form falls
  back to plain backoff). New pure helpers `parse_retry_after_seconds` / `retry_delay`
  (`src/retry_after.h`).
- **Pre-init buffering (Unity 0.9.8 parity)** — `tombstone_add_breadcrumb`,
  `tombstone_track_event`, `tombstone_track_metric`, `tombstone_set_user`,
  `tombstone_set_environment`, `tombstone_set_user_metadata`, `tombstone_set_consent`, and
  `tombstone_start_session` called BEFORE `tombstone_init` now BUFFER instead of failing
  silent: bounded store (breadcrumbs ≤ 64 drop-oldest; events + metrics ≤ 128 combined
  drop-newest; last-write-wins for identity / environment / metadata / consent) replayed in
  order at init AFTER the options are applied — an explicit pre-init Set* beats the matching
  init option (the Unity clobber fix). Replayed tracks carry replay-time timestamps (the
  pre-init window is milliseconds in practice).

Wire-compatible: a caller using none of the new APIs produces byte-identical bodies to 0.6.0
(every new heartbeat field is omitted when unset). New unit suites `frame_stats`, `preinit`,
`retry_after` + byte-exact metadata/heartbeat payload tests, all wired into CTest.

## [0.6.0] - 2026-07-04

### Added — parity with the Unity SDK (deployment env, fleet, device specs)

- **`tombstone_set_environment(environment)`** — deployment environment (e.g. `"production"`,
  `"staging"`, `"dev"`) stamped as `environment` on every payload (crash / bug / event / metric /
  heartbeat; clamped to 64). Seedable via the new `tombstone_options.environment` field; an
  explicit call overrides. NULL/empty is rejected and the current value retained — the environment
  can never be silently blanked. The server defaults to `"production"` when the field is absent.
- **`tombstone_set_server_info(region, hostname)`** — server-lifetime fleet labels emitted on
  heartbeats only (`region` ≤64, `hostname` ≤255), so the dashboard can group and locate dedicated
  servers. NULL keeps a label; `""` clears it.
- **`tombstone_mark_dedicated_server(server_id, region, hostname)`** — declare a dedicated server
  WITHOUT requiring a match: flips role to `"server"` and sets the id (region/hostname optional).
  Previously only `tombstone_start_match` flipped the role, so a matchless native server never
  registered in the Fleet. NULL/empty keeps existing values (never blanks). Clients must never call
  it.
- **Device specs** — new `tombstone_device_t` struct + `tombstone_set_device(device)` (engine-
  agnostic: the SDK reports what you hand it, it never probes hardware; the struct is deep-copied
  and clamped at the ABI boundary). The `device` object is emitted on crash and bug-report bodies,
  and rides heartbeats **until one heartbeat is acked** (then dropped for the session; a lost beat
  re-carries it — same delivery discipline as the Unity SDK), powering the player-profile Hardware
  view for sessions that never crash.

All additions are wire-compatible: a caller that sets none of them produces byte-identical bodies
to 0.5.0 (every new field is omitted when unset). Byte-exact payload tests extended for each.

## [0.5.0] - 2026-06-11

### Added

- **S1 — fulfilment nonce round-trip** — the heartbeat ack's `pendingRequests` now carry a
  single-use `fulfillNonce` + `nonceExpiry` per request; the SDK parses both
  (`find_int_field`, extended `find_pending_requests`) and echoes them verbatim on the
  `/pull-requests/{id}/fulfill` body alongside `sessionId`, so the server can re-derive
  `base64url(HMAC(secret, gameId:requestId:sessionId:nonceExpiry))` and authenticate the
  honouring client. Fail-soft: an older server that mints no nonce gets the pre-S1 body.
- **S3 — signed ingest POSTs** — every ingest POST (crashes / bug-reports / events /
  heartbeats / events:batch / metrics:batch) now carries
  `X-Tombstone-Signature: t=<unixSec>,v1=<hex>`, an HMAC-SHA256 over `"<t>.<rawBody>"`
  keyed by the per-game ingest token, computed at send time on the worker thread. Pull /
  fulfill / editor requests are never signed. Signing is fail-soft (sends unsigned on any
  error). New `hmac_sha256_hex` (RFC 2104 over the existing local SHA-256, validated
  against the RFC 4231 vectors); no third-party crypto vendored.
- **K1 — round-trip metric + per-name sampling** — after each successful ingest POST the
  SDK emits a `tombstone.rtt_ms` metric (config flag `enable_rtt_metric`, default on; the
  metrics batch's own upload is exempt so it cannot recurse). New
  `tombstone_set_sample_rate(handle, name, rate)` installs a per-name keep-rate (bounded,
  mutex-guarded) that drops events/metrics below the rate before buffering.
- **K2 — level/scene context** — `tombstone_set_level(handle, level_name)` adds an info
  breadcrumb `"[scene] level: <name>"` and stores the name as the current level context.
- **K3 — diagnostics snapshot** — `tombstone_diagnostics(handle, out)` fills a C struct
  (`tombstone_diagnostics_t`): initialized, consent, queued outbound, persisted sidecars,
  last-flush age (seconds; -1 when none), and match/server id presence. Clean C ABI, no throw.

## [0.4.1] - 2026-06-11

### Fixed

- **Heartbeat correlation (HIGH)** — heartbeats now carry `role`/`serverId`/`matchId`
  (the `serverContextFields` subset of the server `heartbeatSchema`). Without them the
  server could not register native dedicated servers in the Fleet registry, and
  match/server-targeted log-pulls were never honored for native actors. `role` is always
  present (`"client"` by default, `"server"` once a match starts); `serverId`/`matchId`
  are omitted when empty (never an empty-string enum), populated from the mutex-guarded
  cached match context.
- **Worker batch drainer fail-soft (HIGH)** — the `batch_drainer_()` call in the worker
  `run()` loop is now wrapped in `try/catch (...)`, matching the existing `process()`
  pattern. An exception escaping the worker thread entry would call `std::terminate` and
  crash the host; the SDK must never crash the host.
- **Pre-crash batch flush (MEDIUM)** — `report_crash` now calls `flush_all_batches()`
  before enqueuing the crash ingest (mirrors Unity's `FlushBatches()` in
  `captureException`), so buffered events/metrics are delivered on a fatal crash path
  instead of being lost in memory.

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
