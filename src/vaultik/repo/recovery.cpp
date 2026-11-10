#include "vaultik/repo/recovery.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "vaultik/crypto/hash.hpp"

#if defined(_M_X64) || defined(__x86_64__)
#define VAULTIK_RS_X86 1
#include <immintrin.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

#if defined(VAULTIK_RS_X86) && (defined(__GNUC__) || defined(__clang__))
#define VAULTIK_RS_AVX2 __attribute__((target("avx2")))
#else
#define VAULTIK_RS_AVX2
#endif

namespace vaultik::repo {

namespace {

constexpr std::uint32_t kVersion = 1;
constexpr std::uint8_t kMagic[4] = {'V', 'K', 'R', 'D'};
constexpr std::size_t kShardHashSize = 16;
constexpr std::size_t kHeaderHashSize = 16;
// magic, version, data_size, shard_size, data_shards, parity_shards, data_hash.
constexpr std::size_t kFixedHeaderSize = 4 + 4 + 8 + 4 + 4 + 4 + 32;
constexpr std::size_t kMaxTotalShards = 255;
// More shards means finer-grained damage tracking but more parity work per byte; 240 data shards
// leaves room for parity at low percentages while staying under the GF(256) limit of 255.
constexpr std::size_t kMaxDataShards = 240;
// Small files get fewer, larger shards: a shard below this size costs more in hashes than it saves.
constexpr std::size_t kMinShardSize = 4096;
// Encoding walks the shards in column blocks of this size so the source block stays in L1 and
// the parity blocks in L2 while every parity row is updated.
constexpr std::size_t kBlockSize = 16 * 1024;

// ---- GF(2^8) arithmetic, polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D), generator 2 -----------

struct GfTables {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
    // mul[c][x] = c * x. 64 KiB, built once; each row is the per-coefficient table of the scalar path.
    std::array<std::array<std::uint8_t, 256>, 256> mul{};
    // Split-nibble tables for the vector path: c * x and c * (x << 4) for x in 0..15.
    std::array<std::array<std::uint8_t, 16>, 256> lo{};
    std::array<std::array<std::uint8_t, 16>, 256> hi{};

    GfTables() {
        unsigned x = 1;
        for (unsigned i = 0; i < 255; ++i) {
            exp[i] = static_cast<std::uint8_t>(x);
            log[x] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (unsigned i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        for (unsigned a = 0; a < 256; ++a) {
            for (unsigned b = 0; b < 256; ++b) {
                mul[a][b] = (a == 0 || b == 0) ? 0 : exp[log[a] + log[b]];
            }
            for (unsigned n = 0; n < 16; ++n) {
                lo[a][n] = mul[a][n];
                hi[a][n] = mul[a][n << 4];
            }
        }
    }
};

const GfTables& gf() {
    static const GfTables tables;
    return tables;
}

std::uint8_t gf_mul(std::uint8_t a, std::uint8_t b) { return gf().mul[a][b]; }

std::uint8_t gf_inv(std::uint8_t a) {
    // a != 0 is guaranteed by the callers (Cauchy denominators and pivots are never zero).
    const GfTables& t = gf();
    return t.exp[255 - t.log[a]];
}

// Parity row i, data column j of the Cauchy matrix 1 / (x_i + y_j) with x_i = k + i and y_j = j.
// All x and y are distinct values below 255, so every square submatrix is invertible and the
// systematic code [I; C] rebuilds the data from any k of its k + m shards.
std::uint8_t cauchy(std::size_t k, std::size_t i, std::size_t j) {
    return gf_inv(static_cast<std::uint8_t>((k + i) ^ j));
}

// ---- dst ^= c * src ----------------------------------------------------------------------------

std::atomic<bool> g_simd_enabled{true};

#if defined(VAULTIK_RS_X86)
bool cpu_has_avx2() {
#if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return false;
    __cpuid(r, 1);
    const bool osxsave = (r[2] & (1 << 27)) != 0;
    const bool avx = (r[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;
    if ((_xgetbv(0) & 6) != 6) return false;  // the OS saves the YMM registers
    __cpuidex(r, 7, 0);
    return (r[1] & (1 << 5)) != 0;
#else
    return __builtin_cpu_supports("avx2");
#endif
}

const bool kHaveAvx2 = cpu_has_avx2();

// Returns how many leading bytes it processed (a multiple of 32); the caller does the tail.
VAULTIK_RS_AVX2 std::size_t mul_add_avx2(std::uint8_t c, const std::uint8_t* src, std::uint8_t* dst, std::size_t n) {
    const GfTables& t = gf();
    const __m256i tlo = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t.lo[c].data())));
    const __m256i thi = _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(t.hi[c].data())));
    const __m256i mask = _mm256_set1_epi8(0x0F);
    std::size_t i = 0;
    for (; i + 64 <= n; i += 64) {
        const __m256i s0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i s1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i + 32));
        const __m256i p0 = _mm256_xor_si256(_mm256_shuffle_epi8(tlo, _mm256_and_si256(s0, mask)),
                                            _mm256_shuffle_epi8(thi, _mm256_and_si256(_mm256_srli_epi64(s0, 4), mask)));
        const __m256i p1 = _mm256_xor_si256(_mm256_shuffle_epi8(tlo, _mm256_and_si256(s1, mask)),
                                            _mm256_shuffle_epi8(thi, _mm256_and_si256(_mm256_srli_epi64(s1, 4), mask)));
        __m256i* d0 = reinterpret_cast<__m256i*>(dst + i);
        __m256i* d1 = reinterpret_cast<__m256i*>(dst + i + 32);
        _mm256_storeu_si256(d0, _mm256_xor_si256(_mm256_loadu_si256(d0), p0));
        _mm256_storeu_si256(d1, _mm256_xor_si256(_mm256_loadu_si256(d1), p1));
    }
    for (; i + 32 <= n; i += 32) {
        const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i p = _mm256_xor_si256(_mm256_shuffle_epi8(tlo, _mm256_and_si256(s, mask)),
                                           _mm256_shuffle_epi8(thi, _mm256_and_si256(_mm256_srli_epi64(s, 4), mask)));
        __m256i* d = reinterpret_cast<__m256i*>(dst + i);
        _mm256_storeu_si256(d, _mm256_xor_si256(_mm256_loadu_si256(d), p));
    }
    return i;
}
#endif

