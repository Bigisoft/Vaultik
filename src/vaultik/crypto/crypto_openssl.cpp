// OpenSSL (libcrypto) implementation of the crypto primitives, used on Linux. The output is
// byte-compatible with the CNG implementation, so a repository works on either platform.

#include "vaultik/crypto/crypto.hpp"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace vaultik {

namespace {

struct CtxDeleter {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CtxDeleter>;

int to_int(std::size_t n) {
    if (n > static_cast<std::size_t>(std::numeric_limits<int>::max())) throw Error("buffer too large for OpenSSL");
    return static_cast<int>(n);
}

void check(int ok, const char* what) {
    if (ok != 1) throw Error(std::string(what) + " failed");
}

CipherCtx new_ctx() {
    CipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) throw Error("EVP_CIPHER_CTX_new failed");
    return ctx;
}

const Key32& key_of(void* p) {
    return *static_cast<const Key32*>(p);
}

}  // namespace

void random_bytes(std::span<std::uint8_t> out) {
    if (out.empty()) return;
    check(RAND_bytes(out.data(), to_int(out.size())), "RAND_bytes");
}

void secure_wipe(void* data, std::size_t size) {
    OPENSSL_cleanse(data, size);
}

Key32 sha256(ByteSpan data) {
    Key32 out{};
    unsigned int len = 0;
    check(EVP_Digest(data.data(), data.size(), out.data(), &len, EVP_sha256(), nullptr), "EVP_Digest(SHA256)");
    return out;
}

Key32 hmac_sha256(ByteSpan key, ByteSpan data) {
    Key32 out{};
    unsigned int len = 0;
    if (HMAC(EVP_sha256(), key.data(), to_int(key.size()), data.data(), data.size(), out.data(), &len) == nullptr)
        throw Error("HMAC-SHA256 failed");
    return out;
}

Key32 pbkdf2_sha256(std::string_view password, ByteSpan salt, std::uint32_t iterations) {
    if (iterations == 0) throw Error("PBKDF2 iteration count must be positive");
    if (iterations > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) throw Error("too many iterations");
    Key32 out{};
    check(PKCS5_PBKDF2_HMAC(password.data(), to_int(password.size()), salt.data(), to_int(salt.size()),
                            static_cast<int>(iterations), EVP_sha256(), to_int(out.size()), out.data()),
          "PKCS5_PBKDF2_HMAC");
    return out;
}

Bytes pbkdf2_sha512(std::string_view password, ByteSpan salt, std::uint32_t iterations, std::size_t length) {
    if (iterations == 0) throw Error("PBKDF2 iteration count must be positive");
    // OpenSSL takes an int: a larger count (from a server's reply) would wrap to a negative one.
    if (iterations > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) throw Error("too many iterations");
    Bytes out(length);
    check(PKCS5_PBKDF2_HMAC(password.data(), to_int(password.size()), salt.data(), to_int(salt.size()),
                            static_cast<int>(iterations), EVP_sha512(), to_int(out.size()), out.data()),
          "PKCS5_PBKDF2_HMAC(SHA512)");
    return out;
}

namespace {

void aes128_raw(const void* key, const EVP_CIPHER* mode, const std::uint8_t* iv, std::uint8_t* data, std::size_t size,
                bool encrypt) {
    if (size % 16 != 0) throw Error("AES block data must be a multiple of 16 bytes");
    if (size == 0) return;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!ctx) throw Error("EVP_CIPHER_CTX_new failed");
    check(EVP_CipherInit_ex(ctx.get(), mode, nullptr, static_cast<const unsigned char*>(key), iv, encrypt ? 1 : 0),
          "EVP_CipherInit_ex(AES-128)");
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);
    int len = 0;
    check(EVP_CipherUpdate(ctx.get(), data, &len, data, to_int(size)), "EVP_CipherUpdate(AES-128)");
}

}  // namespace

Aes128::Aes128(ByteSpan key) {
    if (key.size() != 16) throw Error("AES-128 needs a 16-byte key");
    auto* copy = new std::uint8_t[16];
    std::memcpy(copy, key.data(), 16);
    ecb_ = copy;
}

