#include "vaultik/jobs/drive_lock.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/util/hex.hpp"

#ifdef _WIN32
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <vector>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>
#endif

namespace vaultik::jobs {

namespace {

// A name-safe form of the key.
std::string key_hash(const std::string& key) {
    const Id id = blake3(ByteSpan(reinterpret_cast<const std::uint8_t*>(key.data()), key.size()));
    return to_hex(ByteSpan(id.data(), 16));
}

// Whether the parent run passed this drive's lock on (see kHeldDrivesVariable).
bool named_by_parent(const std::string& hash) {
    const std::string held = os::get_env(kHeldDrivesVariable);
    std::size_t at = 0;
    while (at <= held.size()) {
        const std::size_t end = std::min(held.find(';', at), held.size());
        const std::string token = held.substr(at, end - at);
        if (token.substr(0, token.find(':')) == hash) return true;
        at = end + 1;
    }
    return false;
}

// The drive locks and claims this process holds, by drive (any thread): a Windows mutex lets the
// thread that owns it in again, and on every system a lock or claim of this very process must
// count as one of another run.
struct InProcess {
    std::mutex mu;
    std::map<std::string, int> locks;
    std::map<std::string, int> claims;
    std::map<std::string, int> ejects;
};

// Never destroyed: locks and claims held in static objects (a command's claims) let go of
// theirs at exit, after function statics made later than them are gone.
InProcess& in_process() {
    static InProcess* const p = new InProcess;
    return *p;
}

// Takes the in-process part of a lock: false when another DriveLock of this process holds it.
bool take_in_process_lock(const std::string& hash) {
    InProcess& p = in_process();
    std::lock_guard<std::mutex> lock(p.mu);
    if (p.locks[hash] > 0) return false;
    p.locks[hash] = 1;
    return true;
}

void count(std::map<std::string, int>& map, const std::string& hash, int by) {
    InProcess& p = in_process();
    std::lock_guard<std::mutex> lock(p.mu);
    if ((map[hash] += by) <= 0) map.erase(hash);
}

// A claim, or another eject, of this process on the drive.
bool used_in_process(const std::string& hash) {
    InProcess& p = in_process();
    std::lock_guard<std::mutex> lock(p.mu);
    const auto claim = p.claims.find(hash);
    const auto eject = p.ejects.find(hash);
    return (claim != p.claims.end() && claim->second > 0) || (eject != p.ejects.end() && eject->second > 0);
}

#ifdef _WIN32
// The account set with set_drive_lock_account (empty: none).
struct ChosenAccount {
    std::mutex mu;
    std::string sid;
};

ChosenAccount& chosen_account() {
    static ChosenAccount* const c = new ChosenAccount;
    return *c;
}

constexpr int kClaimSlots = 32;

std::wstring wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}

// The account whose runs share the drive's turn (see set_drive_lock_account); empty: none.
std::string lock_account() {
    std::string chosen;
    {
        ChosenAccount& c = chosen_account();
        std::lock_guard<std::mutex> lock(c.mu);
        chosen = c.sid;
    }
    std::string mine;
    try {
        mine = os::current_user_sid();
    } catch (const std::exception&) {
    }
    return drive_lock_account_for(mine, chosen);
}

// `sid` ("S-1-5-21-...") in binary form; empty when it is not a SID.
std::vector<std::uint8_t> sid_bytes(const std::string& sid) {
    std::vector<std::uint8_t> out;
    PSID bin = nullptr;
    if (sid.empty() || !ConvertStringSidToSidW(wide(sid).c_str(), &bin)) return out;
    const DWORD length = GetLengthSid(bin);
    out.resize(length);
    if (!CopySid(length, out.data(), bin)) out.clear();
    LocalFree(bin);
    return out;
}

// Only an object the system account, the administrators or the account whose runs share the turn
// own is used: one made by another account (to hold it and stall these runs) is not.
bool trusted_owner(HANDLE object, const std::vector<std::uint8_t>& account) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return false;
    const bool ok = owner != nullptr &&
                    (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid) ||
                     (!account.empty() && EqualSid(owner, const_cast<std::uint8_t*>(account.data()))));
    LocalFree(sd);
    return ok;
}

