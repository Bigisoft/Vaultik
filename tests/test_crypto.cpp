#include <doctest/doctest.h>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;

TEST_CASE("PBKDF2-HMAC-SHA256 matches published test vectors") {
    const auto salt = as_bytes("salt");
    CHECK(to_hex(pbkdf2_sha256("password", salt, 1)) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(to_hex(pbkdf2_sha256("password", salt, 2)) ==
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
}

TEST_CASE("BLAKE3 matches the reference hash of empty input") {
    CHECK(id_hex(blake3({})) == "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST_CASE("incremental BLAKE3 equals one-shot") {
    const Bytes data(100000, 0x42);
    Blake3Hasher h;
    h.update(ByteSpan(data).first(1000));
    h.update(ByteSpan(data).subspan(1000));
    CHECK(h.finish() == blake3(data));
}

TEST_CASE("keyed hash depends on key and domain") {
    Key32 k1{};
    Key32 k2{};
    k2[0] = 1;
    KeyedHasher h1(k1);
    KeyedHasher h2(k2);
    const auto data = as_bytes("same data");
    CHECK(h1.hash(data) == h1.hash(data));
    CHECK(h1.hash(data) != h2.hash(data));
    CHECK(h1.hash(0, data) != h1.hash(1, data));
}

TEST_CASE("AES-256-GCM seal/open and tamper detection") {
    Key32 key;
    random_bytes(key);
    AesGcm aead(key);
    const auto plain = as_bytes("secret backup data");
    const auto aad = as_bytes("context");

    const Bytes sealed = aead.seal(plain, aad);
    CHECK(sealed.size() == plain.size() + AesGcm::kOverhead);
    const Bytes opened = aead.open(sealed, aad);
    CHECK(std::equal(opened.begin(), opened.end(), plain.begin(), plain.end()));

    // Fresh nonce every time.
    CHECK(aead.seal(plain, aad) != sealed);

    Bytes tampered = sealed;
    tampered[AesGcm::kNonceSize + 3] ^= 0x01;
    CHECK_THROWS_AS(aead.open(tampered, aad), AuthError);
    CHECK_THROWS_AS(aead.open(sealed, as_bytes("other")), AuthError);

    Key32 other;
    random_bytes(other);
    AesGcm wrong(other);
    CHECK_THROWS_AS(wrong.open(sealed, aad), AuthError);

    CHECK(aead.open(aead.seal({}, {}), {}).empty());
}
