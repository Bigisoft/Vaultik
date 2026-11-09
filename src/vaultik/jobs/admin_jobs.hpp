#pragma once

// Jobs that run with administrator rights (Windows). Their scheduled runs must not depend on
// anything an account without those rights can change, so:
//   - Vaultik has to run from a folder only administrators can change (such as Program Files);
//   - the job and its password are copied, with the user's approval, into an admin-only store,
//     <ProgramData>\Vaultik\admin\<user SID>\ (jobs/, secrets/, history/ as in JobStore), which the
//     user can read but not change;
//   - the scheduled task runs "run-job <id> --admin", which reads only that store, after checking
//     that it is still admin-only.
// The user's own store keeps the job as edited in the GUI; saving it copies it again.

#include <string>
#include <vector>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

// The admin store of the account `sid` (default: the current one). Throws on POSIX.
std::string admin_store_dir(const std::string& sid = {});

// Opens the current account's admin store after checking that it, and the folders above it, are
// admin-only. Throws Error saying what is wrong.
JobStore open_admin_store();

// Throws Error unless vaultik.exe, vaultik-task.exe and `program_dir` itself (and every folder
// above it) are admin-only.
void check_program_dir(const std::string& program_dir);

// Run history from the user's store and, when there is one, the admin store, oldest first.
std::vector<RunRecord> combined_history(const JobStore& store, const std::string& id, std::size_t limit = 100);

// Whether connecting the job's drive should start a backup: the job asks for it, its drive is
// connected, and it did not save a version in the last 10 minutes (Windows reports one
// connection several times, and the drive may have been plugged straight back in).
bool connect_backup_due(const JobStore& store, const Job& job);

// Whether the admin store holds the job (so removing it needs administrator rights).
bool has_admin_copy(const std::string& id);

// The elevated side, run as "vaultik admin-job install|remove": copies job `id` from `from` (with
// its password, re-sealed) into the admin store and registers its task; or removes both. Must run
// with the full administrator token of the account `user_sid`. Throws Error.
void install_admin_job(const JobStore& from, const std::string& id, const std::string& user_sid);
void remove_admin_job(const std::string& id, const std::string& user_sid);

// Where the elevated side leaves its error message for the caller (in the admin store, which the
// user can read). Empty on POSIX.
std::string admin_error_file(const std::string& sid = {});

}  // namespace vaultik::jobs
