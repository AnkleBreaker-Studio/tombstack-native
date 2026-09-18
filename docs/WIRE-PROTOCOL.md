# Wire protocol

Everything the SDK sends, byte-for-byte. The server validates with Zod
(`src/lib/*-schema.ts` in the Tombstone monorepo); enums are whitelisted,
strings length-clamped, timestamps must be inside the 90-day retention window.
The SDK clamps client-side so a well-formed call can never be rejected for
size.

## Auth & envelope

- Every ingest POST carries `Authorization: Bearer tmb_...` (per-game SDK
  token) and `Content-Type: application/json`.
- Since 0.9.3, signed ingest sends `X-Tombstack-Signature: t=<unixSec>,v1=<hex>`,
  with HMAC-SHA256(token, `<t>.<rawBody>`). Earlier native releases used the obsolete
  `X-Tombstone-Signature` name, which the current server did not verify.
- Responses use `{ "success": true, "data": ... }` /
  `{ "success": false, "error": ... }`.
- Status classification (matches `tools/lib/upload-classify.mjs`):
  **2xx** delivered → delete sidecar · **408/429/5xx/network** transient →
  retry with backoff, keep sidecar · **other 4xx** poison → drop.

## Common fields

| Field | Rule |
|---|---|
| `occurredAtIso` | ISO-8601 UTC with milliseconds: `2026-06-10T12:00:00.000Z` |
| `buildVersion` | 1..64 chars |
| `os` | `windows` \| `macos` \| `linux` \| `other` (compile-time detected) |
| `arch` | `x64` \| `arm64` \| `x86` \| `other` |
| `userId` | ≤128 chars; v0.8: never unset — the device-derived provisional `dev_...` id until `tombstone_set_user` |
| `priorUserId` | ≤128 chars, heartbeat/crash/bug only; the provisional id carried once during a same-session identity upgrade (until a heartbeat 2xx), **omitted** otherwise |
| `steamId` | ≤32 chars, **omitted** when unset |

Absent optionals are **omitted entirely** — never sent as `""` (an empty-string
enum would fail validation).

## POST /api/v1/ingest/crashes → `201 { crashId, logUpload? }`

```json
{
  "occurredAtIso": "2026-06-10T12:00:00.000Z",
  "buildVersion": "1.4.2",
  "os": "windows",
  "arch": "x64",
  "signature": "3f2a9c...32-hex (≤128 chars)",
  "stackHint": "Access violation in Renderer (1..512)",
  "stackTrace": "frame1\nframe2 (≤8192, omitted when empty)",
  "breadcrumbs": [
    { "tsIso": "2026-06-10T11:59:58.120Z", "level": "Info", "message": "loaded level 3" }
  ],
  "userId": "player-42",
  "steamId": "7656119...",
  "log": true
}
```

- `signature`: caller-supplied, or derived as SHA-256(hint + first 8 frames
  with ` (at path:line)` suffixes stripped) truncated to 32 hex chars — the
  same fingerprint the Unity SDK computes.
- `breadcrumbs`: ≤64 entries sent (server keeps the most recent 50, rejects
  >200); each `{ tsIso? ≤40, level 1..16, message 1..512 }`.
- `log`: **always present.** `true` requests a session-log upload slot;
  `false` means "no log" and is always accepted.
- The synthetic unclean-shutdown report uses the constant signature
  `unclean-shutdown` and the previous session's buildVersion/os/arch from the
  `session.lock` marker.

## POST /api/v1/ingest/bug-reports → `201 { bugId, logUpload? }`

```json
{
  "occurredAtIso": "2026-06-10T12:00:00.000Z",
  "buildVersion": "1.4.2",
  "os": "macos",
  "arch": "arm64",
  "category": "ui (≤32, omitted when empty)",
  "message": "button does nothing (1..4000)",
  "userId": "player-42",
  "steamId": "7656119...",
  "breadcrumbs": [ { "tsIso": "...", "level": "Info", "message": "..." } ],
  "log": true
}
```

## POST /api/v1/ingest/events → `202 { eventId }`

```json
{
  "occurredAtIso": "2026-06-10T12:00:00.000Z",
  "buildVersion": "1.4.2",
  "os": "linux",
  "arch": "x64",
  "name": "level_complete (1..64)",
  "userId": "player-42",
  "attributes": { "level": "3", "difficulty": "hard" }
}
```

- `attributes`: ≤32 flat entries; keys ≤64, values ≤512 (the C API takes
  string values; the schema also accepts numbers/booleans).
- The C API does not send `level`/`message` on events — omitted, never `""`.

## POST /api/v1/ingest/heartbeats → `202 { accepted }`

```json
{
  "sessionId": "a1b2c3d4e5f6... (1..64, random per init)",
  "occurredAtIso": "2026-06-10T12:00:00.000Z",
  "buildVersion": "1.4.2",
  "os": "windows",
  "arch": "x64",
  "userId": "player-42"
}
```

Ephemeral: one attempt, never retried or persisted (a missed beat is stale).

## The log artifact (`"log": true`)

When a crash/bug body carries `"log": true`, the `201` response may include:

```json
{ "success": true, "data": { "crashId": "01H...",
  "logUpload": { "url": "https://bucket.s3.region.amazonaws.com/", "key": "logs/<gameId>/<id>.log",
    "method": "POST", "fields": { "key": "logs/<gameId>/<id>.log", "Content-Type": "text/plain",
      "Policy": "...", "X-Amz-Algorithm": "AWS4-HMAC-SHA256", "X-Amz-Credential": "...",
      "X-Amz-Date": "...", "X-Amz-Signature": "..." } } } }
```

The SDK sends multipart POST to `url`, appending every signed `fields` entry in order,
then the `file` part containing the log bytes with media type `text/plain`.
It sends **no Authorization header**: the game token must never reach the storage host.
Explicit legacy `method: "PUT"` descriptors still use raw text PUT. A missing or malformed
POST policy is rejected rather than downgraded to PUT. Signed upload slots expire, so uploads are retried in-session but never
persisted across launches. For unclean-shutdown reports (and sidecars restored
from a previous run) the **preserved `previous-session.log`** is uploaded
instead, at most once per launch.

## Offline sidecars

Events and metrics use batch endpoints. Each valid envelope stays within 512 KiB of encoded
JSON, including its timestamp and separators. Byte splitting preserves item order and does
not change the 50-item / 10-second flush triggers. Invalid individually oversized items are
isolated so their rejection cannot discard valid neighbours.

`<data_dir>/pending/{crashes,bug-reports,events}/<ms-epoch>-<8hex>.json` —
file content is the raw ingest body above, no wrapper. Drained on the next
`tombstone_init`, or by `tools/uploader/upload.mjs` pointed at the `crashes`
subdirectory.
