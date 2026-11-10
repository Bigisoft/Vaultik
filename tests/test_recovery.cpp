#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <string>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/repo/recovery.hpp"

using namespace vaultik;
using namespace vaultik::repo;

namespace {

Bytes random_bytes(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    Bytes b(n);
    std::size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        const std::uint64_t v = rng();
        std::memcpy(b.data() + i, &v, 8);
    }
    for (; i < n; ++i) b[i] = static_cast<std::uint8_t>(rng());
    return b;
}

// Layout facts the tests need, read back from the header (see docs/FORMAT.md).
struct Layout {
    std::uint64_t data_size;
    std::uint32_t shard_size;
    std::uint32_t k;
    std::uint32_t m;
    std::size_t header_size;
};

Layout layout_of(ByteSpan recovery) {
    ByteReader r(recovery.subspan(4));
    Layout l{};
    r.u32();
    l.data_size = r.u64();
    l.shard_size = r.u32();
    l.k = r.u32();
    l.m = r.u32();
    l.header_size = 60 + 16 * (static_cast<std::size_t>(l.k) + l.m) + 16;
    return l;
}

// Recomputes the header checksum after a test forges header fields.
void reseal_header(Bytes& recovery) {
    const Layout l = layout_of(recovery);
    const Id check = blake3(ByteSpan(recovery.data(), l.header_size - 16));
    std::memcpy(recovery.data() + l.header_size - 16, check.data(), 16);
}

