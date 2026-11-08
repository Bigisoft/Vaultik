#include "vaultik/crypto/hash.hpp"

#include <blake3.h>

#include "vaultik/util/hex.hpp"

namespace vaultik {

std::string id_hex(const Id& id) {
    return to_hex(id);
}

Id id_from_hex(std::string_view hex) {
    if (hex.size() != 64) throw Error("invalid id: expected 64 hex characters");
    const Bytes b = from_hex(hex);
    Id id;
    std::memcpy(id.data(), b.data(), id.size());
    return id;
}

Id random_id() {
    Id id;
    random_bytes(id);
    return id;
}

Id blake3(ByteSpan data) {
    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data.data(), data.size());
    Id out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    return out;
}

struct Blake3Hasher::State {
    blake3_hasher h;
};

Blake3Hasher::Blake3Hasher() : state_(std::make_unique<State>()) {
    blake3_hasher_init(&state_->h);
}

Blake3Hasher::~Blake3Hasher() = default;

void Blake3Hasher::update(ByteSpan data) {
    blake3_hasher_update(&state_->h, data.data(), data.size());
}

Id Blake3Hasher::finish() {
    Id out;
    blake3_hasher_finalize(&state_->h, out.data(), out.size());
    return out;
}

struct KeyedHasher::State {
    blake3_hasher initial;  // keyed state, copied for each hash
};

KeyedHasher::KeyedHasher(const Key32& key) : state_(std::make_unique<State>()) {
    blake3_hasher_init_keyed(&state_->initial, key.data());
}

KeyedHasher::~KeyedHasher() {
    secure_wipe(&state_->initial, sizeof state_->initial);
}

Id KeyedHasher::hash(ByteSpan data) const {
    blake3_hasher h = state_->initial;
    blake3_hasher_update(&h, data.data(), data.size());
    Id out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    secure_wipe(&h, sizeof h);
    return out;
}

Id KeyedHasher::hash(std::uint8_t domain, ByteSpan data) const {
    blake3_hasher h = state_->initial;
    blake3_hasher_update(&h, &domain, 1);
    blake3_hasher_update(&h, data.data(), data.size());
    Id out;
    blake3_hasher_finalize(&h, out.data(), out.size());
    secure_wipe(&h, sizeof h);
    return out;
}

}  // namespace vaultik
