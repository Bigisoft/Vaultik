#include "vaultik/crypto/crypto.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace vaultik {

namespace {

constexpr NTSTATUS kStatusAuthTagMismatch = static_cast<NTSTATUS>(0xC000A002L);

void check(NTSTATUS status, const char* what) {
    if (!BCRYPT_SUCCESS(status)) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s failed (NTSTATUS 0x%08lX)", what, static_cast<unsigned long>(status));
        throw Error(buf);
    }
}

ULONG to_ulong(std::size_t n) {
    if (n > std::numeric_limits<ULONG>::max()) throw Error("buffer too large for CNG");
    return static_cast<ULONG>(n);
}

// CNG wants non-null pointers even for empty buffers in some code paths.
PUCHAR ptr_or_dummy(const std::uint8_t* p) {
    static UCHAR dummy = 0;
    return p != nullptr ? const_cast<PUCHAR>(p) : &dummy;
}

}  // namespace

void random_bytes(std::span<std::uint8_t> out) {
    if (out.empty()) return;
    check(BCryptGenRandom(nullptr, out.data(), to_ulong(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
          "BCryptGenRandom");
}

void secure_wipe(void* data, std::size_t size) {
    SecureZeroMemory(data, size);
}

Key32 sha256(ByteSpan data) {
    Key32 out{};
    check(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, ptr_or_dummy(data.data()), to_ulong(data.size()),
                     out.data(), to_ulong(out.size())),
          "BCryptHash(SHA256)");
    return out;
}

Key32 hmac_sha256(ByteSpan key, ByteSpan data) {
    Key32 out{};
    check(BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, ptr_or_dummy(key.data()), to_ulong(key.size()),
                     ptr_or_dummy(data.data()), to_ulong(data.size()), out.data(), to_ulong(out.size())),
          "BCryptHash(HMAC-SHA256)");
    return out;
}

