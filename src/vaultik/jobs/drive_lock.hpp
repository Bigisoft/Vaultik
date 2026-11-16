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
// Windows: named mutexes in the global namespace (jobs with administrator rights run as the system
// account, in a session of their own). Jobs with administrator rights take turns with the runs of
// the account that set them up; other accounts cannot take or hold the turn. Each mutex is open to
// the system account, the administrators and one account more, the one the run acts for (see
// set_drive_lock_account), which may only wait for it and let go of it; and only one owned by one
// of these three is used. Whoever makes it, it gets the same rights. One that is there but cannot
// be shared (made by another account, or one this account may not open) is not waited for: a run
// goes on without it, an eject counts it as busy; so does a name taken by another kind of object
// (an event, say). A claim waits for an eject mutex that cannot be shared as it waits for one it
// can (at most kEjectWait). The accepted limit: another local account that takes the names first
// (with mutexes of its own, or other objects) can neither take the turn nor stall a run, but it can
// make ejects of that drive report busy and, with mutexes, delay each command by up to kEjectWait
// (the claim's wait). As the account's own programs can hold the turn, a job with administrator
// rights that waited the whole wait goes on with a warning instead of giving up: such a program
// can delay it, never stop it.
// Linux: flock on a file in /run/lock, the same for every account: root's runs take turns with the
// runs of every account (the user's prunes and ejects too). An account uses a file there that
// belongs to root or to itself (its own are readable by it alone); root uses any. As any account
// that can open a file may hold its flock, a run does not wait long for one another account owns:
// root's runs first take a lock of their own that only root may open, and an account's runs locks
// of their own (vaultik-drive-<hash>.lock in the account's runtime folder /run/user/<uid>, used
// when it is a folder of the account's alone, where no other account can make a file first, and
// vaultik-drive-<hash>.<uid>.lock in /run/lock, used when it is a file the account owns; both where
// both can be used, so that a run started outside a login, without the runtime folder, takes turns
// too; where the account cannot use its file in /run/lock (a distribution whose /run/lock only root
// may write to), the file in the given folder as well, after the runtime one, so that such a run,
// or one with another runtime folder (a Flatpak's), takes turns with the others as long as they use
// the same settings folder), so the runs of one account still take turns, whatever settings folder
// each uses wherever /run/lock has their file; then each waits for the shared one only a short
// while (kOtherAccountWait) while a run of another account may hold it, and goes on with a warning.
// A run of root marks that it uses the drive with root's own lock (vaultik-drive-<hash>.root.lock
// in /run/vaultik, a folder only root may write to, so no account can put a file in its place, and
// that every account may look into (set so whatever root's umask); /run/lock where that folder
// cannot be used; every account can see it held in /proc/locks but none can take it): an account's
// run waits for the shared lock for as long as that mark is held, then only the short while, and
// while the mark is held it counts the shared lock as busy even when it is free (root's run may
// have passed it by) or cannot be used at all (another account made the shared file first). The
// claims (vaultik-use-<hash>.lock) work the same way with vaultik-use-<hash>.root.lock: an
// account's claim waits only for an eject of its own or of root's, and its eject counts only the
// claims of its own and of root's as busy. So another account can delay a run by kOtherAccountWait,
// never stop it, can delay a command of an account by 2 seconds at most (a claim waits that long
// for a claims file a process of root's holds alone, as root's eject does for a moment after its
// mark; only one whose four accounts, real, effective, saved and file system, are all root's
// counts), and cannot keep its eject busy. Root's ejects still count the runs and claims of every
// account, fail closed, and root's claims wait for the ejects of every account (at most
// kEjectWait): through the shared files, and through each account's own files that root can find
// (vaultik-drive-<hash>.<uid>.lock and vaultik-use-<hash>.<uid>.lock in /run/lock, the files in
// each runtime folder /run/user/<uid>), which an account's run, claim or eject holds also when it
// is left without the shared file (another account held that one when it started, or made it
// first); an eject of root's counts each one held, and each one there where /proc/locks cannot
// tell. As another account may make an account's file in /run/lock first, so that a run of it
// without its runtime folder holds only the file in its settings folder, root also looks into each
// process that /proc/locks shows holding a flock, and counts one that holds a file of the drive's
// name there (/proc/<pid>/fd) that its account owns and no other account may open; in one look at
// most 256 processes and 65536 descriptors of each account, and 3 seconds in all, so that no
// account can keep it looking for long: an account over that (or a look over its time) cannot be
// told, so root's eject fails closed and root's claim does not wait for it. A flock held in another
// pid namespace (another container that shares /run/lock) cannot be seen in /proc at all, as
// /proc/locks leaves it out: root's eject therefore also tries a flock on each own file of an
// account that it finds by its name (never waiting, let go of at once, never made), and counts one
// it cannot take, or cannot try (fail closed), as busy; this needs statx's mount ID (Linux 5.8 and
// later, where statx is allowed): without it these files are not tried, and only holders in the
// same pid namespace are seen. A holder in another pid namespace of a file root finds only through
// the process that has it open (the settings folder's) is not seen. An eject counts every file of
// these names that the right account (or root, for root's marks) owns, also one with a hard link or
// that other accounts may open: where fs.protected_hardlinks is off, another account may add a hard
// link to such a file while it is held, and must not hide it from an eject that way. Where a holder
// is waited for or trusted (root's claims waiting for an account's eject, an account's runs and
// claims waiting for root's mark, a command that inherits its run's drive), only a file no other
// account may open counts, and in /run/lock only one with no other name (another account could
// link a file its owner holds for something else under that name); the files in root's folder and
// in an account's runtime folder, where no other account can make a name, need no such test. A run
// keeps using its own file in /run/lock (root's mark there too) when it has another name, so the
// runs of an account still take turns; the limit: where fs.protected_hardlinks is off, another
// account may make that name first as a hard link to another file of the account's, which a run or
// claim then waits for while it is held (never longer than its wait). Root does not look
// at a runtime folder, or a file, on a FUSE mount that its mount table shows (another account's
// FUSE program, mounted with allow_other, could keep it waiting for as long as it likes), so such a
// file counts for nothing; for a file a process has open, its mount is looked up in root's mount
// table and then in that process's, and one in neither (a mount taken away with umount -l) is not
// looked at but cannot be told (root's eject fails closed). The details of these files are read as
// the system has them at hand (AT_STATX_DONT_SYNC), never asking their file system, which covers a
// file put in place of another between two looks; and a file root finds by its name is looked at
// through one descriptor (O_PATH, never following a link), so that its owner cannot swap it for a
// link between two looks. The limit: root's runs do not wait for an
// account's own files (a run of that account that waits for root's mark holds them, and would keep
// root's run waiting in turn), so a run of an account left without the shared file may share the
// drive's speed with a run of root's. A file in /run/lock with something mounted on it (another
// account's FUSE file system, mounted with allow_other on a file of its own there, whose program
// could keep every open of it waiting) is never opened, where statx tells mounts (Linux 5.8 and
// later): it counts as one that cannot be used. Where /run/lock cannot be used, a file in the given
// folder instead.
//
// A lock or claim that cannot be had for another reason (the system refuses the object, the folder
// is not writable) never stops a run: the run goes on without it, as runs did before there was a
// lock, because a backup that does not run is worse than two that share a drive.

