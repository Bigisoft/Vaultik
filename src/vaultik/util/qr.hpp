#pragma once

// A small QR Code encoder (ISO/IEC 18004, model 2): byte mode only, all four error correction
// levels, versions 1 to 40, the mask with the lowest penalty unless one is asked for. Enough for
// the recovery kit, which prints the recovery key as a QR code.

#include <cstdint>
#include <string_view>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik::qr {

// Error correction: about 7%, 15%, 25% or 30% of the code may be damaged and still read.
enum class Ecc { L, M, Q, H };

struct Code {
    int version = 0;  // 1..40
    int size = 0;     // modules per side: 17 + 4 * version
    int mask = 0;     // 0..7
    Ecc ecc = Ecc::M;
    std::vector<std::uint8_t> modules;  // size * size, row by row, 1 = dark

    bool dark(int x, int y) const {
        return x >= 0 && y >= 0 && x < size && y < size && modules[static_cast<std::size_t>(y) * size + x] != 0;
    }
};

// Encodes `data` in byte mode with the smallest version from `min_version` on that holds it.
// `mask` -1 picks the mask with the lowest penalty; 0..7 forces one. Throws Error when the data
// does not fit in version 40 or an argument is out of range.
Code encode(ByteSpan data, Ecc ecc, int min_version = 1, int mask = -1);
Code encode_text(std::string_view utf8, Ecc ecc, int min_version = 1, int mask = -1);

// The data bytes a version and level hold in byte mode.
std::size_t byte_capacity(int version, Ecc ecc);

}  // namespace vaultik::qr
