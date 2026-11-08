#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

// Storage for repository files. Names are relative, '/'-separated (for example "data/ab/ab12...").
// Implementations: LocalBackend (local disk, USB drive, network share). S3 comes in phase 3.
class Backend {
public:
    virtual ~Backend() = default;

    // Human-readable location, for messages.
    virtual std::string location() const = 0;

    virtual bool exists(const std::string& name) = 0;

    // Creates or replaces `name` atomically: readers see the old file or the complete new one.
    virtual void put(const std::string& name, ByteSpan data) = 0;

    virtual Bytes get(const std::string& name) = 0;
    virtual Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) = 0;
    virtual std::uint64_t size(const std::string& name) = 0;

    // All file names under `dir`, recursively, as full relative names. Temporary files are skipped.
    virtual std::vector<std::string> list(const std::string& dir) = 0;

    virtual void remove(const std::string& name) = 0;
};

}  // namespace vaultik
