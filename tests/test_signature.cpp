#include "signature.h"
#include "test_framework.h"

#include <string>

using tombstone::build_ingest_signature_header;
using tombstone::compute_crash_signature;
using tombstone::hmac_sha256_hex;
using tombstone::sha256_hex;

TEST_CASE("signature", "sha256 matches the FIPS test vectors") {
    CHECK_EQ(sha256_hex(""),
             std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
    CHECK_EQ(sha256_hex("abc"),
             std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    CHECK_EQ(sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string{"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"});
    // Multi-block input (FIPS 180-4 two-block 896-bit vector).
    CHECK_EQ(sha256_hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
                        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
             std::string{"cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"});
}

TEST_CASE("signature", "hmac-sha256 matches the RFC 4231 test vectors") {
    // RFC 4231 Test Case 1: key = 20 x 0x0b, data = "Hi There".
    CHECK_EQ(hmac_sha256_hex(std::string(20, '\x0b'), "Hi There"),
             std::string{"b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"});
    // RFC 4231 Test Case 2: key = "Jefe", data = "what do ya want for nothing?".
    CHECK_EQ(hmac_sha256_hex("Jefe", "what do ya want for nothing?"),
             std::string{"5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"});
}

TEST_CASE("signature", "hmac with a long key (hashed first per RFC 2104)") {
    // RFC 4231 Test Case 6: key = 131 x 0xaa, data = the long "...First Hash Key First" string.
    CHECK_EQ(hmac_sha256_hex(std::string(131, '\xaa'),
                             "Test Using Larger Than Block-Size Key - Hash Key First"),
             std::string{"60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"});
}

TEST_CASE("signature", "ingest signature header has the t=...,v1=... shape") {
    const std::string body = R"({"name":"boot"})";
    const std::string header = build_ingest_signature_header("tmb_secret", body, 1700000123);
    // The header binds the body to the timestamp: v1 is the HMAC of "<t>.<body>".
    const std::string expected =
        "t=1700000123,v1=" + hmac_sha256_hex("tmb_secret", "1700000123." + body);
    CHECK_EQ(header, expected);
    CHECK_EQ(header.compare(0, 14, "t=1700000123,v"), 0);
}

TEST_CASE("signature", "crash signature is 32 lowercase hex chars") {
    const std::string sig = compute_crash_signature("NullReferenceException", "frame1\nframe2");
    CHECK_EQ(sig.size(), std::size_t{32});
    for (const char c : sig) {
        CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

TEST_CASE("signature", "ignores per-install file locations in frames") {
    // The " (at <path>:<line>)" suffix differs between machines — same bug,
    // same signature.
    const std::string a = compute_crash_signature(
        "boom", "Game.Update () (at C:/buildA/Game.cs:120)\nEngine.Tick () (at C:/buildA/E.cs:5)");
    const std::string b = compute_crash_signature(
        "boom", "Game.Update () (at D:/other/Game.cs:512)\nEngine.Tick () (at D:/other/E.cs:9)");
    CHECK_EQ(a, b);
}

TEST_CASE("signature", "differs when the failure differs") {
    const std::string a = compute_crash_signature("boom", "frameA");
    const std::string b = compute_crash_signature("boom", "frameB");
    const std::string c = compute_crash_signature("other", "frameA");
    CHECK(a != b);
    CHECK(a != c);
}

TEST_CASE("signature", "only the first eight frames matter") {
    std::string deep_trace;
    for (int i = 0; i < 8; ++i) {
        deep_trace += "frame" + std::to_string(i) + "\n";
    }
    const std::string base = compute_crash_signature("boom", deep_trace + "tail1");
    const std::string other = compute_crash_signature("boom", deep_trace + "tail2");
    CHECK_EQ(base, other);
}