void put_u32(Bytes& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

void put_u64(Bytes& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Flips a few bytes inside data shard j.
void damage_shard(Bytes& data, std::size_t shard_size, std::size_t j, std::mt19937_64& rng) {
    const std::size_t begin = j * shard_size;
    const std::size_t len = std::min(shard_size, data.size() - begin);
    for (int t = 0; t < 3; ++t) data[begin + rng() % len] ^= static_cast<std::uint8_t>(1 + rng() % 255);
}

}  // namespace

TEST_CASE("recovery: intact round trip") {
    const Bytes original = random_bytes(1'000'000, 1);
    const Bytes rec = make_recovery(original, 5);
    const Layout l = layout_of(rec);
    CHECK(l.data_size == original.size());
    CHECK(l.k <= 240);
    CHECK(l.k + l.m <= 255);
    CHECK(l.m == std::max<std::uint32_t>(1, (l.k * 5 + 99) / 100));
    CHECK(rec.size() == l.header_size + static_cast<std::size_t>(l.m) * l.shard_size);

    Bytes data = original;
    const RecoveryResult r = repair_with_recovery(data, rec);
    CHECK(r.intact);
    CHECK_FALSE(r.repaired);
    CHECK(r.bad_shards == 0);
    CHECK(data == original);
    CHECK(recovery_matches(original, rec));
}

TEST_CASE("recovery: output is deterministic and the vector path matches the scalar path") {
    const Bytes original = random_bytes(3'000'017, 2);
    const Bytes vec = make_recovery(original, 7);
    set_recovery_simd_enabled(false);
    const Bytes scalar = make_recovery(original, 7);
    set_recovery_simd_enabled(true);
    CHECK(vec == scalar);
    CHECK(make_recovery(original, 7) == vec);
}

TEST_CASE("recovery: random damage in up to m shards is repaired") {
    std::mt19937_64 rng(3);
    for (unsigned percent : {1u, 5u, 10u, 50u}) {
        const Bytes original = random_bytes(2'000'003, percent);
        const Bytes rec = make_recovery(original, percent);
        const Layout l = layout_of(rec);
        for (int round = 0; round < 3; ++round) {
            Bytes data = original;
            const std::size_t count = 1 + rng() % l.m;
            std::set<std::size_t> picked;
            while (picked.size() < count) picked.insert(rng() % l.k);
            for (std::size_t j : picked) damage_shard(data, l.shard_size, j, rng);
            const RecoveryResult r = repair_with_recovery(data, rec);
            CAPTURE(percent);
            CAPTURE(count);
            CHECK(r.repaired);
            CHECK_FALSE(r.intact);
            CHECK(r.bad_shards == count);
            CHECK(data == original);
        }
    }
}

TEST_CASE("recovery: the scalar path repairs too") {
    const Bytes original = random_bytes(500'000, 4);
    const Bytes rec = make_recovery(original, 10);
    const Layout l = layout_of(rec);
    Bytes data = original;
    std::mt19937_64 rng(4);
    for (std::size_t j = 0; j < l.m; ++j) damage_shard(data, l.shard_size, j * 2, rng);
    set_recovery_simd_enabled(false);
    const RecoveryResult r = repair_with_recovery(data, rec);
    set_recovery_simd_enabled(true);
    CHECK(r.repaired);
    CHECK(data == original);
}

TEST_CASE("recovery: a burst of bad bytes spanning several shards") {
    const Bytes original = random_bytes(32u << 20, 5);
    const Bytes rec = make_recovery(original, 5);
    const Layout l = layout_of(rec);
    REQUIRE(l.m >= 4);
    // Zeroes a run of bytes as a bad sector range would: it starts mid-shard and covers parts of
    // three shards.
    Bytes data = original;
    const std::size_t start = 10 * static_cast<std::size_t>(l.shard_size) + l.shard_size / 2;
    std::fill_n(data.begin() + static_cast<std::ptrdiff_t>(start), 2 * l.shard_size, std::uint8_t{0});
    const RecoveryResult r = repair_with_recovery(data, rec);
    CHECK(r.repaired);
    CHECK(r.bad_shards == 3);
    CHECK(data == original);
}

TEST_CASE("recovery: truncated and extended files") {
    const Bytes original = random_bytes(1'234'567, 6);
    const Bytes rec = make_recovery(original, 5);
    const Layout l = layout_of(rec);

    SUBCASE("cut short within the parity limit") {
        Bytes data(original.begin(), original.end() - static_cast<std::ptrdiff_t>(l.shard_size + 100));
        const RecoveryResult r = repair_with_recovery(data, rec);
        CHECK(r.repaired);
        CHECK(r.bad_shards == 2);
        CHECK(data == original);
    }
    SUBCASE("cut short by one byte") {
        Bytes data(original.begin(), original.end() - 1);
        const RecoveryResult r = repair_with_recovery(data, rec);
        CHECK(r.repaired);
        CHECK(r.bad_shards == 1);
        CHECK(data == original);
    }
    SUBCASE("empty file") {
        Bytes data;
        const RecoveryResult r = repair_with_recovery(data, rec);
        CHECK_FALSE(r.repaired);
        CHECK(r.bad_shards == l.k);
        CHECK(data.empty());
    }
    SUBCASE("extended with junk") {
        Bytes data = original;
        const Bytes junk = random_bytes(5000, 7);
        data.insert(data.end(), junk.begin(), junk.end());
        const RecoveryResult r = repair_with_recovery(data, rec);
        CHECK(r.repaired);
        CHECK(r.bad_shards == 0);
        CHECK(data == original);
    }
    SUBCASE("extended and damaged") {
        Bytes data = original;
        data[0] ^= 0xFF;
        data.push_back(1);
        const RecoveryResult r = repair_with_recovery(data, rec);
        CHECK(r.repaired);
        CHECK(r.bad_shards == 1);
        CHECK(data == original);
    }
}

TEST_CASE("recovery: damaged parity shards are skipped") {
    const Bytes original = random_bytes(4'000'000, 8);
    Bytes rec = make_recovery(original, 10);
    const Layout l = layout_of(rec);
    REQUIRE(l.m >= 4);
    std::mt19937_64 rng(8);
    // Damage two parity shards; the rest still cover m - 2 bad data shards.
    rec[l.header_size + 0 * static_cast<std::size_t>(l.shard_size) + 17] ^= 1;
    rec[l.header_size + 3 * static_cast<std::size_t>(l.shard_size) + 99] ^= 0x80;

    Bytes data = original;
    for (std::size_t j = 0; j < l.m - 2; ++j) damage_shard(data, l.shard_size, j * 3 + 1, rng);
    RecoveryResult r = repair_with_recovery(data, rec);
    CHECK(r.repaired);
    CHECK(data == original);

    // One more bad data shard is more than the intact parity can rebuild.
    Bytes worse = original;
    for (std::size_t j = 0; j < l.m - 1; ++j) damage_shard(worse, l.shard_size, j * 3 + 1, rng);
    const Bytes before = worse;
    r = repair_with_recovery(worse, rec);
    CHECK_FALSE(r.repaired);
    CHECK_FALSE(r.intact);
    CHECK(r.bad_shards == l.m - 1);
    CHECK(r.message == std::to_string(l.m - 1) + " parts are damaged but only " + std::to_string(l.m - 2) + " can be rebuilt");
    CHECK(worse == before);
}

TEST_CASE("recovery: truncated recovery data loses only the cut parity shards") {
    const Bytes original = random_bytes(3'000'000, 9);
    const Bytes rec = make_recovery(original, 10);
    const Layout l = layout_of(rec);
    const Bytes cut(rec.begin(), rec.end() - static_cast<std::ptrdiff_t>(l.shard_size + 1));
    Bytes data = original;
    std::mt19937_64 rng(9);
    for (std::size_t j = 0; j < l.m - 2; ++j) damage_shard(data, l.shard_size, j, rng);
    const RecoveryResult r = repair_with_recovery(data, cut);
    CHECK(r.repaired);
    CHECK(data == original);
}

TEST_CASE("recovery: too much damage reports failure") {
    const Bytes original = random_bytes(1'000'000, 10);
    const Bytes rec = make_recovery(original, 5);
    const Layout l = layout_of(rec);
    Bytes data = original;
    std::mt19937_64 rng(10);
    for (std::size_t j = 0; j <= l.m; ++j) damage_shard(data, l.shard_size, j, rng);
    const Bytes before = data;
    const RecoveryResult r = repair_with_recovery(data, rec);
    CHECK_FALSE(r.repaired);
    CHECK_FALSE(r.intact);
    CHECK(r.bad_shards == l.m + 1);
    CHECK(r.message == std::to_string(l.m + 1) + " parts are damaged but only " + std::to_string(l.m) + " can be rebuilt");
    CHECK(data == before);
}

TEST_CASE("recovery: tiny inputs") {
    for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{4095}, std::size_t{4096}, std::size_t{4097}, std::size_t{8193}}) {
        CAPTURE(n);
        const Bytes original = random_bytes(n, 11 + n);
        const Bytes rec = make_recovery(original, 1);
        const Layout l = layout_of(rec);
        CHECK(l.m >= 1);
        CHECK(static_cast<std::uint64_t>(l.k) * l.shard_size >= n);

        Bytes data = original;
        CHECK(repair_with_recovery(data, rec).intact);
        CHECK(recovery_matches(original, rec));

        if (n > 0) {
            data[n / 2] ^= 0x5A;
            const RecoveryResult r = repair_with_recovery(data, rec);
            CHECK(r.repaired);
            CHECK(r.bad_shards == 1);
            CHECK(data == original);

            data.clear();
            const RecoveryResult cut = repair_with_recovery(data, rec);
            CHECK(cut.repaired == (l.k == 1));
            if (l.k == 1) CHECK(data == original);
        }

        data = original;
        data.push_back(7);
        const RecoveryResult ext = repair_with_recovery(data, rec);
        CHECK(ext.repaired);
        CHECK(data == original);
    }
}