Aes128::~Aes128() {
    if (ecb_ != nullptr) {
        OPENSSL_cleanse(ecb_, 16);
        delete[] static_cast<std::uint8_t*>(ecb_);
    }
}

void Aes128::ecb_encrypt(std::uint8_t* data, std::size_t size) const {
    aes128_raw(ecb_, EVP_aes_128_ecb(), nullptr, data, size, true);
}

void Aes128::ecb_decrypt(std::uint8_t* data, std::size_t size) const {
    aes128_raw(ecb_, EVP_aes_128_ecb(), nullptr, data, size, false);
}

void Aes128::cbc_encrypt(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const {
    aes128_raw(ecb_, EVP_aes_128_cbc(), iv, data, size, true);
}

void Aes128::cbc_decrypt(const std::uint8_t* iv, std::uint8_t* data, std::size_t size) const {
    aes128_raw(ecb_, EVP_aes_128_cbc(), iv, data, size, false);
}

AesGcm::AesGcm(const Key32& key) {
    key_ = new Key32(key);
}

AesGcm::~AesGcm() {
    if (key_ != nullptr) {
        auto* k = static_cast<Key32*>(key_);
        secure_wipe(k->data(), k->size());
        delete k;
    }
}

Bytes AesGcm::seal(ByteSpan plaintext, ByteSpan aad) const {
    Bytes out(kNonceSize + plaintext.size() + kTagSize);
    std::uint8_t* nonce = out.data();
    std::uint8_t* cipher = out.data() + kNonceSize;
    std::uint8_t* tag = cipher + plaintext.size();
    random_bytes({nonce, kNonceSize});

    CipherCtx ctx = new_ctx();
    check(EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EVP_EncryptInit_ex");
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceSize), nullptr),
          "set GCM nonce length");
    check(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key_of(key_).data(), nonce), "EVP_EncryptInit_ex");
    int len = 0;
    if (!aad.empty()) check(EVP_EncryptUpdate(ctx.get(), nullptr, &len, aad.data(), to_int(aad.size())), "GCM AAD");
    int written = 0;
    if (!plaintext.empty()) {
        check(EVP_EncryptUpdate(ctx.get(), cipher, &len, plaintext.data(), to_int(plaintext.size())),
              "EVP_EncryptUpdate");
        written = len;
    }
    check(EVP_EncryptFinal_ex(ctx.get(), cipher + written, &len), "EVP_EncryptFinal_ex");
    written += len;
    if (static_cast<std::size_t>(written) != plaintext.size()) throw Error("AES-GCM wrote an unexpected length");
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize), tag), "get GCM tag");
    return out;
}

Bytes AesGcm::open(ByteSpan sealed, ByteSpan aad) const {
    if (sealed.size() < kOverhead) throw AuthError("sealed data is too short");
    const std::size_t clen = sealed.size() - kOverhead;
    const std::uint8_t* nonce = sealed.data();
    const std::uint8_t* cipher = sealed.data() + kNonceSize;
    const std::uint8_t* tag = cipher + clen;

    Bytes out(clen);
    CipherCtx ctx = new_ctx();
    check(EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr), "EVP_DecryptInit_ex");
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceSize), nullptr),
          "set GCM nonce length");
    check(EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key_of(key_).data(), nonce), "EVP_DecryptInit_ex");
    int len = 0;
    if (!aad.empty()) check(EVP_DecryptUpdate(ctx.get(), nullptr, &len, aad.data(), to_int(aad.size())), "GCM AAD");
    int written = 0;
    if (clen > 0) {
        check(EVP_DecryptUpdate(ctx.get(), out.data(), &len, cipher, to_int(clen)), "EVP_DecryptUpdate");
        written = len;
    }
    check(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize),
                              const_cast<std::uint8_t*>(tag)),
          "set GCM tag");
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + written, &len) != 1) {
        secure_wipe(out.data(), out.size());
        throw AuthError("authentication failed (wrong password or corrupted data)");
    }
    written += len;
    if (static_cast<std::size_t>(written) != clen) throw Error("AES-GCM wrote an unexpected length");
    return out;
}

}  // namespace vaultik
