#include "vaultik/chunker/fastcdc.hpp"

#include <array>
#include <bit>
#include <cstring>

namespace vaultik {

namespace {

constexpr std::array<std::uint64_t, 256> make_gear() {
    std::array<std::uint64_t, 256> table{};
    std::uint64_t x = 0x5641554C54494B31ULL;  // "VAULTIK1"
    for (auto& entry : table) {
        x += 0x9E3779B97F4A7C15ULL;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        entry = z ^ (z >> 31);
    }
    return table;
}

constexpr std::array<std::uint64_t, 256> kGear = make_gear();

constexpr std::uint64_t top_bits(unsigned n) {
    return n == 0 ? 0 : (~0ULL << (64 - n));
}

}  // namespace

void ChunkerParams::validate() const {
    if (!std::has_single_bit(avg_size)) throw Error("chunker: average size must be a power of two");
    if (avg_size < 4096 || avg_size > (64u << 20)) throw Error("chunker: average size out of range");
    if (min_size == 0 || min_size >= avg_size) throw Error("chunker: minimum size must be below the average");
    if (max_size <= avg_size || max_size > (256u << 20)) throw Error("chunker: maximum size out of range");
}

FastCdc::FastCdc(const ChunkerParams& params) : params_(params) {
    params_.validate();
    const unsigned bits = static_cast<unsigned>(std::countr_zero(params_.avg_size));
    mask_s_ = top_bits(bits + 2);
    mask_l_ = top_bits(bits - 2);
}

std::size_t FastCdc::cut(ByteSpan data) const {
    std::size_t n = data.size();
    if (n <= params_.min_size) return n;
    if (n > params_.max_size) n = params_.max_size;
    const std::size_t normal = n < params_.avg_size ? n : params_.avg_size;

    std::uint64_t h = 0;
    std::size_t i = params_.min_size;
    const std::uint8_t* p = data.data();
    for (; i < normal; ++i) {
        h = (h << 1) + kGear[p[i]];
        if ((h & mask_s_) == 0) return i + 1;
    }
    for (; i < n; ++i) {
        h = (h << 1) + kGear[p[i]];
        if ((h & mask_l_) == 0) return i + 1;
    }
    return n;
}

ChunkReader::ChunkReader(const FastCdc& cdc, ReadFn read, Bytes& buffer)
    : cdc_(cdc), read_(std::move(read)), buf_(buffer) {
    const std::size_t want = static_cast<std::size_t>(cdc_.params().max_size) * 2;
    if (buf_.size() < want) buf_.resize(want);
}

void ChunkReader::fill() {
    if (start_ > 0) {
        const std::size_t remain = end_ - start_;
        if (remain > 0) std::memmove(buf_.data(), buf_.data() + start_, remain);
        start_ = 0;
        end_ = remain;
    }
    while (!eof_ && end_ < buf_.size()) {
        const std::size_t got = read_(buf_.data() + end_, buf_.size() - end_);
        if (got == 0) {
            eof_ = true;
        } else {
            end_ += got;
        }
    }
}

bool ChunkReader::next(ByteSpan& chunk) {
    if (end_ - start_ < cdc_.params().max_size && !eof_) fill();
    const std::size_t remain = end_ - start_;
    if (remain == 0) return false;
    const std::size_t len = cdc_.cut(ByteSpan(buf_.data() + start_, remain));
    chunk = ByteSpan(buf_.data() + start_, len);
    start_ += len;
    return true;
}

}  // namespace vaultik
