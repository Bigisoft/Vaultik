#pragma once

// Disk images: the layout of a whole disk (its partition table and what each partition holds),
// stored with every image backup so the disk can be recreated on another disk or in a virtual
// disk file. See docs/FORMAT.md, "Disk images".

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vaultik/util/bytes.hpp"

namespace vaultik::image {

using Guid = std::array<std::uint8_t, 16>;  // in the on-disk (GPT, VHDX) byte order

// "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" <-> bytes in Windows GUID order (the first three fields
// little-endian, as GPT and VHDX store them). parse_guid throws Error on malformed text.
Guid parse_guid(std::string_view text);
std::string format_guid(const Guid& g);
Guid random_guid();
bool is_zero(const Guid& g);

// CRC-32 (IEEE 802.3, as GPT uses) and CRC-32C (Castagnoli, as VHDX uses).
std::uint32_t crc32(ByteSpan data, std::uint32_t crc = 0);
std::uint32_t crc32c(ByteSpan data, std::uint32_t crc = 0);

// Well-known GPT partition types.
inline constexpr std::string_view kEfiSystemType = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B";
inline constexpr std::string_view kMsrType = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE";
inline constexpr std::string_view kBasicDataType = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
inline constexpr std::string_view kRecoveryType = "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC";

enum class PartitionStyle { Mbr, Gpt };

// How a partition's content was read.
enum class ReadMethod {
    Used,  // NTFS: only the clusters in use (from a shadow copy when one could be made); the rest is zeros
    Raw,   // every byte (FAT32 EFI system partitions, MSR, unknown file systems)
};

struct Partition {
    int number = 0;              // the partition number Windows gave it (1, 2, ...)
    std::uint64_t offset = 0;    // bytes from the start of the disk
    std::uint64_t length = 0;    // bytes
    // GPT
    std::string type;            // partition type GUID
    std::string guid;            // unique partition GUID
    std::string name;            // "EFI system partition", "Basic data partition"
    std::uint64_t attributes = 0;
    // MBR
    std::uint8_t mbr_type = 0;   // 0x07 NTFS, 0x0C FAT32, 0x27 recovery, ...
    bool active = false;         // the boot flag
    // What it holds, as Windows saw it during the backup (for display; restores copy the bytes).
    std::string filesystem;      // "NTFS", "FAT32", "" when unknown
    std::string label;
    std::string mount;           // "C:\" when it had a drive letter
    std::uint32_t serial = 0;    // the volume serial number
    std::uint32_t cluster_size = 0;
    bool windows = false;        // the volume Windows ran from
    // How it was stored.
    ReadMethod method = ReadMethod::Raw;
    bool shadow_copy = false;    // read from a shadow copy (consistent), not live
    std::uint64_t used_bytes = 0;  // bytes read (the rest stored as zeros)
    // Byte ranges (from the partition start) that were not in use and are stored as zeros. A
    // restore to a disk that is not empty does not need to write them (Used partitions only).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> holes;
};

// A range of the disk outside every partition that is stored as it is (at most 1 MiB at the
// start of each gap and, on MBR disks, at the start of each container: the MBR with its boot
// code, the GPT, the EBRs of logical partitions, a boot loader between the MBR and the first
// partition).
struct Region {
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

struct DiskLayout {
    int version = 1;
    PartitionStyle style = PartitionStyle::Gpt;
    std::uint32_t sector_size = 512;           // logical
    std::uint32_t physical_sector_size = 512;
    std::uint64_t size = 0;                    // bytes
    std::string disk_guid;                     // GPT
    std::uint32_t signature = 0;               // MBR disk signature
    std::uint32_t gpt_entries = 128;           // GPT: entries in the partition array
    // The disk as Windows named it during the backup (for display).
    int number = -1;
    std::string model;
    std::string serial;
    bool system = false;                       // the disk Windows ran from
    std::string id;                            // stable id: "gpt-<disk GUID>" or "mbr-<signature>"
    std::vector<Partition> partitions;         // in disk order
    std::vector<Region> regions;
};

std::string to_json(const DiskLayout& layout);
// Throws Error when the text is not a layout this version understands.
DiskLayout layout_from_json(std::string_view text);

// The stable id of a disk ("gpt-<GUID>", "mbr-1A2B3C4D").
std::string disk_id(const DiskLayout& layout);

// Extended (container) MBR partitions: their space is described by the logical partitions in it.
bool is_container(std::uint8_t mbr_type);

// The parts of the disk outside every partition to store: at most `cap` bytes from the start of
// each gap and, on MBR disks, from the start of each container in a gap (its EBR). Sorted, never
// overlapping.
std::vector<Region> gap_regions(const DiskLayout& layout, std::uint64_t cap = 1u << 20);

// Bytes to write at an offset of the target disk.
struct DiskWrite {
    std::uint64_t offset = 0;
    Bytes data;
};

// The partition tables for a target of `target_size` bytes. GPT: a protective MBR, the primary
// header and entries at the start and the backup copy at the end (written for the target's own
// size, so a larger disk is fine). MBR: nothing (the stored first region holds the table as it was).
// Throws Error when a partition does not fit on the target.
std::vector<DiskWrite> partition_tables(const DiskLayout& layout, std::uint64_t target_size);

// Clears, in `data` (stored bytes of the original disk from `offset` on), the partition tables
// of the layout's disk: the MBR's four entries and, for GPT, the primary and backup headers with
// their entries. A restore writes the stored regions like this and the tables last, so a restore
// that stops halfway leaves a disk without partitions rather than with half-written ones.
void blank_tables(const DiskLayout& layout, std::uint64_t offset, Bytes& data);

// What restoring onto a disk of `target_size` bytes with `sector_size`-byte sectors needs: an
// empty string when it fits, otherwise why not.
std::string fits(const DiskLayout& layout, std::uint64_t target_size, std::uint32_t sector_size);

// The end of the last partition (the smallest disk the layout fits on, without the backup GPT).
std::uint64_t used_end(const DiskLayout& layout);

// A copy with new disk and partition GUIDs (GPT) or a new disk signature (MBR), so the restored
// disk can be attached next to the original without Windows taking it offline for a collision.
void renew_ids(DiskLayout& layout);

// "C: Windows (NTFS, 120 GB)" style one-liners for display.
std::string describe(const Partition& p);

}  // namespace vaultik::image
