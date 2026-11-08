#include "vaultik/repo/pack.hpp"

#include <cstring>
#include <limits>

#include "vaultik/repo/backend.hpp"

namespace vaultik {

namespace {

constexpr std::uint8_t kPackMagic[4] = {'V', 'K', 'P', 'K'};
constexpr char kHeaderAad[] = "VKPKHDR1";
constexpr std::size_t kTrailerSize = 4 + sizeof kPackMagic;

ByteSpan header_aad() {
    return as_bytes(std::string_view(kHeaderAad, sizeof kHeaderAad - 1));
}

std::vector<BlobEntry> decode_header(ByteSpan sealed_header, const AesGcm& aead, std::uint64_t blob_area) {
    const Bytes plain = aead.open(sealed_header, header_aad());
    ByteReader r(plain);
    const std::size_t count = r.count(kMinBlobEntrySize);
    std::vector<BlobEntry> entries;
    entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        BlobEntry e = read_blob_entry(r);
        if (e.offset > blob_area || e.length > blob_area - e.offset) throw Error("malformed pack: entry out of range");
        entries.push_back(e);
    }
    r.expect_done();
    return entries;
}

std::uint32_t parse_trailer(ByteSpan trailer) {
    if (std::memcmp(trailer.data() + 4, kPackMagic, sizeof kPackMagic) != 0) throw Error("not a pack file");
    ByteReader r(trailer.first(4));
    return r.u32();
}

}  // namespace

const char* blob_type_name(BlobType type) {
    switch (type) {
        case BlobType::Data:
            return "data";
        case BlobType::Tree:
            return "tree";
    }
    return "unknown";
}

void write_blob_entry(ByteWriter& w, const BlobEntry& e) {
    w.u8(static_cast<std::uint8_t>(e.type));
    w.raw(e.id);
    w.varint(e.offset);
    w.varint(e.length);
    w.varint(e.raw_len);
}

BlobEntry read_blob_entry(ByteReader& r) {
    BlobEntry e;
    const std::uint8_t type = r.u8();
    if (type > static_cast<std::uint8_t>(BlobType::Tree)) throw Error("malformed entry: unknown blob type");
    e.type = static_cast<BlobType>(type);
    r.raw_into(e.id);
    e.offset = r.varint();
    const std::uint64_t length = r.varint();
    const std::uint64_t raw_len = r.varint();
    if (length > std::numeric_limits<std::uint32_t>::max() || raw_len > std::numeric_limits<std::uint32_t>::max())
        throw Error("malformed entry: blob too large");
    e.length = static_cast<std::uint32_t>(length);
    e.raw_len = static_cast<std::uint32_t>(raw_len);
    return e;
}

void PackWriter::add(BlobType type, const Id& id, ByteSpan sealed, std::uint32_t raw_len) {
    BlobEntry e;
    e.type = type;
    e.id = id;
    e.offset = buf_.size();
    e.length = static_cast<std::uint32_t>(sealed.size());
    e.raw_len = raw_len;
    buf_.insert(buf_.end(), sealed.begin(), sealed.end());
    entries_.push_back(e);
}

Bytes PackWriter::finish(const AesGcm& aead, std::vector<BlobEntry>& entries) {
    ByteWriter h;
    h.varint(entries_.size());
    for (const BlobEntry& e : entries_) write_blob_entry(h, e);
    const Bytes sealed = aead.seal(h.buffer(), header_aad());

    ByteWriter tail;
    tail.u32(static_cast<std::uint32_t>(sealed.size()));
    tail.raw(kPackMagic);

    Bytes out = std::move(buf_);
    out.insert(out.end(), sealed.begin(), sealed.end());
    out.insert(out.end(), tail.buffer().begin(), tail.buffer().end());

    entries = std::move(entries_);
    buf_ = Bytes();
    entries_.clear();
    return out;
}

std::vector<BlobEntry> read_pack_header(Backend& backend, const std::string& name, const AesGcm& aead) {
    const std::uint64_t size = backend.size(name);
    if (size < kTrailerSize) throw Error("pack file too small: " + name);
    const Bytes trailer = backend.get_range(name, size - kTrailerSize, kTrailerSize);
    const std::uint32_t header_len = parse_trailer(trailer);
    if (header_len > size - kTrailerSize) throw Error("malformed pack header length: " + name);
    const std::uint64_t header_off = size - kTrailerSize - header_len;
    const Bytes sealed = backend.get_range(name, header_off, header_len);
    return decode_header(sealed, aead, header_off);
}

std::vector<BlobEntry> parse_pack_header(ByteSpan pack, const AesGcm& aead) {
    if (pack.size() < kTrailerSize) throw Error("pack file too small");
    const std::uint32_t header_len = parse_trailer(pack.last(kTrailerSize));
    if (header_len > pack.size() - kTrailerSize) throw Error("malformed pack header length");
    const std::size_t header_off = pack.size() - kTrailerSize - header_len;
    return decode_header(pack.subspan(header_off, header_len), aead, header_off);
}

}  // namespace vaultik
