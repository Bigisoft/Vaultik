#include <doctest/doctest.h>

#include <random>
#include <set>

#include "vaultik/chunker/fastcdc.hpp"
#include "vaultik/crypto/hash.hpp"

using namespace vaultik;

namespace {

Bytes seeded_bytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    return b;
}

std::vector<Bytes> chunk_all(const FastCdc& cdc, const Bytes& data, std::size_t read_size) {
    std::size_t pos = 0;
    Bytes buffer;
    ChunkReader reader(
        cdc,
        [&](std::uint8_t* dst, std::size_t max) {
            const std::size_t n = std::min({max, read_size, data.size() - pos});
            std::memcpy(dst, data.data() + pos, n);
            pos += n;
            return n;
        },
        buffer);
    std::vector<Bytes> chunks;
    ByteSpan c;
    while (reader.next(c)) chunks.emplace_back(c.begin(), c.end());
    return chunks;
}

}  // namespace

TEST_CASE("chunks reassemble to the input and respect size limits") {
    const ChunkerParams p{};
    FastCdc cdc(p);
    const Bytes data = seeded_bytes(20u << 20, 1);
    const auto chunks = chunk_all(cdc, data, 1u << 20);

    Bytes joined;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        if (i + 1 < chunks.size()) {
            CHECK(chunks[i].size() >= p.min_size);
        }
        CHECK(chunks[i].size() <= p.max_size);
        joined.insert(joined.end(), chunks[i].begin(), chunks[i].end());
    }
    CHECK(joined == data);
    // Average should be in the right ballpark for random data.
    const double avg = static_cast<double>(data.size()) / static_cast<double>(chunks.size());
    CHECK(avg > 512.0 * 1024);
    CHECK(avg < 2.5 * 1024 * 1024);
}

TEST_CASE("chunk boundaries do not depend on read size") {
    FastCdc cdc(ChunkerParams{});
    const Bytes data = seeded_bytes(9u << 20, 2);
    CHECK(chunk_all(cdc, data, 4096) == chunk_all(cdc, data, 3u << 20));
}

TEST_CASE("an insertion only changes nearby chunks") {
    FastCdc cdc(ChunkerParams{});
    const Bytes a = seeded_bytes(16u << 20, 3);
    Bytes b = a;
    b.insert(b.begin() + (5u << 20), 0x5A);

    std::set<Id> ids_a;
    for (const auto& c : chunk_all(cdc, a, 1u << 20)) ids_a.insert(blake3(c));
    std::size_t shared = 0;
    const auto chunks_b = chunk_all(cdc, b, 1u << 20);
    for (const auto& c : chunks_b) shared += ids_a.contains(blake3(c));
    CHECK(shared + 3 >= chunks_b.size());
}

TEST_CASE("small and empty inputs") {
    FastCdc cdc(ChunkerParams{});
    CHECK(chunk_all(cdc, {}, 4096).empty());
    const Bytes small = seeded_bytes(1000, 4);
    const auto chunks = chunk_all(cdc, small, 4096);
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0] == small);
}

TEST_CASE("invalid parameters are rejected") {
    CHECK_THROWS_AS(FastCdc(ChunkerParams{256 * 1024, 1000 * 1000, 4u << 20}), Error);
    CHECK_THROWS_AS(FastCdc(ChunkerParams{2u << 20, 1u << 20, 4u << 20}), Error);
}
