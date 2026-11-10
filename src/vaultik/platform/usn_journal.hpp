#pragma once

// The NTFS/ReFS USN change journal: lets a backup learn which files and folders of a volume changed
// since the previous backup, so folders in which nothing changed need not be scanned again.
//
// File ids and shadow copies: the id of a file is its NTFS file reference (MFT record number plus
// sequence number) or, on ReFS, the 128-bit id. A Volume Shadow Copy is a block-level snapshot of
// the volume, so it holds the same MFT records and the ids read through a shadow copy path
// (\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN\...) are expected to equal those of the live
// volume (about 90 percent sure; Microsoft documents the id as stable for the life of the file on
// the volume but does not say it in so many words for snapshots). What differs is the volume
// serial number inside FILE_ID_INFO, which is why FileRef leaves it out. A caller that relies on
// it must check once against a real shadow copy before trusting it.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_set>

namespace vaultik::os {

// A file's identity on its volume: the 128-bit file id (FILE_ID_INFO; NTFS 64-bit reference
// numbers are zero-extended), plus hashing so it can go into unordered containers.
struct FileRef {
    std::uint64_t low = 0, high = 0;
    bool operator==(const FileRef&) const = default;
};

struct FileRefHash {
    std::size_t operator()(const FileRef& ref) const noexcept {
        return static_cast<std::size_t>((ref.low ^ (ref.high * 0xFF51AFD7ED558CCDull)) * 0x9E3779B97F4A7C15ull >> 7);
    }
};

struct UsnJournalState {
    std::uint64_t journal_id = 0;
    std::int64_t first_usn = 0;
    std::int64_t next_usn = 0;
};

// The change journal of the volume that holds `path` (any path on it). Needs administrator rights
// (the volume is opened). nullopt when the volume keeps no journal (FAT/exFAT, or the journal is
// off). Throws Error on other failures, e.g. access denied when not elevated.
std::optional<UsnJournalState> usn_journal_state(const std::string& path);

struct UsnChanges {
    std::unordered_set<FileRef, FileRefHash> changed;  // every file or folder with a record
    std::unordered_set<FileRef, FileRefHash> parents;  // the folder each record's item is in
    std::int64_t next_usn = 0;                         // where reading stopped
};

// Every record of the journal from `since` up to `until` (exclusive; usually the next_usn of a
// fresh usn_journal_state). nullopt when the journal can no longer answer for that range: its id
// is not `journal_id` any more (deleted and recreated), or `since` is older than its first
// record (it wrapped), so the caller must fall back to a full scan.
std::optional<UsnChanges> usn_changes_since(const std::string& path, std::uint64_t journal_id, std::int64_t since,
                                            std::int64_t until);

// The FileRef of `path` (opened with no access rights needed beyond reading attributes,
// FILE_FLAG_BACKUP_SEMANTICS so folders work, not following a final reparse point). Throws Error.
FileRef file_ref(const std::string& path);

}  // namespace vaultik::os