void mul_add(std::uint8_t c, const std::uint8_t* src, std::uint8_t* dst, std::size_t n) {
    if (c == 0 || n == 0) return;
    std::size_t i = 0;
    if (c == 1) {
        for (; i < n; ++i) dst[i] ^= src[i];
        return;
    }
#if defined(VAULTIK_RS_X86)
    if (kHaveAvx2 && g_simd_enabled.load(std::memory_order_relaxed)) i = mul_add_avx2(c, src, dst, n);
#endif
    const std::uint8_t* table = gf().mul[c].data();
    for (; i < n; ++i) dst[i] ^= table[src[i]];
}

// ---- header ------------------------------------------------------------------------------------

using ShardHash = std::array<std::uint8_t, kShardHashSize>;

ShardHash shard_hash(ByteSpan shard) {
    const Id full = blake3(shard);
    ShardHash h;
    std::memcpy(h.data(), full.data(), h.size());
    return h;
}

struct Header {
    std::uint64_t data_size = 0;
    std::size_t shard_size = 0;
    std::size_t k = 0;  // data shards
    std::size_t m = 0;  // parity shards
    Id data_hash{};
    std::vector<ShardHash> hashes;  // k data shard hashes, then m parity shard hashes
    std::size_t header_size = 0;    // where the parity shards start
};

std::size_t header_size_for(std::size_t total_shards) {
    return kFixedHeaderSize + total_shards * kShardHashSize + kHeaderHashSize;
}

// Parses and checks the header. Throws Error when `recovery` is not recovery data or the header
// describes something impossible; the parity shards after it are not looked at here.
Header parse_header(ByteSpan recovery) {
    if (recovery.size() < kFixedHeaderSize || std::memcmp(recovery.data(), kMagic, sizeof kMagic) != 0) {
        throw Error("not recovery data");
    }
    ByteReader r(recovery.subspan(sizeof kMagic));
    const std::uint32_t version = r.u32();
    if (version != kVersion) throw Error("unsupported recovery data version " + std::to_string(version));
    Header h;
    h.data_size = r.u64();
    const std::uint32_t shard_size = r.u32();
    const std::uint32_t k = r.u32();
    const std::uint32_t m = r.u32();
    // Checked before anything is sized from them, so a forged count cannot overflow or allocate.
    if (k == 0 || m == 0 || k > kMaxTotalShards || m > kMaxTotalShards || k + m > kMaxTotalShards) {
        throw Error("recovery data is damaged: bad shard counts");
    }
    h.k = k;
    h.m = m;
    h.shard_size = shard_size;
    h.header_size = header_size_for(h.k + h.m);
    if (recovery.size() < h.header_size) throw Error("recovery data is damaged: header cut short");

    const ByteSpan covered = recovery.first(h.header_size - kHeaderHashSize);
    const Id check = blake3(covered);
    if (std::memcmp(check.data(), recovery.data() + covered.size(), kHeaderHashSize) != 0) {
        throw Error("recovery data is damaged: header checksum mismatch");
    }

    r.raw_into(h.data_hash);
    h.hashes.resize(h.k + h.m);
    for (ShardHash& sh : h.hashes) r.raw_into(sh);

    // The checksum only proves the header was not damaged by accident; a hostile one can carry a
    // valid checksum, so the values must also be possible. These match what make_recovery writes:
    // every shard but the last lies fully inside the data, so k * shard_size < data_size +
    // shard_size, and shard_size never exceeds the data size by more than the padding.
    const std::uint64_t n = h.data_size;
    if (n > kRecoveryMaxDataSize) throw Error("recovery data is damaged: data size out of range");
    if (n == 0) {
        if (shard_size != 0 || k != 1) throw Error("recovery data is damaged: bad shard layout");
    } else {
        const std::uint64_t s = shard_size;
        if (s == 0 || s > n) throw Error("recovery data is damaged: bad shard size");
        if (s * k < n || s * (k - 1) >= n) throw Error("recovery data is damaged: bad shard layout");
    }
    return h;
}

