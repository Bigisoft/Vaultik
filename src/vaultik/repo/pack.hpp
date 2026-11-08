#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik {

class Backend;

enum class BlobType : std::uint8_t {
    Data = 0,
    Tree = 1,
};

const char* blob_type_name(BlobType type);

struct BlobEntry {
    BlobType type = BlobType::Data;
    Id id{};
    std::uint64_t offset = 0;
    std::uint32_t length = 0;   // sealed length inside the pack
    std::uint32_t raw_len = 0;  // plaintext length
};

void write_blob_entry(ByteWriter& w, const BlobEntry& e);
BlobEntry read_blob_entry(ByteReader& r);
constexpr std::size_t kMinBlobEntrySize = 1 + 32 + 3;

// Accumulates sealed blobs in memory and produces a pack file (see docs/FORMAT.md).
class PackWriter {
public:
    void add(BlobType type, const Id& id, ByteSpan sealed, std::uint32_t raw_len);

    std::size_t size() const { return buf_.size(); }
    bool empty() const { return entries_.empty(); }

    // Appends the sealed header and trailer, returns the complete pack bytes, and moves the blob
    // entries into `entries`. The writer is empty afterwards.
    Bytes finish(const AesGcm& aead, std::vector<BlobEntry>& entries);

private:
    Bytes buf_;
    std::vector<BlobEntry> entries_;
};

// Reads and authenticates the header of a pack file stored in a backend.
std::vector<BlobEntry> read_pack_header(Backend& backend, const std::string& name, const AesGcm& aead);

// Same, for a pack already in memory. Also checks every entry lies inside the blob area.
std::vector<BlobEntry> parse_pack_header(ByteSpan pack, const AesGcm& aead);

}  // namespace vaultik
