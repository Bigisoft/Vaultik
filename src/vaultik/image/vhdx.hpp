#pragma once

// Dynamic VHDX virtual disk files (the format Hyper-V and Windows' "Mount" use; [MS-VHDX]),
// written directly so a disk image can be restored to a file without administrator rights. Only
// the parts of the disk that hold data take space in the file.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vaultik/image/layout.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik::image {

// A file read and written at offsets.
class RandomFile {
public:
    virtual ~RandomFile() = default;
    virtual void write_at(std::uint64_t offset, ByteSpan data) = 0;
    // Throws Error when the range is not all there.
    virtual void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) = 0;
    virtual std::uint64_t size() = 0;
    // Grows (with zeros) or cuts the file.
    virtual void resize(std::uint64_t size) = 0;
    virtual void flush() {}
};

// A new file at `path` (it must not exist), or an existing one opened for reading only.
std::unique_ptr<RandomFile> create_random_file(const std::string& path);
std::unique_ptr<RandomFile> open_random_file(const std::string& path);

// In memory (tests).
class MemoryFile : public RandomFile {
public:
    void write_at(std::uint64_t offset, ByteSpan data) override;
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) override;
    std::uint64_t size() override { return bytes.size(); }
    void resize(std::uint64_t size) override { bytes.resize(static_cast<std::size_t>(size)); }
    Bytes bytes;
};

constexpr std::uint32_t kVhdxBlockSize = 32u << 20;

class VhdxWriter {
public:
    // A new, empty (all zeros) virtual disk of `virtual_size` bytes in the empty file `file`.
    VhdxWriter(RandomFile& file, std::uint64_t virtual_size, std::uint32_t logical_sector = 512,
               std::uint32_t physical_sector = 4096, std::uint32_t block_size = kVhdxBlockSize);

    std::uint64_t virtual_size() const { return size_; }
    // Writes bytes of the virtual disk. Blocks that only ever get zeros take no space.
    void write(std::uint64_t offset, ByteSpan data);
    // Writes the block table and headers. The file is a valid VHDX only after this.
    void finish();
    std::uint64_t allocated_blocks() const { return allocated_; }

private:
    RandomFile& file_;
    std::uint64_t size_;
    std::uint32_t logical_;
    std::uint32_t physical_;
    std::uint32_t block_;
    std::uint64_t chunk_ratio_;
    std::vector<std::uint64_t> blocks_;  // file offset of each payload block, 0 when not present
    std::uint64_t bat_offset_ = 0;
    std::uint64_t bat_length_ = 0;
    std::uint64_t end_ = 0;  // end of the file (next block goes there)
    std::uint64_t allocated_ = 0;
    bool finished_ = false;
};

// Reads a dynamic or fixed VHDX without a parent (tests, and checking a restore).
class VhdxReader {
public:
    explicit VhdxReader(RandomFile& file);  // throws Error when it is not such a file
    std::uint64_t virtual_size() const { return size_; }
    std::uint32_t logical_sector() const { return logical_; }
    std::uint32_t block_size() const { return block_; }
    void read(std::uint64_t offset, std::uint8_t* dst, std::size_t length);

private:
    RandomFile& file_;
    std::uint64_t size_ = 0;
    std::uint32_t logical_ = 512;
    std::uint32_t block_ = 0;
    std::uint64_t chunk_ratio_ = 0;
    std::vector<std::uint64_t> bat_;
};

}  // namespace vaultik::image