#include <chrono>
#include <cstddef>
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
// Linux: how long a run waits for the lock all accounts share while another account may hold it
// (see above), before it goes on without it.
constexpr std::chrono::minutes kOtherAccountWait{5};
// How long a command waits for its claim while an eject of the drive is under way (the eject
// holds off new claims until the drive is gone, so the command then finds it gone).
constexpr std::chrono::seconds kEjectWait{60};

// Windows: the account whose runs share the drive's turn with this process (see above). A run as
// the system account for an account ("run-job --admin --user <SID>") sets it once act_for_account
// has checked that SID; empty: this process's own account again (none for the system account).
// A no-op on POSIX.
void set_drive_lock_account(const std::string& sid);
// That account for a process of `process_sid` that was set `chosen`: `chosen`, else the process's
// own account, but none (empty) for the system account (S-1-5-18).
std::string drive_lock_account_for(const std::string& process_sid, const std::string& chosen);
// The security descriptor (SDDL) of the drive's mutexes for that account, the same whoever makes
// them: full rights for the system account and the administrators; the account (none when empty)
// may wait for one and let go of it, never change who may open it.
std::string drive_lock_sddl(const std::string& account);

// How a DriveLock takes its turn. `run`: a backup, check, prune or restore, which goes on without
// a lock that cannot be shared and gives up (DriveBusy) when the wait ends. `admin_run`: the same
// for a job with administrator rights, except that it goes on without the lock when the wait ends
// (gave_up_waiting()). `eject`: fails closed: a lock that is there but cannot be shared, or one
// the parent run holds, counts as busy (Linux, root: also an account's own lock, see above).
enum class DriveLockMode { run, admin_run, eject };