// What open_mutex found. `denied`: there, but this account may not open it (made before runs
// shared their turn, or by another account); `untrusted`: there, owned by nobody to trust; `none`:
// not there (and not to be made); `error`: anything else (the name taken by another kind of
// object, say).
enum class Opened { ok, denied, untrusted, none, error };

struct Mutex {
    HANDLE handle = nullptr;
    Opened status = Opened::error;
};

// Opens the named mutex, making it when it is not there yet (`create`) with the same rights
// whoever makes it (drive_lock_sddl). A handle only with Opened::ok.
Mutex open_mutex(const std::wstring& name, bool create) {
    const std::string account = lock_account();
    const std::vector<std::uint8_t> sid = sid_bytes(account);
    if (!account.empty() && sid.empty()) return {};
    // Wait for it and let go of it, and read who owns it; never more.
    constexpr DWORD kAccess = SYNCHRONIZE | MUTEX_MODIFY_STATE | READ_CONTROL;
    static_assert(kAccess == 0x120001);
    HANDLE m = nullptr;
    DWORD error = ERROR_FILE_NOT_FOUND;
    if (create) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(wide(drive_lock_sddl(account)).c_str(), SDDL_REVISION_1,
                                                                  &sd, nullptr))
            return {};
        SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
        m = CreateMutexExW(&sa, name.c_str(), 0, kAccess);
        if (m == nullptr) error = GetLastError();
        LocalFree(sd);
    }
    if (m == nullptr) {
        m = OpenMutexW(kAccess, FALSE, name.c_str());
        if (m == nullptr) error = GetLastError();
    }
    if (m == nullptr) {
        if (error == ERROR_ACCESS_DENIED) return {nullptr, Opened::denied};
        if (!create && error == ERROR_FILE_NOT_FOUND) return {nullptr, Opened::none};
        return {};
    }
    if (!trusted_owner(m, sid)) {
        CloseHandle(m);
        return {nullptr, Opened::untrusted};
    }
    return {m, Opened::ok};
}

// There, but not to be shared with this account's runs.
bool not_to_share(const Mutex& m) {
    return m.status == Opened::denied || m.status == Opened::untrusted;
}

std::wstring slot_name(const std::string& hash, int slot) {
    return L"Global\\Vaultik-use-" + wide(hash) + L"-" + std::to_wstring(slot);
}

std::wstring eject_name(const std::string& hash) {
    return L"Global\\Vaultik-eject-" + wide(hash);
}
#else
// The file `name` in /run/lock, shared by every account: the one there when `usable` accepts it,
// or a new one with `mode` (made at most once, also when several runs start at once). Root
// removes one it cannot use and makes its own. -1 when there is none to use.
int open_in_run_lock(const std::string& name, mode_t mode, bool (*usable)(const struct stat&)) {
    struct stat dir{};
    if (::lstat("/run/lock", &dir) != 0 || !S_ISDIR(dir.st_mode)) return -1;
    const std::string path = "/run/lock/" + name;
    const bool root = ::geteuid() == 0;
    // Read only is enough for flock, so another account's file needs no write access; never
    // following a link, and never waiting on a pipe put there in its place.
    constexpr int kFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    for (int attempt = 0; attempt < 4; ++attempt) {
        // Without O_CREAT first: with fs.protected_regular, opening another account's file in a
        // sticky folder such as /run/lock with O_CREAT fails, also for root and also when the file
        // is there already.
        int fd = ::open(path.c_str(), kFlags);
        if (fd < 0 && errno == ENOENT) {
            fd = ::open(path.c_str(), kFlags | O_CREAT | O_EXCL, mode);
            if (fd < 0 && errno == EEXIST) continue;  // another run made it meanwhile: use that one
        }
        if (fd < 0) {
            if ((errno == EACCES || errno == ELOOP) && root && ::unlink(path.c_str()) == 0) continue;
            return -1;
        }
        struct stat st{};
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && usable(st)) {
            if (st.st_uid == ::geteuid()) (void)::fchmod(fd, mode);  // whatever the umask
            return fd;
        }
        ::close(fd);
        if (!root || ::unlink(path.c_str()) != 0) return -1;
    }
    return -1;
}

