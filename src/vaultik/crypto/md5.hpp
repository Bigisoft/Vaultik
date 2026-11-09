#pragma once

// MD5 (RFC 1321). Not for security: only for protocols that ask for it, such as the Content-MD5
// header S3 requires on uploads with Object Lock.

#include <array>
#include <cstdint>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

std::array<std::uint8_t, 16> md5(ByteSpan data);

}  // namespace vaultik
