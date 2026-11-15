#pragma once

#include "vaultik/util/bytes.hpp"

namespace vaultik {

// Reusable zstd compression context (one per thread).
class ZstdCompressor {
public:
    explicit ZstdCompressor(int level);
    ~ZstdCompressor();
    ZstdCompressor(const ZstdCompressor&) = delete;
    ZstdCompressor& operator=(const ZstdCompressor&) = delete;

    // Appends the compressed form of `in` to `out`. Returns the number of bytes appended.
    std::size_t compress_append(ByteSpan in, Bytes& out);
    // The level for what is compressed from now on (any level reads back the same way).
    void set_level(int level) { level_ = level; }
    int level() const { return level_; }

private:
    void* ctx_;  // ZSTD_CCtx*
    int level_;
};

// Reusable zstd decompression context (one per thread).
class ZstdDecompressor {
public:
    ZstdDecompressor();
    ~ZstdDecompressor();
    ZstdDecompressor(const ZstdDecompressor&) = delete;
    ZstdDecompressor& operator=(const ZstdDecompressor&) = delete;

    // Decompresses `in`, which must expand to exactly `raw_len` bytes.
    Bytes decompress(ByteSpan in, std::size_t raw_len);

private:
    void* ctx_;  // ZSTD_DCtx*
};

}  // namespace vaultik