// The name part of the drive's lock objects (mutexes, files) for `key`: hex, the same for every
// account.
std::string drive_lock_name(const std::string& key);

// The environment variable a run that holds drive locks passes to the commands it starts (its
// before and after commands): the tokens of those locks, separated by ';'. Windows: a vaultik
// command started by such a command for the same drive (a prune in an after command, say) then
// does not wait for its own parent run, which waits for it to end. Linux: the lock's holder tells
// instead (see DriveLock), whatever reaches the command (sudo and pkexec do not pass it on); this
// is looked at only where /proc cannot tell who holds a busy lock.
constexpr const char* kHeldDrivesVariable = "VAULTIK_HELD_DRIVES";

class DriveLock {
public:
    // Takes the lock of `key` (see drive_lock_key; an empty key takes none), waiting up to `wait`
    // while other runs hold it. `on_wait` is called once when it has to wait; `cancelled` is asked
    // about once a second while waiting (true: throws Error "cancelled"). Throws DriveBusy when the
    // wait ends without the lock. The run that started this command may hold the drive (a prune in
    // its after command, say), and waits for the command to end: then nothing waits (inherited()),
    // also when that run has other rights (Linux: root's command started by an account's run, or
    // the other way round) and holds only the lock all accounts share, or only root's mark.
    // Windows: that run is the one kHeldDrivesVariable names, when the lock is busy. Linux: a
    // process this one descends from (through any shell, sudo or pkexec) holds a lock file of the
    // drive, as /proc/locks shows, whatever the environment: the busy file itself when this account
    // owns it and no other account may open it (told by the file, never by its path, which another
    // account may move the shared file away from), root's mark, or an own file of the account that
    // process runs as now (never one of another account's, as /proc/locks still names a process
    // that took a lock after it ended, and its number may have gone to a process of another account
    // since), or for a command of root's the drive's file in that process's settings folder, which
    // root sees it has open and that only its account may open (also when another account holds the
    // file all accounts share); the file all accounts share never counts on its own, as any account
    // may lock it. One that is no ancestor (a command that outlived its run, another account's run)
    // is waited for as usual. A lock counts only on the file's own device and inode: /proc/locks
    // gives the device of the file's mount (btrfs: not the subvolume's, which stat gives, so a
    // process counts there only when it has that very file open), and a lock on another device with
    // the same inode never makes a parent run. Where the device of a file's mount cannot be told
    // (before Linux 5.8, or where statx is refused), stat's device counts, except on btrfs and
    // overlayfs. Where /proc gives no answer to use (no holder of the busy file shows there, and
    // for an own lock the file is still busy when tried once more a moment later, as root's eject
    // may hold it just then to see whether it is held; the device of a file's mount on btrfs or
    // overlayfs cannot be told while a process above this one
    // may hold one with its inode, or the processes above this one cannot be read: another pid
    // namespace, hidepid; or the busy file is an own lock of this account that other accounts may
    // open too, as in a settings folder on FAT, exFAT, NTFS or CIFS, which never makes a parent run
    // on its own), kHeldDrivesVariable tells as on Windows, never for an eject; a holder of the
    // file all accounts share that cannot be trusted is no such case.
    // Either way, a lock that is free (the run that started the command has ended) is taken as
    // usual.
    // Another DriveLock of this process on the drive (any thread) counts as busy. `lock_dir`: see
    // above. `mode`: see DriveLockMode.
    DriveLock(const std::string& key, const std::string& lock_dir, std::chrono::milliseconds wait,
              const std::function<void()>& on_wait = {}, const std::function<bool()>& cancelled = {},
              DriveLockMode mode = DriveLockMode::run);
    ~DriveLock();
    DriveLock(const DriveLock&) = delete;
    DriveLock& operator=(const DriveLock&) = delete;

    bool held() const { return handle_ != nullptr; }
    bool waited() const { return waited_; }
    bool inherited() const { return inherited_; }
    // Linux: the lock all accounts share was held by another account for kOtherAccountWait, and
    // the run goes on without it (holding the lock of its own account only). An eject of an
    // account (not root) passes such a lock by at once.
    bool other_account_skipped() const { return skipped_; }
    // Windows: the drive's lock is there but cannot be shared with this run (another account made
    // it, or this account may not open it), and the run goes on without it.
    bool not_shared() const { return not_shared_; }
    // An admin_run that waited the whole wait and goes on without the lock.
    bool gave_up_waiting() const { return gave_up_; }

