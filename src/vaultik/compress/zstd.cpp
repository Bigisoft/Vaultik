#include "vaultik/compress/zstd.hpp"

#include <zstd.h>

#include <string>

namespace vaultik {

namespace {

void check(std::size_t rc, const char* what) {
    if (ZSTD_isError(rc)) throw Error(std::string(what) + ": " + ZSTD_getErrorName(rc));
}

}  // namespace

ZstdCompressor::ZstdCompressor(int level) : ctx_(ZSTD_createCCtx()), level_(level) {
    if (ctx_ == nullptr) throw Error("ZSTD_createCCtx failed");
}

ZstdCompressor::~ZstdCompressor() {
    ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(ctx_));
}

std::size_t ZstdCompressor::compress_append(ByteSpan in, Bytes& out) {
    const std::size_t base = out.size();
    out.resize(base + ZSTD_compressBound(in.size()));
    const std::size_t rc =
        ZSTD_compressCCtx(static_cast<ZSTD_CCtx*>(ctx_), out.data() + base, out.size() - base, in.data(), in.size(),
                          level_);
    check(rc, "zstd compression failed");
    out.resize(base + rc);
    return rc;
}

ZstdDecompressor::ZstdDecompressor() : ctx_(ZSTD_createDCtx()) {
    if (ctx_ == nullptr) throw Error("ZSTD_createDCtx failed");
}

ZstdDecompressor::~ZstdDecompressor() {
    ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(ctx_));
}

Bytes ZstdDecompressor::decompress(ByteSpan in, std::size_t raw_len) {
    Bytes out(raw_len);
    const std::size_t rc =
        ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(ctx_), out.data(), out.size(), in.data(), in.size());
    check(rc, "zstd decompression failed");
    if (rc != raw_len) throw Error("zstd decompression produced an unexpected length");
    return out;
}

}  // namespace vaultik
