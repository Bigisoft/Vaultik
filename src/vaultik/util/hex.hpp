#pragma once

#include <string>
#include <string_view>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

std::string to_hex(ByteSpan data);

// Throws Error when the input is not an even-length lowercase or uppercase hex string.
Bytes from_hex(std::string_view hex);

}  // namespace vaultik