// Largest k for which k plus its parity count fits in 255 shards.
std::size_t max_data_shards(unsigned percent) {
    std::size_t k = kMaxDataShards;
    while (k > 1 && k + (k * percent + 99) / 100 > kMaxTotalShards) --k;
    return k;
}

// Returns the bytes of data shard j from `data`, padded with zeros to the shard size; only the
// last shard can be short, so this copies at most one shard.
ByteSpan padded_shard(ByteSpan data, std::size_t shard_size, std::size_t j, Bytes& pad) {
    const std::size_t begin = j * shard_size;
    if (begin + shard_size <= data.size()) return data.subspan(begin, shard_size);
    pad.assign(shard_size, 0);
    if (begin < data.size()) std::memcpy(pad.data(), data.data() + begin, data.size() - begin);
    return pad;
}

// Inverts the n x n matrix `a` (row-major) over GF(256) by Gauss-Jordan elimination. Returns false
// if it is singular, which cannot happen for rows of [I; Cauchy] but is checked anyway.
bool invert(std::vector<std::uint8_t>& a, std::size_t n, std::vector<std::uint8_t>& inv) {
    inv.assign(n * n, 0);
    for (std::size_t i = 0; i < n; ++i) inv[i * n + i] = 1;
    for (std::size_t col = 0; col < n; ++col) {
        std::size_t pivot = col;
        while (pivot < n && a[pivot * n + col] == 0) ++pivot;
        if (pivot == n) return false;
        if (pivot != col) {
            std::swap_ranges(a.begin() + static_cast<std::ptrdiff_t>(pivot * n), a.begin() + static_cast<std::ptrdiff_t>(pivot * n + n),
                             a.begin() + static_cast<std::ptrdiff_t>(col * n));
            std::swap_ranges(inv.begin() + static_cast<std::ptrdiff_t>(pivot * n), inv.begin() + static_cast<std::ptrdiff_t>(pivot * n + n),
                             inv.begin() + static_cast<std::ptrdiff_t>(col * n));
        }
        const std::uint8_t scale = gf_inv(a[col * n + col]);
        for (std::size_t j = 0; j < n; ++j) {
            a[col * n + j] = gf_mul(a[col * n + j], scale);
            inv[col * n + j] = gf_mul(inv[col * n + j], scale);
        }
        for (std::size_t row = 0; row < n; ++row) {
            const std::uint8_t f = a[row * n + col];
            if (row == col || f == 0) continue;
            for (std::size_t j = 0; j < n; ++j) {
                a[row * n + j] ^= gf_mul(f, a[col * n + j]);
                inv[row * n + j] ^= gf_mul(f, inv[col * n + j]);
            }
        }
    }
    return true;
}

}  // namespace

void set_recovery_simd_enabled(bool enabled) { g_simd_enabled.store(enabled, std::memory_order_relaxed); }

