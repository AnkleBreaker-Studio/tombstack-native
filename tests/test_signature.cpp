#include "signature.h"
#include "test_framework.h"

using tombstone::compute_crash_signature;
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
