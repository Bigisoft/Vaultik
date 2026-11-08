#pragma once

#include <array>
#include <cstdint>

#include "vaultik/chunker/fastcdc.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik {

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kKdfPbkdf2Sha256 = 1;
constexpr std::uint32_t kDefaultKdfIterations = 600000;
constexpr int kDefaultZstdLevel = 3;

// Contents of the plaintext `config` file (see docs/FORMAT.md).
struct RepoConfig {
    std::uint32_t version = kFormatVersion;
    std::array<std::uint8_t, 16> repo_id{};
    std::uint32_t kdf = kKdfPbkdf2Sha256;
    std::uint32_t kdf_iterations = kDefaultKdfIterations;
    std::array<std::uint8_t, 32> salt{};
    ChunkerParams chunker{};
    int zstd_level = kDefaultZstdLevel;
    Bytes wrapped_key;  // nonce || AES-GCM(master key) || tag
};

// Header bytes (everything before the wrapped key); used as AAD for the key wrap.
Bytes serialize_config_header(const RepoConfig& config);
Bytes serialize_config(const RepoConfig& config);
RepoConfig parse_config(ByteSpan data);

// Seals the master key into config.wrapped_key using `password`.
void wrap_master_key(RepoConfig& config, const MasterKey& key, std::string_view password);

// Throws AuthError on a wrong password.
MasterKey unwrap_master_key(const RepoConfig& config, std::string_view password);

}  // namespace vaultik