Bytes make_recovery(ByteSpan data, unsigned percent) {
    if (percent < 1 || percent > 50) throw Error("recovery percentage must be between 1 and 50");
    if (data.size() > kRecoveryMaxDataSize) throw Error("file is too large for recovery data");
    const std::size_t n = data.size();

    std::size_t shard_size = 0;
    std::size_t k = 1;
    if (n > 0) {
        const std::size_t k_max = max_data_shards(percent);
        shard_size = std::max((n + k_max - 1) / k_max, std::min(n, kMinShardSize));
        k = (n + shard_size - 1) / shard_size;
    }
    const std::size_t m = std::max<std::size_t>(1, (k * percent + 99) / 100);

    // Parity: for each column block, every data shard's block is multiplied into every parity
    // shard's block. Only the last data shard can be short, so it alone is padded.
    std::vector<Bytes> parity(m, Bytes(shard_size, 0));
    Bytes pad;
    std::vector<const std::uint8_t*> shards(k);
    for (std::size_t j = 0; j < k; ++j) shards[j] = padded_shard(data, shard_size, j, pad).data();
    for (std::size_t off = 0; off < shard_size; off += kBlockSize) {
        const std::size_t len = std::min(kBlockSize, shard_size - off);
        for (std::size_t j = 0; j < k; ++j) {
            for (std::size_t i = 0; i < m; ++i) mul_add(cauchy(k, i, j), shards[j] + off, parity[i].data() + off, len);
        }
    }

    ByteWriter w;
    w.raw(ByteSpan(kMagic, sizeof kMagic));
    w.u32(kVersion);
    w.u64(n);
    w.u32(static_cast<std::uint32_t>(shard_size));
    w.u32(static_cast<std::uint32_t>(k));
    w.u32(static_cast<std::uint32_t>(m));
    w.raw(blake3(data));
    for (std::size_t j = 0; j < k; ++j) w.raw(shard_hash(ByteSpan(shards[j], shard_size)));
    for (const Bytes& p : parity) w.raw(shard_hash(p));
    const Id check = blake3(w.buffer());
    w.raw(ByteSpan(check.data(), kHeaderHashSize));
    Bytes out = w.take();
    out.reserve(out.size() + m * shard_size);
    for (const Bytes& p : parity) out.insert(out.end(), p.begin(), p.end());
    return out;
}

bool recovery_matches(ByteSpan data, ByteSpan recovery) {
    try {
        const Header h = parse_header(recovery);
        return data.size() == h.data_size && blake3(data) == h.data_hash;
    } catch (const Error&) {
        return false;
    }
}

RecoveryResult repair_with_recovery(Bytes& data, ByteSpan recovery) {
    const Header h = parse_header(recovery);
    RecoveryResult result;
    if (data.size() == h.data_size && blake3(data) == h.data_hash) {
        result.intact = true;
        return result;
    }

    const std::size_t n = static_cast<std::size_t>(h.data_size);
    const std::size_t s = h.shard_size;
    const std::size_t k = h.k;

    // Work on a copy laid out as k padded shards, so `data` stays as it was if repair fails.
    // Bytes past the end of a truncated file read as zeros; their shard hash decides whether that
    // happens to be right.
    Bytes work(k * s, 0);
    const std::size_t have = std::min(data.size(), n);
    if (have > 0) std::memcpy(work.data(), data.data(), have);

    std::vector<std::size_t> bad;
    for (std::size_t j = 0; j < k; ++j) {
        if (shard_hash(ByteSpan(work.data() + j * s, s)) != h.hashes[j]) bad.push_back(j);
    }
    result.bad_shards = bad.size();

    if (!bad.empty()) {
        // Parity shards that are cut off or fail their hash are not used.
        std::vector<std::size_t> good_parity;
        for (std::size_t i = 0; i < h.m; ++i) {
            const std::size_t begin = h.header_size + i * s;
            if (recovery.size() < begin || recovery.size() - begin < s) break;
            if (shard_hash(recovery.subspan(begin, s)) == h.hashes[k + i]) good_parity.push_back(i);
        }
        if (bad.size() > good_parity.size()) {
            result.message = std::to_string(bad.size()) + " parts are damaged but only " + std::to_string(good_parity.size()) +
                             " can be rebuilt";
            return result;
        }

        // The k shards used: every intact data shard, then as many intact parity shards as there
        // are damaged data shards. Row r of `a` expresses shard r in terms of the original data.
        std::vector<const std::uint8_t*> sources;
        std::vector<std::uint8_t> a(k * k, 0);
        std::vector<bool> is_bad(k, false);
        for (std::size_t j : bad) is_bad[j] = true;
        std::size_t row = 0;
        for (std::size_t j = 0; j < k; ++j) {
            if (is_bad[j]) continue;
            a[row * k + j] = 1;
            sources.push_back(work.data() + j * s);
            ++row;
        }
        for (std::size_t p = 0; p < bad.size(); ++p) {
            const std::size_t i = good_parity[p];
            for (std::size_t j = 0; j < k; ++j) a[row * k + j] = cauchy(k, i, j);
            sources.push_back(recovery.data() + h.header_size + i * s);
            ++row;
        }
        std::vector<std::uint8_t> inv;
        if (!invert(a, k, inv)) {
            result.message = "the recovery data cannot rebuild the damaged parts";
            return result;
        }
        // Damaged shards are never among the sources, so they can be rebuilt in place.
        for (std::size_t j : bad) {
            std::uint8_t* out = work.data() + j * s;
            std::memset(out, 0, s);
            for (std::size_t r = 0; r < k; ++r) mul_add(inv[j * k + r], sources[r], out, s);
        }
    }

    work.resize(n);
    if (blake3(work) != h.data_hash) {
        result.message = "the rebuilt data does not match the original";
        return result;
    }
    data = std::move(work);
    result.repaired = true;
    return result;
}

}  // namespace vaultik::repo
