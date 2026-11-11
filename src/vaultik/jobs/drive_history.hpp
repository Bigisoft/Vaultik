#pragma once

// A record of each drive's health over time, so a drive that is getting worse (a few more bad
// sectors every week, a falling health percentage) is noticed while it still works. The readings
// are kept in <config dir>/drive-history.jsonl, one JSON object per line.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/drive_health.hpp"

namespace vaultik::jobs {

// What one look at a drive showed. Every count is optional: a reading without administrator
// rights has no S.M.A.R.T. table, and such a gap means "not known", never zero.
struct DriveReading {
    std::uint64_t time = 0;  // os::now() units
    std::string key;         // the drive's identity (see drive_key)
    std::string model;
    std::optional<int> health;              // health_percent()
    std::optional<std::uint64_t> reallocated;    // S.M.A.R.T. 5
    std::optional<std::uint64_t> pending;        // S.M.A.R.T. 197
    std::optional<std::uint64_t> uncorrectable;  // S.M.A.R.T. 198
    std::optional<std::uint64_t> read_errors;    // unrecoverable read errors / NVMe media errors
};

// The drive's identity across runs and drive letters: its serial number, or (when the system
// gives none) its model and size.
std::string drive_key(const os::DriveHealth& d);
DriveReading reading_of(const os::DriveHealth& d, std::uint64_t now);

// Stores the readings of these drives. A drive's reading is only added when something known in
// it differs from its last stored reading, or that one is a day old, so frequent looks (the
// frontend checks every few minutes) do not grow the file. The file is trimmed to the newest
// readings of each drive when it grows past 1 MB. Best effort: never throws.
void record_drives(const std::vector<os::DriveHealth>& drives, const std::string& dir);
// The same in the right place for this process: the user's config folder, or for a process with
// administrator rights the account's admin store (after checking it is admin-only; nothing is
// stored when it is not), since a privileged write into a folder the user can change could be
// redirected by a planted link.
void record_drives(const std::vector<os::DriveHealth>& drives);
// The stored readings of one drive in `dir`, oldest first.
std::vector<DriveReading> drive_history(const std::string& key, const std::string& dir);
// Those of the user's config folder and the admin store together, oldest first.
std::vector<DriveReading> drive_history(const std::string& key);
// Every stored reading of every drive in `dir`, or in both places, oldest first.
std::vector<DriveReading> all_drive_readings(const std::string& dir);
std::vector<DriveReading> all_drive_readings();

// One way a drive changed within the window.
struct DriveChange {
    enum class What { Health, Reallocated, Pending, Uncorrectable, ReadErrors };
    What what = What::Health;
    std::uint64_t from = 0, to = 0;
    std::uint64_t since = 0;  // time of the reading compared with
};

// How the drive changed between the oldest reading in the window (90 days by default) and
// `now`: more bad sectors or errors, or a health percentage at least 5 points lower. Empty when
// it did not get worse (or there is nothing to compare with).
std::vector<DriveChange> drive_trend(const std::vector<DriveReading>& history, const DriveReading& now,
                                     std::uint64_t window = 90ull * 864'000'000'000ull);
// "3 more reallocated sectors since 2026-10-02 (0 -> 3)", "health fell from 100% to 94% since ...".
std::string describe(const DriveChange& c);

}  // namespace vaultik::jobs