    // The kHeldDrivesVariable entry for the commands this run starts: this lock's token added to
    // the ones it inherited. Empty name when it holds none.
    std::pair<std::string, std::string> child_environment() const;

private:
    void* handle_ = nullptr;  // Windows: the mutex; POSIX: the descriptor + 1
    void* shared_ = nullptr;  // POSIX, root: the shared file's descriptor + 1
    void* own2_ = nullptr;    // POSIX, an account: its second own file's descriptor + 1
    bool waited_ = false;
    bool inherited_ = false;
    bool skipped_ = false;
    bool not_shared_ = false;
    bool gave_up_ = false;
    std::string hash_;
    std::string token_;
};

// A shared claim on the drive behind `key` for as long as it lives (see above). Never throws; it
// waits only while an eject of the drive is under way (at most kEjectWait). One that cannot be
// had is not held.
class DriveClaim {
public:
    DriveClaim(const std::string& key, const std::string& lock_dir);
    ~DriveClaim();
    DriveClaim(const DriveClaim&) = delete;
    DriveClaim& operator=(const DriveClaim&) = delete;

    bool held() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr;  // Windows: the slot's mutex; POSIX: the descriptor + 1
    void* shared_ = nullptr;  // POSIX: the shared file's descriptor + 1, besides an own one
    void* own2_ = nullptr;    // POSIX, an account: its second own file's descriptor + 1
    std::string hash_;
};

// What an eject holds while it ejects: when no claim on the drive behind `key` is held (by any
// process, any thread of this one, the calling thread too), it holds off new claims for as long
// as it lives, so a command that starts meanwhile waits for the eject and then finds the drive
// gone, instead of opening files on it as it goes. busy(): a claim is held (or one that is there
// cannot be checked, or another eject is under way), and nothing is held.
class EjectHold {
public:
    EjectHold(const std::string& key, const std::string& lock_dir);
    ~EjectHold();
    EjectHold(const EjectHold&) = delete;
    EjectHold& operator=(const EjectHold&) = delete;

    bool busy() const { return busy_; }

private:
    void* handle_ = nullptr;  // Windows: the eject mutex; POSIX: the descriptor + 1
    void* shared_ = nullptr;  // POSIX: the shared file's descriptor + 1, besides an own one
    void* own2_ = nullptr;    // POSIX, an account: its second own file's descriptor + 1
    bool busy_ = false;
    std::string hash_;
};

// Whether a claim on the drive behind `key` is held, by another process or any thread of this
// one (an EjectHold that lets go at once).
bool drive_claimed(const std::string& key, const std::string& lock_dir);

namespace testing {

// For tests only (Linux; no-op elsewhere): lets a test without root stand in for other accounts
// with processes of its own. `own_in_folder`: the account's own lock is the file in the given
// folder alone, neither the one in its runtime folder nor the one in /run/lock (so a process of
// this account with another folder stands in for another account, also on the file all accounts
// share in /run/lock, which then counts as one other accounts may open); `root_uid`: the account
// whose marks (see above) and processes count as root's; `other_account_wait`: instead of
// kOtherAccountWait when not negative; `root_dir`: instead of /run/vaultik when not empty;
// `run_user_dir`: instead of /run/user/<uid> when not empty; `run_user_root`: instead of /run/user,
// where root looks for every account's runtime folder, when not empty;
// `proc_locks`: instead of /proc/locks when not empty (a missing file: as if it could not be
// read); `locks_other_device`: every line of /proc/locks carries another device than stat gives,
// the one /proc/self/mountinfo gives the file's mount (as on btrfs); `no_mount_device`: the device
// of a file's mount cannot be told (as without statx's mount ID, before Linux 5.8); `btrfs`: every
// file is on a file system where stat and /proc/locks may give different devices (btrfs, overlayfs);
// `no_statx`: statx is refused (EPERM, as by older Docker seccomp filters); `mountinfo`: instead
// of /proc/self/mountinfo when not empty; `scan_processes` and `scan_descriptors`: when not 0,
// how many processes and descriptors of each account root looks into at most in one look (see
// the eject's limits above).
struct DriveLockHooks {
    bool own_in_folder = false;
    unsigned root_uid = 0;
    std::chrono::milliseconds other_account_wait{-1};
    std::string root_dir;
    std::string run_user_dir;
    std::string proc_locks;
    bool locks_other_device = false;
    bool no_mount_device = false;
    std::string run_user_root;
    bool btrfs = false;
    bool no_statx = false;
    std::string mountinfo;
    std::size_t scan_processes = 0;
    std::size_t scan_descriptors = 0;
};
void set_drive_lock_hooks(const DriveLockHooks& hooks);

}  // namespace testing

}  // namespace vaultik::jobs
