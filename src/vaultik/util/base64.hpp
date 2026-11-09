#pragma once

#include <string>
#include <string_view>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

// Standard base64 with padding (RFC 4648 section 4).
std::string base64_encode(ByteSpan data);
// URL-safe base64 without padding (RFC 4648 section 5).
std::string base64url_encode(ByteSpan data);
// Decodes either alphabet, with or without padding. Throws Error on anything else.
Bytes base64_decode(std::string_view text);

}  // namespace vaultik
