#pragma once

// Ejecting a job's backup drive: never while a backup, health check, cleanup or restore uses it
// (they hold the drive's lock, see drive_lock.hpp), and optionally on its own once the last run
// that waited for the drive is done ("eject when done", a setting per drive kept in
// <settings dir>/drives-eject.json).

#include <string>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

// The drive root the job's backup is on now ("F:\\", "/run/media/me/USB"); empty for cloud
// storage, a network share or a drive that is not connected.
std::string backup_drive_root(const Job& job);

// Ejects the job's backup drive, holding its lock meanwhile so no run starts on it. Empty on
// success, otherwise why not (in use by a run, not removable, refused by the system).
std::string eject_backup_drive(const Job& job, const std::string& settings_dir);

// "Eject when done" for the drive with this volume id (Job::repository_volume).
bool eject_when_done(const std::string& settings_dir, const std::string& volume);
void set_eject_when_done(const std::string& settings_dir, const std::string& volume, bool on);

}  // namespace vaultik::jobs
