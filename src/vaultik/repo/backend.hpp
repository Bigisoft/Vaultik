#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik {

// Storage for repository files. Names are relative, '/'-separated (for example "data/ab/ab12...").
// Implementations: LocalBackend (local disk, USB drive, network share), S3Backend and the cloud
// drives (backends.hpp).
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

    // Archive storage (S3 storage classes): packs of file data and their repair data may be kept
    // in a cheaper storage class than everything else. The class for them, empty when they are
    // stored like everything else (every backend but S3).
    virtual std::string data_class() const { return {}; }
    // Whether files in that class must be restored for a while (thawed) before they can be read;
    // reading one before throws ArchivedError.
    virtual bool data_needs_thaw() const { return false; }
    // Writes a pack of file data or its repair data, in data_class() when there is one.
    virtual void put_data(const std::string& name, ByteSpan data) { put(name, data); }
};

}  // namespace vaultik
