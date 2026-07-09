# Tombstack Native SDK — per-engine integration notes

> The product is **Tombstack**; the `tombstone_*` C symbols and the
> `tombstone.dll` / `libtombstone` library name are kept for ABI stability.

The SDK is one shared library + one pure C99 header. Integration is always
the same four steps:

1. Load/link the library and include `<tombstone/tombstone.h>`.
2. `tombstone_options_init` → fill `endpoint`, `token`, `build_version` →
   `tombstone_init` (once, at boot).
3. Pipe your engine's log output into `tombstone_log_line` (this feeds both
   the breadcrumb trail and the uploadable session log).
4. Call `tombstone_report_crash` from wherever your engine already learns
   about failures (top-level exception filter, script-VM error handler, assert
   handler), `tombstone_flush` + `tombstone_shutdown` on clean exit.

Threading: every call is thread-safe; uploads happen on a background thread,
never on the caller's. Nothing throws across the ABI. Calls made *before*
`tombstone_init` buffer (bounded) and replay at init, so early breadcrumbs,
events, and Set* calls are never lost.

## Identity-first startup (recommended)

By default the first heartbeat leaves the moment `tombstone_init` returns —
as an *anonymous, "production"* session. If you know who is playing (or which
environment you are in), hold the first beat until you have said so:

```c
tombstone_options opt;
tombstone_options_init(&opt);
opt.endpoint = "https://your-tenant.example.com";
opt.token = "tmb_...";
opt.build_version = "1.4.2";
opt.auto_start_session = 0;               /* hold heartbeats + batches */
tombstone_init(&opt);

tombstone_set_user("player-123", NULL);   /* identity first */
tombstone_set_environment("staging");
const char *mk[] = {"displayName"};
const char *mv[] = {"Reaper"};
tombstone_set_user_metadata(mk, mv, 1);

tombstone_start_session();                /* NOW the first beat leaves */
```

`tombstone_start_session()` is a one-way idempotent latch; crash and bug
reports are never held by it. Per-frame performance: call
`tombstone_report_frame(frame_ms)` once per frame and each heartbeat carries
that interval's fps average / slow-frame % / hitch count / worst frame.

## Unreal Engine

- Put the header + import lib in a `ThirdParty/Tombstack` module; add the
  `.lib`/`.so` to `PublicAdditionalLibraries` and stage the runtime library
  with `RuntimeDependencies`.
- Init in `UGameInstance::Init`; shutdown in `Shutdown`.
- Log hook: register a `FOutputDevice` with `GLog->AddOutputDevice(...)` and
  forward each line to `tombstone_log_line` (map `ELogVerbosity` →
  `tombstone_level`).
- Crashes: in `FCoreDelegates::OnHandleSystemError` or your custom
  `ReportCrash` path, call `tombstone_report_crash(NULL, summary, callstack, 1)`
  then `tombstone_flush(2000)`. (Unreal's own crash handler still runs;
  Phase 2 minidump capture will integrate beneath it.)
- `data_dir`: `FPaths::ProjectSavedDir() / TEXT("Tombstack")`.

## Godot 4 (GDExtension)

- Link `libtombstone` from your extension; init in
  `_initialize`/`ENTER_TREE` of an autoload.
- Log hook: `OS.add_logger` (4.5+) or wrap `push_error`/`push_warning` call
  sites; forward script errors to `tombstone_report_crash` with the GDScript
  stack as `stack_trace`.
- `data_dir`: pass `OS.get_user_data_dir() + "/tombstone"`.

## Custom engine / plain C or C++

- See [`examples/minimal.c`](../examples/minimal.c).
- If you already have a SEH filter (`SetUnhandledExceptionFilter`) or signal
  handlers, keep them minimal: format what you safely can, write your own
  notes, and call `tombstone_report_crash` from the *next* launch if your
  process is too corrupted to upload — or rely on the SDK's unclean-shutdown
  detection, which needs zero crash-time code.
- Dedicated servers: disable the session log (`enable_session_log = 0`) if
  you already ship logs elsewhere; heartbeats give you live session counts.

## C# / other managed runtimes (non-Unity)

```csharp
[DllImport("tombstone")] static extern int tombstone_init(ref TombstoneOptions o);
[DllImport("tombstone")] static extern int tombstone_report_crash(
    string signature, string stackHint, string stackTrace, int attachLog);
```

Marshal strings as UTF-8 (`CharSet.Ansi` is fine for ASCII; prefer
`UnmanagedType.LPUTF8Str` on modern runtimes). Unity itself should use the
official Tombstack Unity SDK instead — it has deeper engine hooks.

## Consent / store policy

If your platform requires opt-in telemetry, init with `consent_granted = 0`
and call `tombstone_set_consent(1)` after the player accepts. While consent
is off nothing is recorded or sent — calls return `TOMBSTONE_DISABLED`.

## Shipping checklist

- [ ] `build_version` matches the build you ship (dashboard filters by it).
- [ ] `data_dir` points at a writable per-user location.
- [ ] Engine log hooked into `tombstone_log_line`.
- [ ] `tombstone_flush` + `tombstone_shutdown` on clean exit (otherwise the
      next launch reports an unclean shutdown — by design).
- [ ] The SDK token is treated as a build secret (don't commit it).
