#pragma once

#include <cstddef>
#include <string>

#include "vaultik/util/bytes.hpp"

namespace vaultik::repo {

// Recovery data lets a damaged file (usually a pack file) be rebuilt, like a .par2 file: the data
// is split into k contiguous shards and m Reed-Solomon parity shards are computed over GF(2^8), so
// any k intact shards out of the k + m rebuild the original. Every shard has its own hash, so the
// damaged ones are found without guessing. See docs/FORMAT.md, "Recovery data".

// Largest input make_recovery accepts and repair_with_recovery believes a header about.
constexpr std::size_t kRecoveryMaxDataSize = std::size_t{1} << 30;

// Recovery data for `data`: `percent` (1..50) of its size as Reed-Solomon parity, plus a hash of
// every shard so damaged ones can be found. Self-describing (magic, version, sizes, hashes).
// Throws Error when `percent` is out of range or `data` is larger than kRecoveryMaxDataSize.
Bytes make_recovery(ByteSpan data, unsigned percent);

struct RecoveryResult {
    bool intact = false;         // data already matched, nothing done
    bool repaired = false;       // data was damaged and is now exactly the original
    std::size_t bad_shards = 0;  // data shards found damaged (or missing because the file was cut short)
    std::string message;         // why it could not be repaired, when !intact && !repaired
};

// Checks `data` against `recovery` and, when it is damaged but recoverable, rebuilds it in place
// (also when it is shorter or longer than the original: truncated or extended). `data` is left
// unchanged when it cannot be repaired. Never throws for damaged input data; throws Error only
// when `recovery` itself is not recovery data at all (bad magic/version/header checksum, or a
// header whose values are impossible). Damaged parity shards inside `recovery` are detected by
// their hashes and simply not used.
RecoveryResult repair_with_recovery(Bytes& data, ByteSpan recovery);

// Cheap check: whether `recovery` describes `data` (header size + whole-data hash). Returns false
// for anything that is not valid recovery data instead of throwing.
bool recovery_matches(ByteSpan data, ByteSpan recovery);

// Tests only: turns the vector (AVX2) code path off or on, to check both give the same bytes.
// Has no effect on CPUs without AVX2, which always use the scalar path.
void set_recovery_simd_enabled(bool enabled);

}  // namespace vaultik::repo
