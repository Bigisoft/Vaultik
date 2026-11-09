#include "vaultik/crypto/md5.hpp"

#include <bit>
#include <cstring>

namespace vaultik {

namespace {

constexpr std::uint32_t kShift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                      5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

constexpr std::uint32_t kSine[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

void block(std::uint32_t state[4], const std::uint8_t* p) {
    std::uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = static_cast<std::uint32_t>(p[i * 4]) | (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (std::uint32_t i = 0; i < 64; ++i) {
        std::uint32_t f, g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f = f + a + kSine[i] + m[g];
        a = d;
        d = c;
        c = b;
        b = b + std::rotl(f, static_cast<int>(kShift[i]));
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

}  // namespace

std::array<std::uint8_t, 16> md5(ByteSpan data) {
    std::uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::size_t i = 0;
    for (; i + 64 <= data.size(); i += 64) block(state, data.data() + i);

    // The tail, a 0x80 byte, zeros, and the length in bits (little-endian) in one or two blocks.
    std::uint8_t tail[128] = {};
    const std::size_t rest = data.size() - i;
    if (rest > 0) std::memcpy(tail, data.data() + i, rest);
    tail[rest] = 0x80;
    const std::size_t total = rest + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    for (int k = 0; k < 8; ++k) tail[total - 8 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
    for (std::size_t off = 0; off < total; off += 64) block(state, tail + off);

    std::array<std::uint8_t, 16> out{};
    for (int w = 0; w < 4; ++w)
        for (int k = 0; k < 4; ++k) out[w * 4 + k] = static_cast<std::uint8_t>(state[w] >> (8 * k));
    return out;
}

}  // namespace vaultik
