#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

using Key32 = std::array<std::uint8_t, 32>;

// Cryptographically secure random bytes (CNG on Windows, OpenSSL elsewhere).
void random_bytes(std::span<std::uint8_t> out);

// Overwrites memory in a way the compiler cannot optimize away.
void secure_wipe(void* data, std::size_t size);

// SHA-256 and HMAC-SHA256 (for signing cloud storage requests).
Key32 sha256(ByteSpan data);
Key32 hmac_sha256(ByteSpan key, ByteSpan data);

// PBKDF2-HMAC-SHA256 with a 32-byte output.
Key32 pbkdf2_sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations);

// AES-256-GCM (CNG on Windows, OpenSSL elsewhere). Output layout: nonce(12) || ciphertext || tag(16).
class AesGcm {
public:
    static constexpr std::size_t kNonceSize = 12;
    static constexpr std::size_t kTagSize = 16;
    static constexpr std::size_t kOverhead = kNonceSize + kTagSize;

    explicit AesGcm(const Key32& key);
    ~AesGcm();

    AesGcm(const AesGcm&) = delete;
    AesGcm& operator=(const AesGcm&) = delete;

    Bytes seal(ByteSpan plaintext, ByteSpan aad) const;

    // Throws AuthError when the tag does not verify (wrong key, tampering, corruption).
    Bytes open(ByteSpan sealed, ByteSpan aad) const;

private:
    void* key_ = nullptr;  // BCRYPT_KEY_HANDLE, or a heap copy of the key for OpenSSL
};

// The repository master key. Wiped from memory on destruction.
struct MasterKey {
    Key32 enc{};  // AES-256-GCM key for every sealed object
    Key32 id{};   // keyed BLAKE3 key for blob ids

    MasterKey() = default;
    MasterKey(const MasterKey&) = default;
    MasterKey& operator=(const MasterKey&) = default;
    ~MasterKey() {
        secure_wipe(enc.data(), enc.size());
        secure_wipe(id.data(), id.size());
    }
};

}  // namespace vaultik
