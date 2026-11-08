#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik {

// 32-byte BLAKE3 output; identifies blobs, packs, index files and snapshots.
using Id = std::array<std::uint8_t, 32>;

struct IdHash {
    std::size_t operator()(const Id& id) const noexcept {
        std::size_t v;
        std::memcpy(&v, id.data(), sizeof v);  // ids are uniformly random already
        return v;
    }
};

std::string id_hex(const Id& id);
Id id_from_hex(std::string_view hex);
Id random_id();

// Unkeyed BLAKE3 of a buffer.
Id blake3(ByteSpan data);

// Incremental unkeyed BLAKE3.
class Blake3Hasher {
public:
    Blake3Hasher();
    ~Blake3Hasher();
    Blake3Hasher(const Blake3Hasher&) = delete;
    Blake3Hasher& operator=(const Blake3Hasher&) = delete;

    void update(ByteSpan data);
    Id finish();

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Keyed BLAKE3, used for blob ids so stored data does not reveal content fingerprints.
class KeyedHasher {
public:
    explicit KeyedHasher(const Key32& key);
    ~KeyedHasher();
    KeyedHasher(const KeyedHasher&) = delete;
    KeyedHasher& operator=(const KeyedHasher&) = delete;

    Id hash(ByteSpan data) const;

    // hash(domain || data) without copying; used to separate blob types.
    Id hash(std::uint8_t domain, ByteSpan data) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace vaultik