Key32 pbkdf2_sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations) {
    if (iterations == 0) throw Error("PBKDF2 iteration count must be positive");
    BCRYPT_ALG_HANDLE alg = nullptr;
    check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "BCryptOpenAlgorithmProvider(SHA256)");
    Key32 out{};
    const NTSTATUS status =
        BCryptDeriveKeyPBKDF2(alg, ptr_or_dummy(reinterpret_cast<const std::uint8_t*>(password.data())),
                              to_ulong(password.size()), ptr_or_dummy(salt.data()), to_ulong(salt.size()),
                              iterations, out.data(), to_ulong(out.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    check(status, "BCryptDeriveKeyPBKDF2");
    return out;
}

Bytes pbkdf2_sha512(std::string_view password, ByteSpan salt, std::uint32_t iterations, std::size_t length) {
    if (iterations == 0) throw Error("PBKDF2 iteration count must be positive");
    BCRYPT_ALG_HANDLE alg = nullptr;
    check(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "BCryptOpenAlgorithmProvider(SHA512)");
    Bytes out(length);
    const NTSTATUS status =
        BCryptDeriveKeyPBKDF2(alg, ptr_or_dummy(reinterpret_cast<const std::uint8_t*>(password.data())),
                              to_ulong(password.size()), ptr_or_dummy(salt.data()), to_ulong(salt.size()),
                              iterations, out.data(), to_ulong(out.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    check(status, "BCryptDeriveKeyPBKDF2");
    return out;
}

Aes128::Aes128(ByteSpan key) {
    if (key.size() != 16) throw Error("AES-128 needs a 16-byte key");
    BCRYPT_KEY_HANDLE ecb = nullptr;
    BCRYPT_KEY_HANDLE cbc = nullptr;
    check(BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &ecb, nullptr, 0, const_cast<PUCHAR>(key.data()), 16, 0),
          "BCryptGenerateSymmetricKey(ECB)");
    const NTSTATUS st =
        BCryptGenerateSymmetricKey(BCRYPT_AES_CBC_ALG_HANDLE, &cbc, nullptr, 0, const_cast<PUCHAR>(key.data()), 16, 0);
    if (!BCRYPT_SUCCESS(st)) {
        BCryptDestroyKey(ecb);
        check(st, "BCryptGenerateSymmetricKey(CBC)");
    }
    ecb_ = ecb;
    cbc_ = cbc;
}

Aes128::~Aes128() {
    if (ecb_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(ecb_));
    if (cbc_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(cbc_));
}

namespace {

void aes_raw(void* key, const std::uint8_t* iv, std::uint8_t* data, std::size_t size, bool encrypt) {
    if (size % 16 != 0) throw Error("AES block data must be a multiple of 16 bytes");
    if (size == 0) return;
    UCHAR ivbuf[16];
    if (iv != nullptr) std::memcpy(ivbuf, iv, 16);
    ULONG written = 0;
    const auto h = static_cast<BCRYPT_KEY_HANDLE>(key);
    const NTSTATUS st = encrypt ? BCryptEncrypt(h, data, to_ulong(size), nullptr, iv ? ivbuf : nullptr, iv ? 16 : 0,
                                                data, to_ulong(size), &written, 0)
                                : BCryptDecrypt(h, data, to_ulong(size), nullptr, iv ? ivbuf : nullptr, iv ? 16 : 0,
                                                data, to_ulong(size), &written, 0);
    check(st, encrypt ? "BCryptEncrypt(AES)" : "BCryptDecrypt(AES)");
}

}  // namespace

void Aes128::ecb_encrypt(std::uint8_t* data, std::size_t size) const {
    aes_raw(ecb_, nullptr, data, size, true);
}

void Aes128::ecb_decrypt(std::uint8_t* data, std::size_t size) const {
    aes_raw(ecb_, nullptr, data, size, false);
}

void Aes128::cbc_encrypt(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const {
    aes_raw(cbc_, iv, data, size, true);
}

void Aes128::cbc_decrypt(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const {
    aes_raw(cbc_, iv, data, size, false);
}

AesGcm::AesGcm(const Key32& key) {
    BCRYPT_KEY_HANDLE handle = nullptr;
    check(BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &handle, nullptr, 0,
                                     const_cast<PUCHAR>(key.data()), to_ulong(key.size()), 0),
          "BCryptGenerateSymmetricKey");
    key_ = handle;
}

AesGcm::~AesGcm() {
    if (key_ != nullptr) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

Bytes AesGcm::seal(ByteSpan plaintext, ByteSpan aad) const {
    Bytes out(kNonceSize + plaintext.size() + kTagSize);
    std::uint8_t* nonce = out.data();
    std::uint8_t* cipher = out.data() + kNonceSize;
    std::uint8_t* tag = cipher + plaintext.size();
    random_bytes({nonce, kNonceSize});

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce;
    info.cbNonce = static_cast<ULONG>(kNonceSize);
    info.pbAuthData = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
    info.cbAuthData = to_ulong(aad.size());
    info.pbTag = tag;
    info.cbTag = static_cast<ULONG>(kTagSize);

    ULONG written = 0;
    check(BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), ptr_or_dummy(plaintext.data()),
                        to_ulong(plaintext.size()), &info, nullptr, 0, ptr_or_dummy(cipher),
                        to_ulong(plaintext.size()), &written, 0),
          "BCryptEncrypt");
    if (written != plaintext.size()) throw Error("BCryptEncrypt wrote an unexpected length");
    return out;
}

Bytes AesGcm::open(ByteSpan sealed, ByteSpan aad) const {
    if (sealed.size() < kOverhead) throw AuthError("sealed data is too short");
    const std::size_t clen = sealed.size() - kOverhead;
    const std::uint8_t* nonce = sealed.data();
    const std::uint8_t* cipher = sealed.data() + kNonceSize;
    const std::uint8_t* tag = cipher + clen;

    Bytes out(clen);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = const_cast<PUCHAR>(nonce);
    info.cbNonce = static_cast<ULONG>(kNonceSize);
    info.pbAuthData = aad.empty() ? nullptr : const_cast<PUCHAR>(aad.data());
    info.cbAuthData = to_ulong(aad.size());
    info.pbTag = const_cast<PUCHAR>(tag);
    info.cbTag = static_cast<ULONG>(kTagSize);

    ULONG written = 0;
    const NTSTATUS status =
        BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), ptr_or_dummy(cipher), to_ulong(clen), &info, nullptr, 0,
                      ptr_or_dummy(out.data()), to_ulong(clen), &written, 0);
    if (status == kStatusAuthTagMismatch) {
        secure_wipe(out.data(), out.size());
        throw AuthError("authentication failed (wrong password or corrupted data)");
    }
    check(status, "BCryptDecrypt");
    if (written != clen) throw Error("BCryptDecrypt wrote an unexpected length");
    return out;
}

}  // namespace vaultik
