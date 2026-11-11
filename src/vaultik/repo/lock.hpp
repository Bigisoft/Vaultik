#pragma once

// Repository locks (see docs/FORMAT.md, "Locks").
//
// Every run that reads or writes snapshot data holds a lock file in locks/. Backups, restores and
// checks take a shared lock and can run side by side; forget and prune take an exclusive lock,
// because they delete data that a concurrent run could be relying on. A holder refreshes its lock
// every few minutes, so the lock of a run that crashed or lost power goes stale and is ignored.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

// Thrown when another run holds a conflicting lock.
class LockedError : public Error {
public:
    using Error::Error;
};

struct LockInfo {
    bool exclusive = false;
    std::uint64_t time = 0;  // FILETIME of the last refresh
    std::uint32_t pid = 0;
    std::string hostname;
    std::string username;
    std::string command;  // "backup", "prune", ...
};

constexpr std::uint64_t kTicksPerMinute = 60ull * 10'000'000ull;
constexpr std::uint64_t kLockRefreshTicks = 5 * kTicksPerMinute;
// Not refreshed for this long: the holder is gone.
constexpr std::uint64_t kLockStaleTicks = 30 * kTicksPerMinute;
// The same for another computer's lock. Its time comes from that computer's clock and is judged
// by this one's, so the two may differ: a computer whose clock runs ahead would otherwise take a
// live lock for stale and remove it. This much difference is tolerated.
constexpr std::uint64_t kLockStaleOtherTicks = 2 * 60 * kTicksPerMinute;

Bytes serialize_lock(const LockInfo& info);
LockInfo parse_lock(ByteSpan data);

// Stale: held by a process on this computer that has ended or did not refresh it for
// kLockStaleTicks, or held by another computer that did not refresh it for kLockStaleOtherTicks.
bool is_stale(const LockInfo& info, std::uint64_t now);

// "shared lock by backup (me@PC, process 1234, refreshed 2026-11-09 10:00:00)"
std::string describe(const LockInfo& info);

struct LockEntry {
    std::string name;               // "locks/<hex>"
    std::optional<LockInfo> info;  // empty when the file cannot be read
};

std::vector<LockEntry> list_locks(Repository& repo);
void write_lock(Repository& repo, const std::string& name, const LockInfo& info);

// Removes stale locks, or every lock (also unreadable ones) with `all`. Returns how many.
std::size_t remove_locks(Repository& repo, bool all);

// Holds a lock for its lifetime and refreshes it from a background thread.
class RepoLock {
public:
    enum class Mode { Shared, Exclusive };

    // Waits up to `wait` for conflicting locks to go away, then throws LockedError.
    RepoLock(Repository& repo, Mode mode, std::string command,
             std::chrono::seconds wait = std::chrono::seconds(0));
    ~RepoLock();
    RepoLock(const RepoLock&) = delete;
    RepoLock& operator=(const RepoLock&) = delete;

    // Removes the lock now (also done by the destructor).
    void release();

    // False once the lock file has disappeared: another run judged it stale (this computer slept
    // or hung for a long time) and may have deleted data since. Checked by the refresh thread
    // every minute; held() checks the repository right now.
    bool lost() const { return lost_.load(); }
    bool held();

private:
    enum class State { Held, Gone, Unknown };
    // One look at the lock file; Unknown when the backend could not answer.
    State probe();
    void refresh_loop();

    Repository& repo_;
    std::atomic<bool> lost_{false};
    std::string name_;
    LockInfo info_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

}  // namespace vaultik
