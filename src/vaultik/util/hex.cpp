#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::string to_hex(ByteSpan data) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.resize(data.size() * 2);
    for (std::size_t i = 0; i < data.size(); ++i) {
        out[2 * i] = kDigits[data[i] >> 4];
        out[2 * i + 1] = kDigits[data[i] & 0x0F];
    }
    return out;
}

Bytes from_hex(std::string_view hex) {
    if (hex.size() % 2 != 0) throw Error("invalid hex string: odd length");
    Bytes out(hex.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(hex[2 * i]);
        const int lo = nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) throw Error("invalid hex string");
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

}  // namespace vaultik