TEST_CASE("recovery: recovery_matches") {
    const Bytes original = random_bytes(100'000, 12);
    const Bytes rec = make_recovery(original, 5);
    CHECK(recovery_matches(original, rec));
    Bytes changed = original;
    changed[500] ^= 1;
    CHECK_FALSE(recovery_matches(changed, rec));
    CHECK_FALSE(recovery_matches(ByteSpan(original).first(original.size() - 1), rec));
    CHECK_FALSE(recovery_matches(original, make_recovery(changed, 5)));
    CHECK_FALSE(recovery_matches(original, Bytes{}));
    CHECK_FALSE(recovery_matches(original, random_bytes(5000, 13)));
}

TEST_CASE("recovery: bad arguments to make_recovery") {
    const Bytes data = random_bytes(1000, 14);
    CHECK_THROWS_AS(make_recovery(data, 0), Error);
    CHECK_THROWS_AS(make_recovery(data, 51), Error);
    CHECK_NOTHROW(make_recovery(data, 50));
}

TEST_CASE("recovery: garbage and forged recovery data throw and never crash") {
    const Bytes original = random_bytes(300'000, 15);
    const Bytes rec = make_recovery(original, 5);
    const Layout l = layout_of(rec);
    Bytes data = original;

    CHECK_THROWS_AS(repair_with_recovery(data, Bytes{}), Error);
    CHECK_THROWS_AS(repair_with_recovery(data, random_bytes(10'000, 16)), Error);
    CHECK_THROWS_AS(repair_with_recovery(data, ByteSpan(rec).first(30)), Error);
    CHECK_THROWS_AS(repair_with_recovery(data, ByteSpan(rec).first(l.header_size - 1)), Error);

    SUBCASE("wrong magic") {
        Bytes bad = rec;
        bad[0] = 'X';
        CHECK_THROWS_AS(repair_with_recovery(data, bad), Error);
    }
    SUBCASE("wrong version") {
        Bytes bad = rec;
        put_u32(bad, 4, 2);
        reseal_header(bad);
        CHECK_THROWS_AS(repair_with_recovery(data, bad), Error);
    }
    SUBCASE("every single header byte flipped") {
        for (std::size_t i = 0; i < l.header_size; ++i) {
            Bytes bad = rec;
            bad[i] ^= 0x01;
            CAPTURE(i);
            CHECK_THROWS_AS(repair_with_recovery(data, bad), Error);
            CHECK_FALSE(recovery_matches(original, bad));
        }
        CHECK(data == original);
    }
    SUBCASE("forged fields with a valid checksum") {
        auto forged = [&](auto&& edit) {
            Bytes bad = rec;
            edit(bad);
            reseal_header(bad);
            Bytes d = original;
            CHECK_THROWS_AS(repair_with_recovery(d, bad), Error);
            CHECK(d == original);
        };
        forged([](Bytes& b) { put_u64(b, 8, std::uint64_t{1} << 40); });  // 1 TiB of data
        forged([](Bytes& b) { put_u64(b, 8, ~std::uint64_t{0}); });
        forged([](Bytes& b) { put_u32(b, 16, 0xFFFFFFFFu); });   // shard size
        forged([](Bytes& b) { put_u32(b, 16, 0); });
        forged([](Bytes& b) { put_u32(b, 16, 1); });             // shards would not cover the data
        forged([](Bytes& b) { put_u64(b, 8, 1); });              // shard far larger than the data
        forged([](Bytes& b) { put_u64(b, 8, 0); });
        // Changing the counts moves where the checksum sits; reseal_header follows them, and the
        // parse must still reject them before sizing anything.
        Bytes counts = rec;
        put_u32(counts, 20, 0xFFFFFFFFu);
        CHECK_THROWS_AS(repair_with_recovery(data, counts), Error);
        put_u32(counts, 20, 200);
        put_u32(counts, 24, 100);
        CHECK_THROWS_AS(repair_with_recovery(data, counts), Error);
        put_u32(counts, 20, 0);
        put_u32(counts, 24, 1);
        CHECK_THROWS_AS(repair_with_recovery(data, counts), Error);
        put_u32(counts, 20, 1);
        put_u32(counts, 24, 0);
        CHECK_THROWS_AS(repair_with_recovery(data, counts), Error);
    }
    SUBCASE("random headers with a valid checksum") {
        std::mt19937_64 rng(17);
        for (int t = 0; t < 2000; ++t) {
            Bytes bad = rec;
            // Plausible small counts so the forged header still fits, random everything else.
            put_u64(bad, 8, rng() % (std::uint64_t{1} << (rng() % 25)));
            put_u32(bad, 16, static_cast<std::uint32_t>(rng() % (std::uint64_t{1} << (rng() % 33))));
            put_u32(bad, 20, l.k);
            put_u32(bad, 24, l.m);
            reseal_header(bad);
            Bytes d = original;
            try {
                const RecoveryResult r = repair_with_recovery(d, bad);
                CHECK_FALSE(r.repaired);
            } catch (const Error&) {
            }
        }
    }
    SUBCASE("garbage parity with a matching header") {
        Bytes bad = rec;
        for (std::size_t i = l.header_size; i < bad.size(); ++i) bad[i] = static_cast<std::uint8_t>(i * 31);
        Bytes d = original;
        d[0] ^= 1;
        const RecoveryResult r = repair_with_recovery(d, bad);
        CHECK_FALSE(r.repaired);
        CHECK_FALSE(r.message.empty());
    }
}

TEST_CASE("recovery: encode throughput") {
    const Bytes data = random_bytes(32u << 20, 18);
    const auto start = std::chrono::steady_clock::now();
    const Bytes rec = make_recovery(data, 5);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    MESSAGE("make_recovery 32 MiB at 5%: " << static_cast<int>(32.0 / seconds) << " MB/s ("
                                           << static_cast<int>(seconds * 1000) << " ms, " << rec.size() << " bytes of recovery data)");
    set_recovery_simd_enabled(false);
    const auto start2 = std::chrono::steady_clock::now();
    const Bytes rec2 = make_recovery(data, 5);
    const double seconds2 = std::chrono::duration<double>(std::chrono::steady_clock::now() - start2).count();
    set_recovery_simd_enabled(true);
    MESSAGE("scalar path: " << static_cast<int>(32.0 / seconds2) << " MB/s");
    CHECK(rec2 == rec);

    Bytes damaged = data;
    const Layout l = layout_of(rec);
    std::mt19937_64 rng(18);
    for (std::size_t j = 0; j < l.m; ++j) damage_shard(damaged, l.shard_size, j * 7, rng);
    const auto start3 = std::chrono::steady_clock::now();
    const RecoveryResult r = repair_with_recovery(damaged, rec);
    const double seconds3 = std::chrono::duration<double>(std::chrono::steady_clock::now() - start3).count();
    MESSAGE("repair of " << l.m << " bad shards: " << static_cast<int>(seconds3 * 1000) << " ms");
    CHECK(r.repaired);
    CHECK(damaged == data);
}
