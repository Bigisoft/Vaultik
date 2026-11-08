#pragma once

#include <cstdint>
#include <functional>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

struct ChunkerParams {
    std::uint32_t min_size = 256 * 1024;
    std::uint32_t avg_size = 1024 * 1024;  // must be a power of two
    std::uint32_t max_size = 4 * 1024 * 1024;

    // Throws Error when the parameters are unusable.
    void validate() const;
};

// FastCDC content-defined chunking with normalization level 2 (see docs/FORMAT.md).
class FastCdc {
public:
    explicit FastCdc(const ChunkerParams& params);

    const ChunkerParams& params() const { return params_; }

    // Length of the first chunk in `data`. Precondition: data.size() >= max_size, or `data` is the
    // final remainder of the stream.
    std::size_t cut(ByteSpan data) const;

private:
    ChunkerParams params_;
    std::uint64_t mask_s_;
    std::uint64_t mask_l_;
};

// Splits a stream into chunks using a caller-owned buffer, so memory stays at 2 * max_size no
// matter how large the input is.
class ChunkReader {
public:
    // Fills dst with up to `max` bytes; returns 0 at end of stream.
    using ReadFn = std::function<std::size_t(std::uint8_t* dst, std::size_t max)>;

    // `buffer` is resized to 2 * max_size if it is smaller, and reused across files.
    ChunkReader(const FastCdc& cdc, ReadFn read, Bytes& buffer);

    // Returns false at end of stream. The chunk view stays valid until the next call.
    bool next(ByteSpan& chunk);

private:
    void fill();

    const FastCdc& cdc_;
    ReadFn read_;
    Bytes& buf_;
    std::size_t start_ = 0;
    std::size_t end_ = 0;
    bool eof_ = false;
};

}  // namespace vaultik
