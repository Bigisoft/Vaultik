#include "vaultik/jobs/drive_lock.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/jobs/job.hpp"
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
#include <unistd.h>

#include <cerrno>
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
constexpr int kClaimSlots = 32;

// This process's account, as a SID ("S-1-5-21-..."); empty when it cannot be told.
std::vector<std::uint8_t> user_sid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> info(size);
    std::vector<std::uint8_t> out;
    if (size > 0 && GetTokenInformation(token, TokenUser, info.data(), size, &size)) {
        const PSID sid = reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid;
        const DWORD length = GetLengthSid(sid);
        out.resize(length);
        if (!CopySid(length, out.data(), sid)) out.clear();
    }
    CloseHandle(token);
    return out;
}

// Only an object this account, the system account or the administrators own is used: one made by
// another account (to hold it and stall this account's runs) is not.
bool trusted_owner(HANDLE object, const std::vector<std::uint8_t>& me) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd) !=
        ERROR_SUCCESS)
        return false;
    const bool ok = owner != nullptr &&
                    (IsWellKnownSid(owner, WinLocalSystemSid) || IsWellKnownSid(owner, WinBuiltinAdministratorsSid) ||
                     (!me.empty() && EqualSid(owner, const_cast<std::uint8_t*>(me.data()))));
    LocalFree(sd);
    return ok;
}

// Opens the named mutex, making it when it is not there yet (`create`), so that only this
// account, the system account and administrators may open it. Null when it cannot be used;
// `denied` then says whether it is there but this account may not open it (one a run with
// administrator rights made, say).
HANDLE open_mutex(const std::wstring& name, bool create, bool* denied = nullptr) {
    if (denied != nullptr) *denied = false;
    const std::vector<std::uint8_t> me = user_sid();
    constexpr DWORD kAccess = SYNCHRONIZE | MUTEX_MODIFY_STATE | READ_CONTROL;
    HANDLE m = nullptr;
    bool refused = false;
    if (create) {
        std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)";
        LPWSTR mine = nullptr;
        if (!me.empty() && ConvertSidToStringSidW(const_cast<std::uint8_t*>(me.data()), &mine)) {
            sddl += L"(A;;GA;;;" + std::wstring(mine) + L")";
            LocalFree(mine);
        }
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
            sa.lpSecurityDescriptor = sd;
        m = CreateMutexExW(&sa, name.c_str(), 0, kAccess);
        if (m == nullptr) refused = GetLastError() == ERROR_ACCESS_DENIED;
        if (sd != nullptr) LocalFree(sd);
    }
    if (m == nullptr) {
        m = OpenMutexW(kAccess, FALSE, name.c_str());
        if (m == nullptr && GetLastError() == ERROR_ACCESS_DENIED) refused = true;
        if (m == nullptr && denied != nullptr) *denied = refused;
    }
    if (m != nullptr && !trusted_owner(m, me)) {
        CloseHandle(m);
        m = nullptr;
    }
    return m;
}

std::wstring wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
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

