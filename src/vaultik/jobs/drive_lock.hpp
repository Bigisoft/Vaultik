#pragma once

// One run at a time per backup drive. Several jobs that back up to the same drive (often an
// external USB drive behind a slow hub) would otherwise run at once and share its speed, each
// taking as long as all of them together, with the drive's head jumping between them. A backup,
// a health check, a test restore or a prune of a job takes the drive's lock first; another run
// for the same drive waits for it (a long but finite time) and says that it waits.
//
// Besides, every command that opens a repository or a backup drive holds a shared claim on the
// drive (DriveClaim) for as long as it runs, so the drive is never ejected under it, also between
// two of its file opens; many claims go together, and an eject asks whether any is held.
//
// The lock is keyed by the drive's identity (its volume ID; for a network share or a folder whose
// drive cannot be told, the folder), so it holds across drive letters, and across runs from Task
// Scheduler, systemd timers and the app. The system releases it when the run ends, however it
// ends (a run that crashed leaves nothing behind).
//
// Windows: named mutexes in the global namespace, which only this account, the system account and
// administrators may open, and only one owned by one of them is used: another account cannot hold
// the lock to stall this account's runs. (Jobs with administrator rights, run by the system
// account, so do not wait for the user's own runs, nor the other way round.)
// Linux: flock on a file in /run/lock, the same for every account (root's runs, the user's
// prunes and ejects), used only when it belongs to root or this account; where it cannot be used,
// a file in the given folder (the user's settings) instead.
//
// A lock or claim that cannot be had for another reason (the system refuses the object, the folder
// is not writable) never stops a run: the run goes on without it, as runs did before there was a
// lock, because a backup that does not run is worse than two that share a drive.

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

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

// The environment variable a run that holds drive locks passes to the commands it starts (its
// before and after commands): the tokens of those locks, separated by ';'. A vaultik command
// started by such a command for the same drive (a prune in an after command, say) then does not
// wait for its own parent run, which waits for it to end.
constexpr const char* kHeldDrivesVariable = "VAULTIK_HELD_DRIVES";

class DriveLock {
public:
    // Takes the lock of `key` (see drive_lock_key; an empty key takes none), waiting up to `wait`
    // while other runs hold it. `on_wait` is called once when it has to wait; `cancelled` is
    // asked about once a second while waiting (true: throws Error "cancelled"). Throws DriveBusy
    // when the wait ends without the lock. When kHeldDrivesVariable names this drive, the parent
    // run holds it: nothing is taken and nothing waits (inherited()). `lock_dir`: see above.
    DriveLock(const std::string& key, const std::string& lock_dir, std::chrono::milliseconds wait,
              const std::function<void()>& on_wait = {}, const std::function<bool()>& cancelled = {});
    ~DriveLock();
    DriveLock(const DriveLock&) = delete;
    DriveLock& operator=(const DriveLock&) = delete;

    bool held() const { return handle_ != nullptr; }
    bool waited() const { return waited_; }
    bool inherited() const { return inherited_; }

    // The kHeldDrivesVariable entry for the commands this run starts: this lock's token added to
    // the ones it inherited. Empty name when it holds none.
    std::pair<std::string, std::string> child_environment() const;

private:
    void* handle_ = nullptr;  // Windows: the mutex; POSIX: the descriptor + 1
    bool waited_ = false;
    bool inherited_ = false;
    std::string token_;
};

// A shared claim on the drive behind `key` for as long as it lives (see above). Never throws and
// never waits for long; one that cannot be had is not held.
class DriveClaim {
public:
    DriveClaim(const std::string& key, const std::string& lock_dir);
    ~DriveClaim();
    DriveClaim(const DriveClaim&) = delete;
    DriveClaim& operator=(const DriveClaim&) = delete;

    bool held() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;  // Windows: the slot's mutex; POSIX: the descriptor + 1
};

// Whether a claim on the drive behind `key` is held, by another process or another thread of
// this one (Windows: a claim of the calling thread is not counted).
bool drive_claimed(const std::string& key, const std::string& lock_dir);

}  // namespace vaultik::jobs
