#pragma once

// What is still unprotected: the gaps in a job's protection that the Overview lists in plain
// words, each with the button that leads to its fix.

#include <cstdint>
#include <string>
#include <vector>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

struct ProtectionGap {
    // "password", "second-copy", "kit", "test-restore", "paused", "check-failed", "check-overdue",
    // "checks-off", "cloud-only", "one-drive", "same-drive"
    std::string id;
    std::string text;  // one plain sentence
    // What the button does: "edit" (the job's settings), "kit" (save the recovery kit), "test" (a
    // test restore), "resume" (the schedule), "check" (a health check now); empty: no button.
    std::string fix;
};

struct ProtectionFacts {
    bool password_saved = false;  // the job's password is saved on this computer (or its admin copy has it)
    bool kit_saved = false;       // a recovery kit for its destination was saved or printed here
};

// The gaps of `job`, given its history (oldest first), all saved jobs (for "every job on one
// drive"), and what only the store knows (ProtectionFacts).
std::vector<ProtectionGap> protection_gaps(const Job& job, const std::vector<Job>& all,
                                           const std::vector<RunRecord>& history, const ProtectionFacts& facts,
                                           std::uint64_t now);
// The facts from the store: the saved password, the recovery kit marker.
ProtectionFacts protection_facts(const JobStore& store, const Job& job);

// One calm note about a backup drive's file system (os::file_system_of), empty for none: exFAT
// and FAT have no journal, so a drive unplugged while it is written can lose more than that.
std::string drive_note(const std::string& file_system);

// Remembers that a recovery kit for `location` was saved or printed (a marker file in
// <settings dir>/kits; it holds nothing of the key), and whether one was.
void note_recovery_kit(const std::string& settings_dir, const std::string& location);
bool recovery_kit_noted(const std::string& settings_dir, const std::string& location);

}  // namespace vaultik::jobs
