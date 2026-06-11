# Third-party software

| Component | Version | License | Usage |
|---|---|---|---|
| [libcurl](https://curl.se/libcurl/) | system, or 8.11.1 via CMake FetchContent | [curl license](https://curl.se/docs/copyright.html) (MIT-derivative) | HTTPS transport (ingest POSTs + presigned S3 PUTs) |

Notes:

- When `find_package(CURL)` resolves a system libcurl, that build's own TLS
  backend and licenses apply (e.g. OpenSSL on most Linux distributions).
- The FetchContent fallback builds curl statically, HTTP-only, against the
  platform TLS stack: Schannel (Windows), SecureTransport (macOS), OpenSSL
  (Linux) — no bundled TLS library.
- The SHA-256 implementation in `src/signature.cpp` and the civil-date
  algorithm in `src/clock.cpp` are independent implementations of public
  algorithms (FIPS 180-4; Howard Hinnant's public-domain date algorithms) —
  no third-party code is vendored.
- The HMAC-SHA256 in `src/signature.cpp` (request signing, §S3) is an
  independent implementation of the RFC 2104 HMAC construction layered over
  that same local SHA-256 — again no third-party code is vendored. It is
  validated in CI against the RFC 4231 published test vectors.

Phase 2 (minidump capture) will add: sentry-native (MIT), Crashpad
(Apache-2.0), mini_chromium (BSD) — provenance will be recorded here when
they are vendored, per the monorepo's license gate (BSD/Apache/MIT only).
