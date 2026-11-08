#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/util/error.hpp"

namespace vaultik {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;

inline ByteSpan as_bytes(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// Little-endian serializer used by every on-disk structure (see docs/FORMAT.md).
class ByteWriter {
public:
    void u8(std::uint8_t v) { buf_.push_back(v); }

    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void u64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }

    void varint(std::uint64_t v) {
        while (v >= 0x80) {
            buf_.push_back(static_cast<std::uint8_t>(v | 0x80));
            v >>= 7;
        }
        buf_.push_back(static_cast<std::uint8_t>(v));
    }

    void raw(ByteSpan s) { buf_.insert(buf_.end(), s.begin(), s.end()); }

    void str(std::string_view s) {
        varint(s.size());
        raw(as_bytes(s));
    }

    const Bytes& buffer() const { return buf_; }
    Bytes take() { return std::move(buf_); }

private:
    Bytes buf_;
};

// Bounds-checked reader matching ByteWriter. Throws Error on malformed input.
class ByteReader {
public:
    explicit ByteReader(ByteSpan data) : data_(data) {}

    std::uint8_t u8() {
        need(1);
        return data_[pos_++];
    }

    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return v;
    }

    std::uint64_t u64() {
        need(8);
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(data_[pos_ + i]) << (8 * i);
        pos_ += 8;
        return v;
    }

    std::uint64_t varint() {
        std::uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            const std::uint8_t b = u8();
            v |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0) return v;
        }
        throw Error("malformed data: varint too long");
    }

    ByteSpan raw(std::size_t n) {
        need(n);
        ByteSpan s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

    template <std::size_t N>
    void raw_into(std::array<std::uint8_t, N>& out) {
        ByteSpan s = raw(N);
        std::memcpy(out.data(), s.data(), N);
    }

    std::string str() {
        const std::uint64_t n = varint();
        if (n > remaining()) throw Error("malformed data: string length out of range");
        ByteSpan s = raw(static_cast<std::size_t>(n));
        return {reinterpret_cast<const char*>(s.data()), s.size()};
    }

    // Reads a count that must be plausible given the bytes left (each item >= min_item_size).
    std::size_t count(std::size_t min_item_size) {
        const std::uint64_t n = varint();
        if (min_item_size > 0 && n > remaining() / min_item_size) throw Error("malformed data: count out of range");
        return static_cast<std::size_t>(n);
    }

    std::size_t remaining() const { return data_.size() - pos_; }

    void expect_done() const {
        if (pos_ != data_.size()) throw Error("malformed data: trailing bytes");
    }

private:
    void need(std::size_t n) const {
        if (n > remaining()) throw Error("malformed data: unexpected end");
    }

    ByteSpan data_;
    std::size_t pos_ = 0;
};

}  // namespace vaultik
