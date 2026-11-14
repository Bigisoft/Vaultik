#pragma once

// One run at a time per backup drive. Several jobs that back up to the same drive (often an
// external USB drive behind a slow hub) would otherwise run at once and share its speed, each
// taking as long as all of them together, with the drive's head jumping between them. A backup,
// a health check, a test restore or a prune of a job takes the drive's lock first; another run
// for the same drive waits for it (a long but finite time) and says that it waits.
//
// The lock is keyed by the drive's identity (its volume ID; for a network share or a folder whose
// drive cannot be told, the folder), so it holds across drive letters, and across runs from Task
// Scheduler, systemd timers and the app. Windows: a named mutex in the global namespace, which
// runs of every account (also the system account of jobs with administrator rights) can wait
// for. Linux: flock on a file in the user's settings folder. The system releases both when the
// run ends, however it ends.

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include "vaultik/util/error.hpp"

namespace vaultik::jobs {

struct Job;

// The drive was busy with other runs for the whole wait.
class DriveBusy : public Error {
public:
    using Error::Error;
};

// The lock's key for a destination: empty for cloud storage (no drive to share); `volume` is
// the drive recorded with the job (Job::repository_volume), if any.
std::string drive_lock_key(const std::string& location, const std::string& volume = {});
std::string drive_lock_key(const Job& job);

// How long a run waits for another one on its drive before it gives up.
constexpr std::chrono::hours kDriveLockWait{12};

class DriveLock {
public:
    // Takes the lock of `key` (see drive_lock_key; an empty key takes none), waiting up to `wait`
    // while other runs hold it. `on_wait` is called once when it has to wait; `cancelled` is
    // asked about once a second while waiting (true: throws Error "cancelled"). Throws DriveBusy
    // when the wait ends without the lock. Linux: the lock file is in `lock_dir`.
    DriveLock(const std::string& key, const std::string& lock_dir, std::chrono::milliseconds wait,
              const std::function<void()>& on_wait = {}, const std::function<bool()>& cancelled = {});
    ~DriveLock();
    DriveLock(const DriveLock&) = delete;
    DriveLock& operator=(const DriveLock&) = delete;

    bool held() const { return handle_ != nullptr; }
    bool waited() const { return waited_; }

private:
    void* handle_ = nullptr;  // Windows: the mutex; POSIX: the descriptor + 1
    bool waited_ = false;
};

}  // namespace vaultik::jobs