bool own_or_roots(const struct stat& st) {
    return ::geteuid() == 0 || st.st_uid == 0 || st.st_uid == ::geteuid();
}

bool roots(const struct stat& st) {
    return st.st_uid == 0;
}

struct Hooks {
    std::mutex mu;
    testing::DriveLockHooks hooks;
};

Hooks& hooks_store() {
    static Hooks* const h = new Hooks;
    return *h;
}

testing::DriveLockHooks hooks() {
    Hooks& h = hooks_store();
    std::lock_guard<std::mutex> lock(h.mu);
    return h.hooks;
}

std::chrono::milliseconds other_account_wait() {
    const std::chrono::milliseconds set = hooks().other_account_wait;
    return set.count() >= 0 ? set : std::chrono::duration_cast<std::chrono::milliseconds>(kOtherAccountWait);
}

// The file `name` in `dir` (the user's settings), for this account alone. -1 when it cannot be
// opened.
int open_in_dir(const std::string& name, const std::string& dir) {
    (void)os::make_dir(dir);  // the user's settings folder, which is there
    return ::open(os::join(dir, name).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
}

// The lock file `name` all accounts share (see open_in_run_lock): an account's own is readable by
// it alone (and root), root's by all. Otherwise in `fallback_dir`. -1 when neither can be opened.
int open_lock_file(const std::string& name, const std::string& fallback_dir) {
    const int fd = open_in_run_lock(name, ::geteuid() == 0 ? 0644 : 0600, own_or_roots);
    if (fd >= 0) return fd;
    return open_in_dir(name, fallback_dir);
}

// What a run of an account (not root) locks for `name`: `own`, which only runs of this account
// can hold; and, when the file all accounts share is not this account's own (root's, which any
// account that can open it may hold), that one as `shared` too, which is waited for only a short
// while. Without a shared file of another account, `own` is the shared file (or, without one, a
// file in `lock_dir`). Either may be -1.
struct AccountFiles {
    int own = -1;
    int shared = -1;
};

AccountFiles open_account_files(const std::string& name, const std::string& lock_dir) {
    AccountFiles f;
    const int fd = open_in_run_lock(name, 0600, own_or_roots);
    struct stat st{};
    if (fd >= 0 && ::fstat(fd, &st) == 0 && (st.st_uid != ::geteuid() || hooks().shared_foreign)) {
        f.shared = fd;
        f.own = open_in_dir(name, lock_dir);
        return f;
    }
    f.own = fd >= 0 ? fd : open_in_dir(name, lock_dir);
    return f;
}

// Whether root holds its mark `name` in /run/lock (`write_only`: holds it alone, as an eject
// does). Only root may open such a file, so only root can hold it; every account can see in
// /proc/locks that it is held. False when that cannot be told.
bool root_holds(const std::string& name, bool write_only) {
    struct stat st{};
    if (::lstat(("/run/lock/" + name).c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != hooks().root_uid)
        return false;
    std::ifstream in("/proc/locks");
    std::string line;
    while (std::getline(in, line)) {
        // "1: FLOCK  ADVISORY  WRITE 1234 fe:00:5678 0 EOF"; "1: -> FLOCK ..." for one that waits.
        std::istringstream words(line);
        std::string id, kind, advisory, type, pid, where;
        if (!(words >> id >> kind) || kind == "->" || kind != "FLOCK") continue;
        if (!(words >> advisory >> type >> pid >> where)) continue;
        unsigned dev_major = 0, dev_minor = 0;
        unsigned long long inode = 0;
        if (std::sscanf(where.c_str(), "%x:%x:%llu", &dev_major, &dev_minor, &inode) != 3) continue;
        if (dev_major != major(st.st_dev) || dev_minor != minor(st.st_dev) || inode != st.st_ino) continue;
        if (!write_only || type == "WRITE") return true;
    }
    return false;
}

int fd_of(void* handle) {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
}

void* handle_of(int fd) {
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
}
#endif

}  // namespace

std::string drive_lock_name(const std::string& key) {
    return key_hash(key);
}

void set_drive_lock_account(const std::string& sid) {
#ifdef _WIN32
    ChosenAccount& c = chosen_account();
    std::lock_guard<std::mutex> lock(c.mu);
    c.sid = sid;
#else
    (void)sid;
#endif
}

std::string drive_lock_account_for(const std::string& process_sid, const std::string& chosen) {
    if (!chosen.empty()) return chosen;
    if (process_sid == "S-1-5-18") return {};  // the system account acts for nobody unless told
    return process_sid;
}

std::string drive_lock_sddl(const std::string& account) {
    // Protected, and no SACL. 0x120001: SYNCHRONIZE, READ_CONTROL and MUTEX_MODIFY_STATE, never
    // WRITE_DAC, WRITE_OWNER or DELETE.
    std::string sddl = "D:P(A;;GA;;;SY)(A;;GA;;;BA)";
    if (!account.empty()) sddl += "(A;;0x120001;;;" + account + ")";
    return sddl;
}

std::string drive_lock_key(const std::string& location, const std::string& volume) {
    if (location.empty() || is_cloud_location(location)) return {};
    if (!volume.empty()) return volume;
    std::string full;
    try {
        full = os::absolute_path(location);
        const os::VolumeLocation loc = os::volume_of(full);
        if (!loc.volume.empty()) return loc.volume;
    } catch (const std::exception&) {
        full = location;
    }
#ifdef _WIN32
    return os::fold_case(full);
#else
    return full;
#endif
}

std::string drive_lock_key(const Job& job) {
    return drive_lock_key(job.repository, job.repository_volume);
}

DriveLock::DriveLock(const std::string& key, const std::string& lock_dir, std::chrono::milliseconds wait,
                     const std::function<void()>& on_wait, const std::function<bool()>& cancelled, DriveLockMode mode) {
    if (key.empty()) return;
    const std::string hash = key_hash(key);
    // Named by the run that started this command: tried without waiting. Busy, that run holds the
    // drive and waits for this command to end; free, it ended (this command outlived it) and the
    // lock is taken as usual.
    const bool parent = named_by_parent(hash);
    const bool fail_closed = mode == DriveLockMode::eject;
    const auto busy_now = [&] {
        if (parent && !fail_closed) {
            inherited_ = true;
            return true;
        }
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + wait;
    // Once more around the wait; false: a job with administrator rights waited the whole wait and
    // goes on without the lock.
    const auto waiting = [&] {
        if (parent && fail_closed) throw DriveBusy("the run that started this command uses the backup drive");
        if (!waited_ && on_wait) on_wait();
        waited_ = true;
        if (cancelled && cancelled()) throw Error("cancelled");
        if (std::chrono::steady_clock::now() >= deadline) {
            if (mode == DriveLockMode::admin_run) {
                gave_up_ = true;
                return false;
            }
            throw DriveBusy("another Vaultik run kept the backup drive busy for too long; try again later");
        }
        return true;
    };
    const auto pause = [&](std::chrono::milliseconds most) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::clamp(left, std::chrono::milliseconds(0), most));
    };
    // Another DriveLock of this process first (it may be this very thread).
    while (!take_in_process_lock(hash)) {
        if (busy_now() || !waiting()) return;
        pause(std::chrono::milliseconds(100));
    }
    // From here, returning without a handle lets go of the in-process part again.
    struct Undo {
        const std::string& hash;
        bool keep = false;
        ~Undo() {
            if (!keep) count(in_process().locks, hash, -1);
        }
    } undo{hash};
    std::uint8_t rnd[8];
    random_bytes(rnd);
    const std::string token = hash + ":" + to_hex(ByteSpan(rnd, sizeof rnd));
#ifdef _WIN32
    (void)lock_dir;
    const Mutex opened = open_mutex(L"Global\\Vaultik-drive-" + wide(hash), true);
    if (opened.handle == nullptr) {
        // There, but not to be shared with this account's runs (another account made it, or this
        // one may not open it): never waited for, nor taken over. An eject counts it as busy; a
        // run goes on without the lock.
        if (not_to_share(opened)) {
            if (busy_now()) return;
            if (fail_closed) throw DriveBusy("a run this account cannot see into uses the backup drive");
            not_shared_ = true;
        }
        return;  // no lock to share (the runs then overlap, as before)
    }
    HANDLE m = opened.handle;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const DWORD slice = static_cast<DWORD>(std::clamp<long long>(left.count(), 0, 1000));
        const DWORD r = WaitForSingleObject(m, waited_ ? slice : 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {  // abandoned: its run ended without letting go
            handle_ = m;
            token_ = token;
            hash_ = hash;
            undo.keep = true;
            return;
        }
        if (r == WAIT_FAILED) {
            CloseHandle(m);
            return;
        }
        if (busy_now()) {
            CloseHandle(m);
            return;
        }
        try {
            if (!waiting()) {
                CloseHandle(m);
                return;
            }
        } catch (...) {
            CloseHandle(m);
            throw;
        }
    }
#else
    const std::string name = "vaultik-drive-" + hash + ".lock";
    const std::string mark = "vaultik-drive-" + hash + ".root.lock";
    const bool root = ::geteuid() == 0;
    int own = -1;
    int shared = -1;
    if (root) {
        // Its own lock first, which only root may open (and the mark of a root run), then the one
        // all accounts share.
        own = open_in_run_lock(mark, 0600, roots);
        shared = open_in_run_lock(name, 0644, own_or_roots);
        if (own < 0) {
            own = shared >= 0 ? shared : open_lock_file(name, lock_dir);
            shared = -1;
        }
    } else {
        // The account's own lock first when the shared one is another account's (see
        // open_account_files).
        const AccountFiles files = open_account_files(name, lock_dir);
        own = files.own;
        shared = files.shared;
    }
    const auto close_all = [&] {
        if (own >= 0) ::close(own);
        if (shared >= 0) ::close(shared);
        own = shared = -1;
    };
    if (own < 0 && shared < 0) return;
    while (own >= 0) {
        if (::flock(own, LOCK_EX | LOCK_NB) == 0) break;
        if ((errno != EWOULDBLOCK && errno != EINTR) || busy_now()) {
            close_all();
            return;
        }
        try {
            if (!waiting()) {
                close_all();
                return;
            }
        } catch (...) {
            close_all();
            throw;
        }
        pause(std::chrono::milliseconds(250));
    }
    if (shared >= 0) {
        // Root: every root run took root's own lock first, so whoever holds this one now is another
        // account. An account: this account's runs took its own first, so whoever holds it is root
        // (while root's mark is held) or another account. Another account is waited for only a
        // short while (or for the whole wait, when that is shorter); an eject of an account passes
        // it by at once, while root's eject counts it as busy (fail closed, root's runs take turns
        // with every account's).
        const auto bound = other_account_wait();
        auto other_deadline = std::min(deadline, std::chrono::steady_clock::now() + bound);
        for (;;) {
            if (::flock(shared, LOCK_EX | LOCK_NB) == 0) break;
            if (errno != EWOULDBLOCK && errno != EINTR) {
                ::close(shared);
                shared = -1;
                break;
            }
            if (own < 0 && busy_now()) {  // the parent run holds it
                close_all();
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            const bool by_root = !root && root_holds(mark, false);
            if (by_root) other_deadline = std::min(deadline, now + bound);
            const bool pass = !by_root && (fail_closed ? !root : now >= other_deadline && other_deadline < deadline);
            if (pass) {
                ::close(shared);
                shared = -1;
                skipped_ = true;
                break;
            }
            try {
                if (!waiting()) {
                    close_all();
                    return;
                }
            } catch (...) {
                close_all();
                throw;
            }
            pause(std::chrono::milliseconds(250));
        }
    }
    if (own < 0) {
        if (shared < 0) return;  // passed by, and no lock of its own (the runs then overlap)
        own = shared;
        shared = -1;
    }
    handle_ = handle_of(own);
    if (shared >= 0) shared_ = handle_of(shared);
    token_ = token;
    hash_ = hash;
    undo.keep = true;
#endif
}

DriveLock::~DriveLock() {
    if (handle_ == nullptr) return;
#ifdef _WIN32
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (shared_ != nullptr) {
        ::flock(fd_of(shared_), LOCK_UN);
        ::close(fd_of(shared_));
    }
    ::flock(fd_of(handle_), LOCK_UN);
    ::close(fd_of(handle_));
#endif
    count(in_process().locks, hash_, -1);
}

std::pair<std::string, std::string> DriveLock::child_environment() const {
    if (!held() && !inherited_) return {};
    std::string value = os::get_env(kHeldDrivesVariable);
    if (held()) value += (value.empty() ? "" : ";") + token_;
    return {kHeldDrivesVariable, value};
}

DriveClaim::DriveClaim(const std::string& key, const std::string& lock_dir) {
    if (key.empty()) return;
    try {
        const std::string hash = key_hash(key);
        const auto until = std::chrono::steady_clock::now() + kEjectWait;
#ifdef _WIN32
        (void)lock_dir;
        // A claim takes one of a few slots; an eject looks at all of them.
        HANDLE held = nullptr;
        for (int slot = 0; slot < kClaimSlots && held == nullptr; ++slot) {
            const HANDLE m = open_mutex(slot_name(hash, slot), true).handle;
            if (m == nullptr) continue;  // one that cannot be shared: another slot
            const DWORD r = WaitForSingleObject(m, 0);
            if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) held = m;
            else CloseHandle(m);
        }
        if (held == nullptr) return;
        // An eject holds its mutex from before it looks at the slots until the drive is gone: one
        // under way now may not have seen this slot, so it is waited for (and the drive is then
        // gone, which the command finds).
        for (;;) {
            const Mutex eject = open_mutex(eject_name(hash), false);
            const HANDLE e = eject.handle;
            if (e != nullptr) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
                const DWORD r = WaitForSingleObject(e, static_cast<DWORD>(std::clamp<long long>(left.count(), 0, 60000)));
                if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(e);
                CloseHandle(e);
                break;
            }
            // Not there (no eject), or made by an eject this account may not open: waited for.
            if (eject.status != Opened::denied || std::chrono::steady_clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        handle_ = held;
#else
        const std::string name = "vaultik-use-" + hash + ".lock";
        const std::string mark = "vaultik-use-" + hash + ".root.lock";
        int own = -1;
        int shared = -1;
        if (::geteuid() == 0) {
            // Root's mark first (an eject of root's holds it alone), then the claims file.
            own = open_in_run_lock(mark, 0600, roots);
            shared = open_lock_file(name, lock_dir);
            if (own < 0) std::swap(own, shared);
        } else {
            const AccountFiles files = open_account_files(name, lock_dir);
            own = files.own;
            shared = files.shared;
        }
        // Shared with every other claim; only an eject holds one alone, until the drive is gone.
        const auto take = [&](int& fd, bool bounded) {
            for (;;) {
                if (::flock(fd, LOCK_SH | LOCK_NB) == 0) return;
                // An account (bounded) waits on the file of another account only while root's eject
                // holds its mark: another account may hold it alone for good.
                if ((errno != EWOULDBLOCK && errno != EINTR) || std::chrono::steady_clock::now() >= until ||
                    (bounded && !root_holds(mark, true))) {
                    ::close(fd);
                    fd = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        if (own >= 0) {
            take(own, false);
            if (own < 0) {  // an eject kept it for the whole wait
                if (shared >= 0) ::close(shared);
                return;
            }
        }
        if (shared >= 0) take(shared, ::geteuid() != 0);
        if (own < 0) std::swap(own, shared);
        if (own < 0) return;
        handle_ = handle_of(own);
        if (shared >= 0) shared_ = handle_of(shared);
#endif
        hash_ = hash;
        count(in_process().claims, hash_, 1);
    } catch (const std::exception&) {
    }
}

DriveClaim::~DriveClaim() {
    if (handle_ == nullptr) return;
#ifdef _WIN32
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (shared_ != nullptr) {
        ::flock(fd_of(shared_), LOCK_UN);
        ::close(fd_of(shared_));
    }
    ::flock(fd_of(handle_), LOCK_UN);
    ::close(fd_of(handle_));
#endif
    count(in_process().claims, hash_, -1);
}

EjectHold::EjectHold(const std::string& key, const std::string& lock_dir) {
    if (key.empty()) return;
    const std::string hash = key_hash(key);
    if (used_in_process(hash)) {
        busy_ = true;
        return;
    }
#ifdef _WIN32
    (void)lock_dir;
    // The eject's own mutex first: a claim taken from now on waits for it (see DriveClaim).
    const Mutex eject = open_mutex(eject_name(hash), true);
    HANDLE e = eject.handle;
    if (e != nullptr) {
        const DWORD r = WaitForSingleObject(e, 0);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
            CloseHandle(e);
            e = nullptr;
            busy_ = true;  // another eject is under way
        }
    } else if (not_to_share(eject)) {
        busy_ = true;  // one this account cannot see into: as if another eject were under way
    }
    for (int slot = 0; slot < kClaimSlots && !busy_; ++slot) {
        const Mutex slot_mutex = open_mutex(slot_name(hash, slot), false);
        const HANDLE m = slot_mutex.handle;
        if (m == nullptr) {
            // Never made (or not a mutex): free. There but not to be shared with this account
            // (another account made it, or this one may not open it): in use (fail closed).
            if (not_to_share(slot_mutex)) busy_ = true;
            continue;
        }
        const DWORD r = WaitForSingleObject(m, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(m);
        CloseHandle(m);
        if (r == WAIT_TIMEOUT) busy_ = true;
    }
    if (busy_ && e != nullptr) {
        ReleaseMutex(e);
        CloseHandle(e);
        e = nullptr;
    }
    handle_ = e;
    if (!busy_) {
        hash_ = hash;
        count(in_process().ejects, hash_, 1);
    }
#else
    const std::string name = "vaultik-use-" + hash + ".lock";
    const std::string mark = "vaultik-use-" + hash + ".root.lock";
    const bool root = ::geteuid() == 0;
    int own = -1;
    int shared = -1;
    if (root) {
        // Root's mark first (root's claims hold it), then the claims file of every account.
        own = open_in_run_lock(mark, 0600, roots);
        shared = open_lock_file(name, lock_dir);
        if (own < 0) std::swap(own, shared);
    } else {
        const AccountFiles files = open_account_files(name, lock_dir);
        own = files.own;
        shared = files.shared;
    }
    const auto close_all = [&] {
        if (own >= 0) ::close(own);
        if (shared >= 0) ::close(shared);
        own = shared = -1;
    };
    // Alone on them: no claim is held, and none can be taken until this lets go.
    if (own >= 0 && ::flock(own, LOCK_EX | LOCK_NB) != 0) {
        busy_ = errno == EWOULDBLOCK;
        close_all();
        return;
    }
    if (shared >= 0 && ::flock(shared, LOCK_EX | LOCK_NB) != 0) {
        // Root: a claim of any account (fail closed). An account: a claim or eject of root's while
        // root's mark is held, else one of another account, passed by.
        const bool busy = errno == EWOULDBLOCK && (root || root_holds(mark, false));
        ::close(shared);
        shared = -1;
        if (busy || own < 0) {
            busy_ = busy;
            close_all();
            return;
        }
    }
    if (own < 0) std::swap(own, shared);
    if (own < 0) return;
    handle_ = handle_of(own);
    if (shared >= 0) shared_ = handle_of(shared);
    hash_ = hash;
    count(in_process().ejects, hash_, 1);
#endif
}

EjectHold::~EjectHold() {
    if (!hash_.empty()) count(in_process().ejects, hash_, -1);
    if (handle_ == nullptr) return;
#ifdef _WIN32
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    if (shared_ != nullptr) {
        ::flock(fd_of(shared_), LOCK_UN);
        ::close(fd_of(shared_));
    }
    ::flock(fd_of(handle_), LOCK_UN);
    ::close(fd_of(handle_));
#endif
}

bool drive_claimed(const std::string& key, const std::string& lock_dir) {
    return EjectHold(key, lock_dir).busy();
}

namespace testing {

void set_drive_lock_hooks(const DriveLockHooks& set) {
#ifdef _WIN32
    (void)set;
#else
    Hooks& h = hooks_store();
    std::lock_guard<std::mutex> lock(h.mu);
    h.hooks = set;
#endif
}

}  // namespace testing

}  // namespace vaultik::jobs
