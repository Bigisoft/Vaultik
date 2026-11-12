#pragma once

// Shrinking an NTFS volume for a restore onto a smaller disk, without moving any of its data: the
// volume can lose only the clusters after the last one in use. What changes is what ntfsresize
// changes when it shrinks a volume that needs no relocation: the boot sector (and its backup at
// the new end), $Bitmap (its size and runs, and the bits past the new end), the sparse $Bad
// stream of $BadClus (its size), $LogFile (emptied, so Windows replays nothing against the
// changed metadata) and the dirty flag of $Volume (so Windows checks the volume once when it
// first mounts it). Before anything is planned, every file record in use is read and every run
// of every non-resident attribute checked to end before the new end, so a volume whose bitmap
// does not match its files is refused rather than cut.

#include <cstdint>
#include <functional>
#include <vector>

#include "vaultik/image/extents.hpp"
#include "vaultik/image/layout.hpp"

namespace vaultik::image {

// Reads bytes of the volume as it was backed up (offsets from the partition start, any
// alignment). Throws Error when it cannot.
using VolumeReader = std::function<void(std::uint64_t offset, std::uint8_t* dst, std::size_t length)>;

// What the boot sector and $Bitmap of a volume say.
struct NtfsFacts {
    std::uint32_t sector_size = 0;
    std::uint32_t cluster_size = 0;
    std::uint32_t record_size = 0;     // bytes per file record
    std::uint64_t total_sectors = 0;   // of the volume (the backup boot sector follows them)
    std::uint64_t clusters = 0;
    std::uint64_t used_end = 0;        // the last cluster in use + 1
    std::uint64_t used_clusters = 0;   // clusters in use
};

// Throws Error when the partition does not hold an NTFS volume this code can shrink.
NtfsFacts ntfs_facts(const VolumeReader& read, std::uint64_t partition_length);

// The smallest partition length, a multiple of `align` bytes, the volume fits in without moving
// data, keeping at least `reserve` bytes free.
std::uint64_t ntfs_min_length(const NtfsFacts& f, std::uint64_t align, std::uint64_t reserve = 0);

// Writes to a restored copy of a volume, in addition to the stored bytes.
struct VolumePatch {
    std::vector<DiskWrite> writes;  // offsets from the partition start, whole sectors
    Ranges fill;                    // ranges (from the partition start) to fill with 0xFF bytes
    bool empty() const { return writes.empty() && fill.empty(); }
};

// The writes that turn a restored copy of the volume (stored bytes up to `new_length`) into a
// volume for a partition of `new_length` bytes that starts at sector `new_first_sector` (it
// started at `old_first_sector`). Throws Error, before anything is written, when the volume
// cannot be cut there without moving data or its metadata is not as expected.
VolumePatch ntfs_shrink(const VolumeReader& read, std::uint64_t old_length, std::uint64_t new_length,
                        std::uint64_t old_first_sector, std::uint64_t new_first_sector);

// For an NTFS volume whose partition moves unchanged from `old_first_sector` to
// `new_first_sector`: both boot sectors with the new start ("hidden sectors"). Empty when the
// partition holds no NTFS volume or its boot sector did not record the old start.
VolumePatch ntfs_move(const VolumeReader& read, std::uint64_t length, std::uint64_t old_first_sector,
                      std::uint64_t new_first_sector);

}  // namespace vaultik::image
