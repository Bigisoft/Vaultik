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

// PBKDF2-HMAC-SHA512 with an output of `length` bytes (MEGA sign-in).
Bytes pbkdf2_sha512(std::string_view password, ByteSpan salt, std::uint32_t iterations, std::size_t length);

// Raw AES-128 without padding, for protocols that define their own modes (MEGA). Lengths must be
// multiples of 16. Operates in place.
class Aes128 {
public:
    explicit Aes128(ByteSpan key);  // 16 bytes
    ~Aes128();
    Aes128(const Aes128&) = delete;
    Aes128& operator=(const Aes128&) = delete;

    void ecb_encrypt(std::uint8_t* data, std::size_t size) const;
    void ecb_decrypt(std::uint8_t* data, std::size_t size) const;
    // `iv` is 16 bytes; it is not changed.
    void cbc_encrypt(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const;
    void cbc_decrypt(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const;

private:
    void* ecb_ = nullptr;
    void* cbc_ = nullptr;
};

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
