#pragma once

// Disk image backup and restore (see docs/FORMAT.md, "Disk images"). A disk is stored as an
// ordinary snapshot tree, so deduplication, prune, verify, copy and repair work on it unchanged:
//
//   disk0/layout.json          the DiskLayout (layout.hpp) as JSON
//   disk0/r0, r1, ...          the stored regions outside the partitions (MBR, GPT, boot loaders)
//   disk0/p0/g0000/s000000     partition 0, in segments of 1 GiB (64 per group folder)
//
// Unused clusters of NTFS partitions are stored as zeros; long runs of them as zero chunks that
// cost nothing (one blob per size).

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/image/extents.hpp"
#include "vaultik/image/layout.hpp"
#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik::image {

// Where an image backup reads a disk from.
class DiskSource {
public:
    virtual ~DiskSource() = default;
    // The layout, with each partition's method, shadow copy and volume details filled in.
    virtual DiskLayout& layout() = 0;
    // What to store of partition `index`: the extents covering it (one Data extent for Raw).
    virtual std::vector<Extent> plan(std::size_t index) = 0;
    // Reads partition bytes; offset and length are multiples of the sector size. Called from one
    // thread at a time, though not always the same one (reads run ahead of the chunking).
    virtual void read(std::size_t index, std::uint64_t offset, std::uint8_t* dst, std::size_t length) = 0;
    // Reads bytes of the disk itself (the stored regions); sector-aligned.
    virtual void read_disk(std::uint64_t offset, std::uint8_t* dst, std::size_t length) = 0;
};

// Where a restore writes a disk to.
class DiskTarget {
public:
    virtual ~DiskTarget() = default;
    virtual std::uint64_t size() const = 0;
    virtual std::uint32_t sector_size() const = 0;
    // Whether every byte reads as zero before the restore (a new virtual disk), so zeros need not
    // be written at all.
    virtual bool zeroed() const = 0;
    // The alignment writes need (the sector size for a disk, 1 for a file).
    virtual std::uint32_t alignment() const = 0;
    virtual void write(std::uint64_t offset, ByteSpan data) = 0;
    // Called once everything is written (the partition tables last).
    virtual void finish() = 0;
};

struct ImageProgress {
    std::uint64_t done = 0;       // bytes of the disks gone through
    std::uint64_t total = 0;      // bytes of all partitions and regions
    std::uint64_t read = 0;       // bytes read from the disks (backup) or written (restore)
    std::uint64_t new_bytes = 0;  // backup: data not in the repository yet
    std::string current;          // "C: Windows (NTFS, 500 GB)"
};

struct ImageBackupOptions {
    std::vector<DiskSource*> disks;
    std::vector<std::string> tags;
    std::optional<Id> parent;  // the previous image of these disks (shown as the version before)
    std::function<void(const ImageProgress&)> on_progress;  // at most ~5 times per second
    std::function<void(const std::string&)> on_warning;
    std::function<bool()> should_cancel;  // polled often; true stops with Cancelled
    std::function<void()> before_save;    // may throw to discard the snapshot
};

struct ImageBackupResult {
    Id snapshot_id{};
    Snapshot snapshot;
};

// The snapshot path of an image of a disk ("image:gpt-<GUID>").
std::string image_path(const DiskLayout& layout);

ImageBackupResult run_image_backup(Repository& repo, const ImageBackupOptions& options);

// The disks in an image snapshot (throws Error when it is not one).
std::vector<DiskLayout> image_disks(Repository& repo, const Snapshot& snapshot);

struct ImageRestoreOptions {
    std::size_t disk = 0;  // which disk of the snapshot
    // New disk and partition ids, so the copy can be attached next to the original.
    bool new_ids = false;
    std::function<void(const ImageProgress&)> on_progress;
    std::function<bool()> should_cancel;
};

// Writes the disk to `target`: the stored regions, every partition, then the partition tables.
// Throws Error when the disk does not fit, Cancelled when stopped.
void run_image_restore(Repository& repo, const Snapshot& snapshot, DiskTarget& target,
                       const ImageRestoreOptions& options);

}  // namespace vaultik::image
