#include "signature.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace tombstone {

namespace {

// Compact, portable SHA-256 (FIPS 180-4). Public-domain-style reference
// implementation, kept local so the SDK has zero crypto dependencies.
constexpr std::array<std::uint32_t, 64> k = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

std::uint32_t rotr(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32U - n)); }

struct Sha256State {
    std::array<std::uint32_t, 8> h = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                                      0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

    void process_block(const unsigned char *block) noexcept {
        std::array<std::uint32_t, 64> w = {};
        for (std::size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16U) |
                   (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8U) |
                   static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        auto [a, b, c, d, e, f, g, hh] = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
};

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// Raw 32-byte SHA-256 digest of `data` (the binary form HMAC composes over).
std::array<unsigned char, 32> sha256_raw(std::string_view data) {
    Sha256State state;
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.data());
    const std::size_t full_blocks = data.size() / 64;
    for (std::size_t i = 0; i < full_blocks; ++i) {
        state.process_block(bytes + i * 64);
    }

    // Final padded block(s): 0x80, zeros, 64-bit big-endian bit length.
    std::array<unsigned char, 128> tail = {};
    const std::size_t remainder = data.size() % 64;
    if (remainder > 0) {
        std::memcpy(tail.data(), bytes + full_blocks * 64, remainder);
    }
    tail[remainder] = 0x80U;
    const std::size_t tail_blocks = (remainder + 1 + 8 > 64) ? 2 : 1;
    const std::uint64_t bit_length = static_cast<std::uint64_t>(data.size()) * 8U;
    for (std::size_t i = 0; i < 8; ++i) {
        tail[tail_blocks * 64 - 1 - i] = static_cast<unsigned char>((bit_length >> (8U * i)) & 0xFFU);
    }
    for (std::size_t i = 0; i < tail_blocks; ++i) {
        state.process_block(tail.data() + i * 64);
    }

    std::array<unsigned char, 32> digest = {};
    for (std::size_t word = 0; word < 8; ++word) {
        digest[word * 4] = static_cast<unsigned char>((state.h[word] >> 24U) & 0xFFU);
        digest[word * 4 + 1] = static_cast<unsigned char>((state.h[word] >> 16U) & 0xFFU);
        digest[word * 4 + 2] = static_cast<unsigned char>((state.h[word] >> 8U) & 0xFFU);
        digest[word * 4 + 3] = static_cast<unsigned char>(state.h[word] & 0xFFU);
    }
    return digest;
}

std::string to_hex(const unsigned char *bytes, std::size_t length) {
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (std::size_t i = 0; i < length; ++i) {
        out += hex_digits[(bytes[i] >> 4U) & 0xFU];
        out += hex_digits[bytes[i] & 0xFU];
    }
    return out;
}

}  // namespace

std::string sha256_hex(std::string_view data) {
    const std::array<unsigned char, 32> digest = sha256_raw(data);
    return to_hex(digest.data(), digest.size());
}

std::string hmac_sha256_hex(std::string_view key, std::string_view message) {
    constexpr std::size_t block_size = 64;
    // Keys longer than the block are first hashed (RFC 2104).
    std::array<unsigned char, block_size> key_block = {};
    if (key.size() > block_size) {
        const std::array<unsigned char, 32> hashed = sha256_raw(key);
        std::memcpy(key_block.data(), hashed.data(), hashed.size());
    } else if (!key.empty()) {
        std::memcpy(key_block.data(), key.data(), key.size());
    }

    std::string inner;
    inner.reserve(block_size + message.size());
    for (const unsigned char byte : key_block) {
        inner += static_cast<char>(byte ^ 0x36U);
    }
    inner.append(message.data(), message.size());
    const std::array<unsigned char, 32> inner_digest = sha256_raw(inner);

    std::string outer;
    outer.reserve(block_size + inner_digest.size());
    for (const unsigned char byte : key_block) {
        outer += static_cast<char>(byte ^ 0x5cU);
    }
    outer.append(reinterpret_cast<const char *>(inner_digest.data()), inner_digest.size());
    const std::array<unsigned char, 32> mac = sha256_raw(outer);
    return to_hex(mac.data(), mac.size());
}

std::string build_ingest_signature_header(std::string_view ingest_key, std::string_view body,
                                          std::int64_t unix_seconds) {
    const std::string t = std::to_string(unix_seconds);
    std::string signed_input;
    signed_input.reserve(t.size() + 1 + body.size());
    signed_input += t;
    signed_input += '.';
    signed_input.append(body.data(), body.size());
    return "t=" + t + ",v1=" + hmac_sha256_hex(ingest_key, signed_input);
}

std::string compute_crash_signature(std::string_view stack_hint, std::string_view stack_trace) {
    constexpr std::size_t signature_frames = 8;
    constexpr std::size_t signature_hex_length = 32;

    std::string material{stack_hint};
    std::size_t frames = 0;
    std::size_t line_start = 0;
    while (frames < signature_frames && line_start < stack_trace.size()) {
        std::size_t line_end = stack_trace.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            line_end = stack_trace.size();
        }
        std::string_view frame = stack_trace.substr(line_start, line_end - line_start);
        // Drop file paths / line numbers so the same bug hashes the same.
        const std::size_t at = frame.find(" (at ");
        if (at != std::string_view::npos) {
            frame = frame.substr(0, at);
        }
        frame = trim(frame);
        if (!frame.empty()) {
            material += '\n';
            material += frame;
            ++frames;
        }
        line_start = line_end + 1;
    }
    return sha256_hex(material).substr(0, signature_hex_length);
}

}  // namespace tombstone
