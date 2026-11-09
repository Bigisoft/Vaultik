#include "vaultik/util/base64.hpp"

#include "vaultik/util/error.hpp"

namespace vaultik {

namespace {

std::string encode(ByteSpan data, const char* alphabet, bool pad) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 3 <= data.size(); i += 3) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(alphabet[v >> 18]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(alphabet[(v >> 6) & 63]);
        out.push_back(alphabet[v & 63]);
    }
    const std::size_t rest = data.size() - i;
    if (rest == 1) {
        const std::uint32_t v = data[i] << 16;
        out.push_back(alphabet[v >> 18]);
        out.push_back(alphabet[(v >> 12) & 63]);
        if (pad) out += "==";
    } else if (rest == 2) {
        const std::uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(alphabet[v >> 18]);
        out.push_back(alphabet[(v >> 12) & 63]);
        out.push_back(alphabet[(v >> 6) & 63]);
        if (pad) out.push_back('=');
    }
    return out;
}

int value_of(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

}  // namespace

std::string base64_encode(ByteSpan data) {
    return encode(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", true);
}

std::string base64url_encode(ByteSpan data) {
    return encode(data, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_", false);
}

Bytes base64_decode(std::string_view text) {
    while (!text.empty() && text.back() == '=') text.remove_suffix(1);
    if (text.size() % 4 == 1) throw Error("invalid base64 text");
    Bytes out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = value_of(c);
        if (v < 0) throw Error("invalid base64 text");
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFF));
        }
    }
    return out;
}

}  // namespace vaultik
