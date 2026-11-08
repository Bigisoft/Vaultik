#include "vaultik/crypto/crypto.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
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
