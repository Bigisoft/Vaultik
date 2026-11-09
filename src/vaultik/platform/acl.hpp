#pragma once

// Who may change a file or folder (Windows), for jobs that run with administrator rights: such a
// job must only run programs and read settings that accounts without those rights cannot change.

#include <string>

namespace vaultik::os {

// Whether only the system and administrators can change `path`: its owner, and every entry of its
// permissions that grants writing, deleting, or changing the permissions or owner, is SYSTEM,
// Administrators or TrustedInstaller. With `ancestor`, only renaming and permission changes count
// (a folder above the one that matters may let users add their own files). False with the reason in
// `why` otherwise, also when the permissions cannot be read (FAT drives have none). Always false on
// POSIX.
bool admin_only(const std::string& path, bool ancestor = false, std::string* why = nullptr);

// admin_only(path), and admin_only(..., ancestor = true) for every folder above it.
bool admin_only_chain(const std::string& path, std::string* why = nullptr);

// Makes `path` a folder that only the system and administrators can change, and that the account
// `reader_sid` (when not empty) can read. An existing one that is not admin-only is renamed aside
// first ("<path>.untrusted-<random>"). Needs administrator rights. Throws Error.
void make_admin_dir(const std::string& path, const std::string& reader_sid = {});

// The current account's security identifier ("S-1-5-21-..."), and whether this process has the
// full administrator token. Windows only (POSIX: throws / false).
std::string current_user_sid();
bool process_elevated();

// The machine-wide data folder (C:\ProgramData), from the system rather than the environment, which
// a user can change. Windows only (POSIX: throws).
std::string program_data_dir();

}  // namespace vaultik::os