// The lock file `name` all accounts share (see open_in_run_lock): an account's own is readable by
// it alone (and root), root's by all. Otherwise in `fallback_dir`. -1 when neither can be opened.
int open_lock_file(const std::string& name, const std::string& fallback_dir) {
    const int fd = open_in_run_lock(name, ::geteuid() == 0 ? 0644 : 0600, own_or_roots);
    if (fd >= 0) return fd;
    (void)os::make_dir(fallback_dir);  // the user's settings folder, which is there
    return ::open(os::join(fallback_dir, name).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
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
                     const std::function<void()>& on_wait, const std::function<bool()>& cancelled, bool fail_closed) {
    if (key.empty()) return;
    const std::string hash = key_hash(key);
    // Named by the run that started this command: tried without waiting. Busy, that run holds the
    // drive and waits for this command to end; free, it ended (this command outlived it) and the
    // lock is taken as usual.
    const bool parent = named_by_parent(hash);
    const auto busy_now = [&] {
        if (parent && !fail_closed) {
            inherited_ = true;
            return true;
        }
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + wait;
    const auto waiting = [&] {
        if (parent && fail_closed) throw DriveBusy("the run that started this command uses the backup drive");
        if (!waited_ && on_wait) on_wait();
        waited_ = true;
        if (cancelled && cancelled()) throw Error("cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw DriveBusy("another Vaultik run kept the backup drive busy for too long; try again later");
    };
    const auto pause = [&](std::chrono::milliseconds most) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::clamp(left, std::chrono::milliseconds(0), most));
    };
    // Another DriveLock of this process first (it may be this very thread).
    while (!take_in_process_lock(hash)) {
        if (busy_now()) return;
        waiting();
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
    bool denied = false;
    HANDLE m = open_mutex(L"Global\\Vaultik-drive-" + wide(hash), true, &denied);
    if (m == nullptr) {
        // There, but made by a run this account may not open (one with administrator rights): it
        // runs now. An eject waits for nothing; a run goes on without the lock, as before.
        if (denied && busy_now()) return;
        if (denied && fail_closed) throw DriveBusy("a run with administrator rights uses the backup drive");
        return;  // no lock to share (the runs then overlap, as before)
    }
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
            waiting();
        } catch (...) {
            CloseHandle(m);
            throw;
        }
    }
#else
    const std::string name = "vaultik-drive-" + hash + ".lock";
    const bool root = ::geteuid() == 0;
    // Root: its own lock first, which only root may open, then the one all accounts share.
    int own = root ? open_in_run_lock("vaultik-drive-" + hash + ".root.lock", 0600, roots) : -1;
    int shared = root ? open_in_run_lock(name, 0644, own_or_roots) : -1;
    if (own < 0) {
        own = shared >= 0 ? shared : open_lock_file(name, lock_dir);
        shared = -1;
    }
    const auto close_all = [&] {
        if (own >= 0) ::close(own);
        if (shared >= 0) ::close(shared);
        own = shared = -1;
    };
    if (own < 0) return;
    for (;;) {
        if (::flock(own, LOCK_EX | LOCK_NB) == 0) break;
        if ((errno != EWOULDBLOCK && errno != EINTR) || busy_now()) {
            close_all();
            return;
        }
        try {
            waiting();
        } catch (...) {
            close_all();
            throw;
        }
        pause(std::chrono::milliseconds(250));
    }
    if (shared >= 0) {
        // Every root run took root's own lock first, so whoever holds this one now is another
        // account: waited for only a short while (or for the whole wait, when that is shorter).
        const auto other_deadline = std::min(deadline, std::chrono::steady_clock::now() + kOtherAccountWait);
        for (;;) {
            if (::flock(shared, LOCK_EX | LOCK_NB) == 0) break;
            if (errno != EWOULDBLOCK && errno != EINTR) {
                ::close(shared);
                shared = -1;
                break;
            }
            if (std::chrono::steady_clock::now() >= other_deadline && other_deadline < deadline && !fail_closed) {
                ::close(shared);
                shared = -1;
                skipped_ = true;
                break;
            }
            try {
                waiting();
            } catch (...) {
                close_all();
                throw;
            }
            pause(std::chrono::milliseconds(250));
        }
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
            HANDLE m = open_mutex(slot_name(hash, slot), true);
            if (m == nullptr) continue;
            const DWORD r = WaitForSingleObject(m, 0);
            if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) held = m;
            else CloseHandle(m);
        }
        if (held == nullptr) return;
        // An eject holds its mutex from before it looks at the slots until the drive is gone: one
        // under way now may not have seen this slot, so it is waited for (and the drive is then
        // gone, which the command finds).
        for (;;) {
            bool denied = false;
            HANDLE e = open_mutex(eject_name(hash), false, &denied);
            if (e != nullptr) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
                const DWORD r = WaitForSingleObject(e, static_cast<DWORD>(std::clamp<long long>(left.count(), 0, 60000)));
                if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(e);
                CloseHandle(e);
                break;
            }
            // Not there (no eject), or made by an eject with administrator rights: waited for.
            if (!denied || std::chrono::steady_clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        handle_ = held;
#else
        const int fd = open_lock_file("vaultik-use-" + hash + ".lock", lock_dir);
        if (fd < 0) return;
        // Shared with every other claim; only an eject holds it alone, until the drive is gone.
        for (;;) {
            if (::flock(fd, LOCK_SH | LOCK_NB) == 0) {
                handle_ = handle_of(fd);
                break;
            }
            if ((errno != EWOULDBLOCK && errno != EINTR) || std::chrono::steady_clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (handle_ == nullptr) {
            ::close(fd);
            return;
        }
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
    bool denied = false;
    HANDLE e = open_mutex(eject_name(hash), true, &denied);
    if (e != nullptr) {
        const DWORD r = WaitForSingleObject(e, 0);
        if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED) {
            CloseHandle(e);
            e = nullptr;
            busy_ = true;  // another eject is under way
        }
    } else if (denied) {
        busy_ = true;  // an eject with administrator rights is under way
    }
    for (int slot = 0; slot < kClaimSlots && !busy_; ++slot) {
        HANDLE m = open_mutex(slot_name(hash, slot), false, &denied);
        if (m == nullptr) {
            // Never made, or made by nobody to trust; there but not to be opened by this account
            // (a command with administrator rights holds it): in use.
            if (denied) busy_ = true;
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
    const int fd = open_lock_file("vaultik-use-" + hash + ".lock", lock_dir);
    if (fd < 0) return;
    // Alone on it: no claim is held, and none can be taken until this lets go.
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        handle_ = handle_of(fd);
        hash_ = hash;
        count(in_process().ejects, hash_, 1);
        return;
    }
    busy_ = errno == EWOULDBLOCK;
    ::close(fd);
#endif
}

EjectHold::~EjectHold() {
    if (!hash_.empty()) count(in_process().ejects, hash_, -1);
    if (handle_ == nullptr) return;
#ifdef _WIN32
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    ::flock(fd_of(handle_), LOCK_UN);
    ::close(fd_of(handle_));
#endif
}

bool drive_claimed(const std::string& key, const std::string& lock_dir) {
    return EjectHold(key, lock_dir).busy();
}

}  // namespace vaultik::jobs
