#include "vaultik/jobs/drive_lock.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

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
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
// shared their turn, or by another account); `untrusted`: there, owned by nobody to trust;
// `wrong_type`: the name is taken by another kind of object (an event, say, which any account may
// make to keep the name from Vaultik); `none`: not there (and not to be made); `error`: anything
// else.
enum class Opened { ok, denied, untrusted, wrong_type, none, error };

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
        if (error == ERROR_INVALID_HANDLE) return {nullptr, Opened::wrong_type};
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

// Busy for an eject (fail closed): one not to be shared, or a name taken by another kind of object,
// behind which a run or command may be (it got no lock or claim there either).
bool busy_for_eject(const Mutex& m) {
    return not_to_share(m) || m.status == Opened::wrong_type;
}

std::wstring slot_name(const std::string& hash, int slot) {
    return L"Global\\Vaultik-use-" + wide(hash) + L"-" + std::to_wstring(slot);
}

std::wstring eject_name(const std::string& hash) {
    return L"Global\\Vaultik-eject-" + wide(hash);
}
#else
// Root's own folder for its marks (see open_root_mark).
constexpr char kRootMarkDir[] = "/run/vaultik";

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

// Opens `path` in /run/lock with `flags` only when nothing is mounted on it: another account could
// mount a FUSE file system (with allow_other) on a file of its own there, so that opening that file
// would wait on its program for as long as it likes. The file is first opened with O_PATH, which
// asks no file system, and its mount compared with the folder's by statx, which does not either;
// then it is opened through that descriptor (/proc/self/fd), so that it is the file looked at.
// Where statx cannot tell mounts (refused, or before Linux 5.8), opened as it is. -1 with errno
// otherwise (EBUSY: something is mounted on it; ELOOP: a link).
int open_unmounted(const std::string& path, int flags) {
    const int at = ::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (at < 0) return -1;
    constexpr int kLook = AT_STATX_DONT_SYNC | AT_SYMLINK_NOFOLLOW;
    struct statx file{}, dir{};
    const bool told = !hooks().no_statx &&
                      ::statx(at, "", AT_EMPTY_PATH | kLook, STATX_TYPE | STATX_MNT_ID, &file) == 0 &&
                      ::statx(AT_FDCWD, "/run/lock", kLook, STATX_MNT_ID, &dir) == 0 &&
                      (file.stx_mask & dir.stx_mask & STATX_MNT_ID) != 0;
    int error = 0;
    int fd = -1;
    if (!told) {
        fd = ::open(path.c_str(), flags);
    } else if (S_ISLNK(file.stx_mode)) {
        errno = ELOOP;
    } else if (file.stx_mnt_id != dir.stx_mnt_id) {
        errno = EBUSY;
    } else {
        fd = ::open(("/proc/self/fd/" + std::to_string(at)).c_str(), flags & ~O_NOFOLLOW);
        if (fd < 0 && errno == ENOENT) fd = ::open(path.c_str(), flags);  // no /proc
    }
    if (fd < 0) error = errno;
    ::close(at);
    errno = error;
    return fd;
}

// The prefix of the names make_named gives new files in /run/lock before they are lock files.
constexpr char kNewPrefix[] = ".vaultik-new-";

// Removes this account's files of make_named that a process killed between making one and removing
// its name left in /run/lock: only regular files this account owns (looked at by name in the
// folder, never through a link) that were last changed more than a minute ago (another process
// may be making one now), each removed by its name in the folder. Another account cannot remove
// or replace a file of this account's in sticky /run/lock. At most the first 4096 names there are
// looked at, as any account can fill /run/lock with names.
void remove_stale_new_files() {
    DIR* d = ::opendir("/run/lock");
    if (d == nullptr) return;
    const std::time_t old = std::time(nullptr) - 60;
    int left = 4096;
    while (const dirent* e = ::readdir(d)) {
        if (--left < 0) break;
        if (std::strncmp(e->d_name, kNewPrefix, sizeof kNewPrefix - 1) != 0) continue;
        struct stat st{};
        if (::fstatat(::dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(st.st_mode) &&
            st.st_uid == ::geteuid() && st.st_mtime < old)
            (void)::unlinkat(::dirfd(d), e->d_name, 0);
    }
    ::closedir(d);
}

// Makes the file `path` (`name` in /run/lock) with `mode`, holding its name (see named) from the
// moment that name is there: the name is written into a new file with a random name of its own
// first, which is then linked as `path` (never over a file there) and its own name removed, so no
// lock name ever points to a file without its name in it. -1 with errno otherwise (EEXIST: another
// run made `path` meanwhile); a file whose name could not be written whole (a full /run/lock,
// which another account may fill) is removed again. A process killed in between leaves its new
// file behind, which the next one this account makes removes (see remove_stale_new_files).
int make_named(const std::string& path, const std::string& name, mode_t mode) {
    remove_stale_new_files();
    std::uint8_t rnd[8];
    try {
        random_bytes(rnd);
    } catch (const std::exception&) {
        errno = EIO;
        return -1;
    }
    const std::string temp = std::string("/run/lock/") + kNewPrefix + to_hex(ByteSpan(rnd, sizeof rnd));
    // Writable, to write its name into it; flock needs no more.
    const int fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, mode);
    if (fd < 0) return -1;
    const ssize_t written = hooks().short_name_write ? 0 : ::write(fd, name.data(), name.size());
    int error = 0;
    if (written != static_cast<ssize_t>(name.size())) error = written < 0 ? errno : ENOSPC;
    else if (::link(temp.c_str(), path.c_str()) != 0) error = errno;
    ::unlink(temp.c_str());
    if (error == 0) return fd;
    ::close(fd);
    errno = error;
    return -1;
}

// Writes `name` into the file of `fd` (opened read only), which is empty: through a descriptor of
// its own to that very file. Left empty when it cannot be written whole.
void write_name(int fd, const std::string& name) {
    if (hooks().short_name_write) return;
    const int w = ::open(("/proc/self/fd/" + std::to_string(fd)).c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
    if (w < 0) return;
    if (::pwrite(w, name.data(), name.size(), 0) != static_cast<ssize_t>(name.size())) (void)::ftruncate(w, 0);
    ::close(w);
}

bool flock_held(int fd);

// The file `name` in /run/lock, shared by every account: the one there when `usable` accepts it,
// or a new one with `mode` (made at most once, also when several runs start at once), never one
// with something mounted on it (see open_unmounted). A file this makes holds its name (see named
// and make_named); one of this account's that an earlier version made, which holds no name, gets
// its name written into it here while it has no other name, so that a second name another account
// gives it later leaves it usable. Root removes one it cannot use and makes its own, except a file
// of root's that a process holds (see flock_held): another run of root's may hold its mark there,
// which must stay the one other accounts see; then `held` (when given) is set. -1 when there is
// none to use.
int open_in_run_lock(const std::string& name, mode_t mode, bool (*usable)(int, const struct stat&, const std::string&),
                     bool* held = nullptr) {
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
        int fd = open_unmounted(path, kFlags);
        if (fd < 0 && errno == ENOENT) {
            fd = make_named(path, name, mode);
            if (fd < 0 && errno == EEXIST) continue;  // another run made it meanwhile: use that one
        }
        if (fd < 0) {
            if ((errno == EACCES || errno == ELOOP) && root && ::unlink(path.c_str()) == 0) continue;
            return -1;
        }
        struct stat st{};
        const bool regular = ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
        if (regular && st.st_uid == ::geteuid() && st.st_nlink == 1 && st.st_size == 0) {
            write_name(fd, name);
            if (::fstat(fd, &st) != 0) st.st_nlink = 0;  // not to be used
        }
        if (regular && st.st_nlink > 0 && usable(fd, st, name)) {
            if (st.st_uid == ::geteuid()) (void)::fchmod(fd, mode);  // whatever the umask
            return fd;
        }
        // Root keeps a mark of its own that it cannot use while a process may hold it: removing it
        // would hide that holder (another run or claim of root's on an earlier mark) from the
        // ejects of other accounts, which look at the mark by its name. The trade-off: this run or
        // claim of root's goes on without a mark meanwhile (on the shared file), so an account's
        // run does not wait the whole while for it; only where root's own folder cannot be used.
        const bool keep = root && regular && st.st_uid == 0 && flock_held(fd);
        ::close(fd);
        if (keep) {
            if (held != nullptr) *held = true;
            return -1;
        }
        if (!root || ::unlink(path.c_str()) != 0) return -1;
    }
    return -1;
}

bool own_or_roots(int, const struct stat& st, const std::string&) {
    return ::geteuid() == 0 || st.st_uid == 0 || st.st_uid == ::geteuid();
}

// Whether a file (`uid`, `mode` as stat gives them) is `owner`'s, and no other account may open it.
bool private_file(uid_t uid, mode_t mode, uid_t owner) {
    return uid == owner && (mode & 077) == 0;
}

// Whether a file found by its name in a folder where any account may make names (/run/lock) is one
// that only `owner` can hold: private (see private_file), and no hard link, which another account
// may have put there (to a file of `owner`'s that `owner` holds for something else, where
// fs.protected_hardlinks is off). Only where a holder is trusted or waited for (see
// held_by_ancestor, and a claim waiting for an eject): an eject counts a file of `owner`'s by its
// name also when it is not so (fail closed), as another account can add a hard link to `owner`'s
// own file at any time.
bool private_by_name(uid_t uid, mode_t mode, std::uint64_t links, uid_t owner) {
    return private_file(uid, mode, owner) && links == 1;
}

// Whether the file of `fd` (`st` as fstat gives it), found as `name` in /run/lock, is that lock
// file: one that holds `name` as open_in_run_lock writes it into each file it makes (only the
// file's owner, or root, can write it), or an empty one with no other name (an earlier version's,
// which open_in_run_lock gives its name). Where fs.protected_hardlinks is off, another account may
// make a name its owner has not used yet first as a hard link to another file of the owner's
// (another lock file, held for another drive or for a run while a claim waits; for root, any file
// of root's on that file system, such as /run/xtables.lock), which then holds another name or
// nothing; so does such a file after its owner or root removed its own name while that link was
// there, which then has one name, but not its own. A hard link another account adds to the real
// file under a name of its own keeps it usable. Read through `fd` (a regular file, never opened
// through a link; see open_in_run_lock), at most one byte more than `name`.
bool named(int fd, const struct stat& st, const std::string& name) {
    if (st.st_nlink == 1 && st.st_size == 0) return true;
    char text[256];
    if (name.size() >= sizeof text) return false;
    const ssize_t n = ::pread(fd, text, name.size() + 1, 0);
    return n == static_cast<ssize_t>(name.size()) && name.compare(0, name.size(), text, name.size()) == 0;
}

// Root's mark in /run/lock, for open_root_mark to use: root's, only root may open it, and it is the
// mark's own file (see named; otherwise root removes the name and makes its own). A hard link to it
// (another account may add one at any time) does not keep root from using it: root would otherwise
// remake it while another run of root's holds the one there.
bool roots(int fd, const struct stat& st, const std::string& name) {
    return private_file(st.st_uid, st.st_mode, 0) && named(fd, st, name);
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

// The account's own file in /run/lock, for open_account_files to use: the account's, only it may
// open it, and it is that name's own file (see named; otherwise the file in the settings folder
// instead).
// A hard link to it (another account may add one at any time) does not keep the account from using
// it: its runs would otherwise fall back to their settings folders and stop taking turns.
bool mine(int fd, const struct stat& st, const std::string& name) {
    return private_file(st.st_uid, st.st_mode, ::geteuid()) && named(fd, st, name);
}

// The folder of every account's runtime folder.
std::string run_user_root() {
    const std::string set = hooks().run_user_root;
    return set.empty() ? std::string("/run/user") : set;
}

// The account's own runtime folder (/run/user/<uid>): a folder (not a link) that the account owns
// and nobody else may look into, so that no other account can make a file in it first. Empty when
// there is none such.
std::string run_user_dir() {
    const std::string set = hooks().run_user_dir;
    const std::string dir = set.empty() ? run_user_root() + "/" + std::to_string(::geteuid()) : set;
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::geteuid() ||
        (st.st_mode & 07777) != 0700)
        return {};
    return dir;
}

// The file `name` in that folder, made when missing: -1 when there is no such folder, or no file
// of the account's own to use there.
int open_in_run_user(const std::string& name) {
    const std::string dir = run_user_dir();
    if (dir.empty()) return -1;
    const int fd = ::open((dir + "/" + name).c_str(), O_RDONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st{};
    if (fd >= 0 && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == ::geteuid()) {
        (void)::fchmod(fd, 0600);
        return fd;
    }
    if (fd >= 0) ::close(fd);
    return -1;
}

// What a run of an account (not root) locks for `name` ("vaultik-drive-<hash>.lock"; see
// open_account_files), in this order: `own`, the account's own locks, which only processes of the
// account can hold, so that every run of the account takes turns there, then `shared`, the file all
// accounts share (when it can be used), which root's runs and those of other accounts may hold too
// and which is waited for only a short while. Any may be missing (-1). `folder` is the one of `own`
// in the settings folder (always the last), which is let go of where its file system refuses
// flocks (see folder_refused); `loose`: other accounts may open that one too (see
// open_account_files), so a run and a claim may pass it by after a while; `exposed`: other
// accounts may write to the settings folder itself (see private_folder), so they may also rename
// or remove that file, and a run and an eject that meet only there may then not meet at all.
struct AccountFiles {
    std::vector<int> own;
    int folder = -1;
    bool loose = false;
    bool exposed = false;
    int shared = -1;
};

// Whether only this account may make, rename or remove names in folder `dir`: it owns it, and
// neither other accounts nor a group other than its own may write to it. False when that cannot
// be told. A settings folder on FAT, exFAT or NTFS mounted with umask 000 is not.
bool private_folder(const std::string& dir) {
    struct stat st{};
    return ::stat(dir.c_str(), &st) == 0 && st.st_uid == ::geteuid() && (st.st_mode & 002) == 0 &&
           ((st.st_mode & 020) == 0 || st.st_gid == ::getegid());
}

// The account's own locks for `name`, so that its runs take turns whatever settings folder each
// uses (the installed app and a portable one, VAULTIK_CONFIG_DIR): the file `name` in its own
// runtime folder (see run_user_dir), where no other account can make one first, and the file
// "vaultik-drive-<hash>.<uid>.lock" in /run/lock, readable by the account alone and used only when
// it is a file the account owns that holds its name (see mine; another account may have made one
// of that name first, or given an earlier version's empty one a second name): a run without the
// runtime folder (one started outside a login, without systemd's user manager) still takes turns
// with the others there. Then always the file `name` in `lock_dir` (the settings folder, one file
// less where it is one of the others): whether the account can use its file in /run/lock depends
// on what other accounts do there (they can add a name to it and remove it again, fill /run/lock
// so that no file is made or an earlier version's file gets no name, make the name first and
// remove it again), so two processes of the account may find it usable one and not the other,
// and they must still meet: those that use the same settings folder do, there, whatever happens in
// /run/lock (a distribution whose /run/lock only root may write to too), as long as no other
// account may write to that folder (see AccountFiles). Also where
// other accounts may open that file (`loose`; see private_file): a settings folder on FAT, exFAT,
// NTFS or CIFS, as a portable copy's on a USB stick. Another account could hold such a one for
// good, so a run that holds another own file waits for it only a short while (kOtherAccountWait,
// as for the shared file), and a claim at most its wait for an eject (kEjectWait), and then goes on
// without it while another account's processes alone hold it (see other_account_holds); an eject
// still counts it as busy (fail closed). Every process takes them in this order, so none waits for
// another that waits for it.
AccountFiles open_account_files(const std::string& name, const std::string& lock_dir) {
    AccountFiles f;
    if (!hooks().own_in_folder) {
        if (const int fd = open_in_run_user(name); fd >= 0) f.own.push_back(fd);
        const std::string stem = name.substr(0, name.size() - std::string(".lock").size());
        if (const int fd = open_in_run_lock(stem + "." + std::to_string(::geteuid()) + ".lock", 0600, mine); fd >= 0)
            f.own.push_back(fd);
    }
    if (const int fd = open_in_dir(name, lock_dir); fd >= 0) {
        struct stat st{};
        const bool same = ::fstat(fd, &st) != 0 || std::any_of(f.own.begin(), f.own.end(), [&](int other) {
                              struct stat o{};
                              return ::fstat(other, &o) == 0 && o.st_dev == st.st_dev && o.st_ino == st.st_ino;
                          });
        // Taken once where the settings folder is the runtime folder (VAULTIK_CONFIG_DIR).
        if (same) {
            ::close(fd);
        } else {
            f.own.push_back(fd);
            f.folder = fd;
            f.loose = !private_file(st.st_uid, st.st_mode, ::geteuid());
        }
    }
    f.exposed = !private_folder(lock_dir);
    f.shared = open_in_run_lock(name, 0600, own_or_roots);
    return f;
}

// Whether a flock on the settings folder's file `fd` (see AccountFiles) failed as its file system
// refuses flocks for good (a home folder on NFS without its lock service, say), not as another
// process holds it: then no process can hold that file either, and it is let go of instead of
// costing the run, claim or eject its other locks. Tells by errno, right after the flock.
bool folder_refused(int fd, const AccountFiles& files) {
    return fd >= 0 && fd == files.folder && errno != EWOULDBLOCK && errno != EINTR;
}

// The folder of root's marks (see open_root_mark).
std::string root_dir() {
    const std::string set = hooks().root_dir;
    return set.empty() ? std::string(kRootMarkDir) : set;
}

// Whether `dir` may hold root's marks (root only): a folder (not a link) that root owns, that
// nobody else may write to, so that no account can put a file in a mark's place, and that every
// account may look into, so that each can see root's marks held (see root_holds). Made first when
// missing, and set to 0755 whatever the umask (one of 027 or 077 would leave it closed to the
// other accounts, which would then never see root using the drive).
bool usable_root_dir(const std::string& dir) {
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    bool ok = ::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == 0;
    if (ok && (st.st_mode & 07777) != 0755) {
        (void)::fchmod(fd, 0755);
        ok = ::fstat(fd, &st) == 0;
    }
    ::close(fd);
    return ok && (st.st_mode & 022) == 0 && (st.st_mode & 0005) == 0005;
}

// Root's mark `name` (root only; see the header): in root's own folder, where no account can make
// a file in its place (in sticky /run/lock, an account that kept making one there could keep root
// from its mark). Where that folder cannot be used, in /run/lock as before. -1 when neither can;
// then `held` is set when the one in /run/lock cannot be used but a process holds it (see
// open_in_run_lock), which an eject counts as busy.
int open_root_mark(const std::string& name, bool& held) {
    const std::string dir = root_dir();
    if (usable_root_dir(dir)) {
        const int fd = ::open((dir + "/" + name).c_str(), O_RDONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
        struct stat st{};
        if (fd >= 0 && ::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0) {
            (void)::fchmod(fd, 0600);
            return fd;
        }
        if (fd >= 0) ::close(fd);
    }
    return open_in_run_lock(name, 0600, roots, &held);
}

// What one look into other accounts' processes (root only; see private_files_of) may cost, so that
// an account cannot keep root's claims and ejects waiting with many processes or many descriptors:
// at most kScanProcesses processes and kScanDescriptors descriptors per account, each account
// apart (one over its budget hides no other account's processes), and kScanTime in all; every
// descriptor read counts, also when a process's open files are read again (see holds). An
// account over its budget, or a look over its time, is one that cannot be told (unsure).
constexpr std::size_t kScanProcesses = 256;
constexpr std::size_t kScanDescriptors = 65536;
constexpr std::chrono::seconds kScanTime{3};

struct ScanBudget {
    std::map<long long, std::pair<std::size_t, std::size_t>> used;  // per account: processes, descriptors
    std::chrono::steady_clock::time_point until =
        std::chrono::steady_clock::now() + (hooks().scan_time.count() >= 0
                                                ? hooks().scan_time
                                                : std::chrono::duration_cast<std::chrono::milliseconds>(kScanTime));
    std::size_t processes = hooks().scan_processes > 0 ? hooks().scan_processes : kScanProcesses;
    std::size_t descriptors = hooks().scan_descriptors > 0 ? hooks().scan_descriptors : kScanDescriptors;

    bool in_time() const { return std::chrono::steady_clock::now() < until; }

    // Whether one more process (`descriptor` false) or descriptor of `account` may be looked into.
    bool take(long long account, bool descriptor) {
        std::pair<std::size_t, std::size_t>& u = used[account];
        if (!in_time()) return false;
        return descriptor ? ++u.second <= descriptors : ++u.first <= processes;
    }
};

// A flock held on a file, as /proc/locks shows it (every account may read it).
struct HeldFlock {
    unsigned dev_major = 0;
    unsigned dev_minor = 0;
    unsigned long long inode = 0;
    bool write = false;
    long pid = 0;  // the process that took it
};

// The flocks held now (not the ones waited for). Empty when /proc/locks cannot be read (then
// `readable`, when given, is false). With `budget`, read only within its time (a long /proc/locks,
// which any account can make with many flocks, is cut short; then the budget is out of time).
std::vector<HeldFlock> held_flocks(bool* readable = nullptr, const ScanBudget* budget = nullptr) {
    std::vector<HeldFlock> out;
    const testing::DriveLockHooks set = hooks();
    std::ifstream in(set.proc_locks.empty() ? std::string("/proc/locks") : set.proc_locks);
    if (readable != nullptr) *readable = in.is_open();
    std::string line;
    while ((budget == nullptr || budget->in_time()) && std::getline(in, line)) {
        // "1: FLOCK  ADVISORY  WRITE 1234 fe:00:5678 0 EOF"; "1: -> FLOCK ..." for one that waits.
        std::istringstream words(line);
        std::string id, kind, advisory, type, pid, where;
        if (!(words >> id >> kind) || kind == "->" || kind != "FLOCK") continue;
        if (!(words >> advisory >> type >> pid >> where)) continue;
        HeldFlock f;
        if (std::sscanf(where.c_str(), "%x:%x:%llu", &f.dev_major, &f.dev_minor, &f.inode) != 3) continue;
        try {
            f.pid = std::stol(pid);
        } catch (const std::exception&) {
            continue;
        }
        f.write = type == "WRITE";
        if (set.locks_other_device) {
            f.dev_major ^= 0x800;
            f.dev_minor ^= 0x800;
        }
        out.push_back(f);
    }
    return out;
}

// A file as /proc/locks names it. /proc/locks gives the device of the file's file system, which
// /proc/self/mountinfo shows for the file's mount; stat gives the same one, except on btrfs, which
// gives each subvolume a device of its own while /proc/locks gives them all the file system's, and
// a subvolume's inode numbers repeat in the others (any account may make a subvolume of its own).
struct LockFile {
    dev_t dev = 0;  // as stat gives it
    ino_t ino = 0;
    uid_t uid = 0;
    mode_t mode = 0;
    std::uint64_t links = 0;
    bool has_mount_id = false;  // statx gave its mount's ID
    std::uint64_t mount_id = 0;
    bool mount_known = false;  // the device of its mount could be told (see with_mount)
    unsigned mount_major = 0;
    unsigned mount_minor = 0;
    std::string path;  // where it was found by its name (see lock_file_named); empty otherwise
};

// The mounts, from /proc/self/mountinfo ("36 35 98:0 / /mnt rw - ext4 /dev/sda1 rw"; `file`: the
// one of another process instead), read at most once for one look at the drive's locks however
// many files it looks at: the device of each, and which are FUSE mounts, whose files root never
// looks at (a FUSE program of another account, mounted with allow_other, could keep each look
// waiting for as long as it likes).
struct Mounts {
    std::string file = "/proc/self/mountinfo";
    bool read = false;
    std::map<std::uint64_t, std::pair<unsigned, unsigned>> devices;
    std::vector<std::uint64_t> ids;
    std::vector<std::uint64_t> fuse_ids;
    std::vector<std::string> fuse_points;

    void read_all() {
        if (read) return;
        read = true;
        const std::string set = hooks().mountinfo;
        std::ifstream in(set.empty() ? file : set);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream words(line);
            std::uint64_t id = 0;
            std::uint64_t parent = 0;
            std::string dev, root, point, word;
            unsigned a = 0;
            unsigned b = 0;
            if (!(words >> id >> parent >> dev >> root >> point)) continue;
            ids.push_back(id);
            if (std::sscanf(dev.c_str(), "%u:%u", &a, &b) == 2) devices[id] = {a, b};
            while (words >> word && word != "-") {
            }
            std::string type;
            if (!(words >> type) || (type != "fuse" && type != "fuseblk" && type.rfind("fuse.", 0) != 0)) continue;
            fuse_ids.push_back(id);
            fuse_points.push_back(unescape(point));
        }
    }

    // The device of the mount `mount_id`. False when it cannot be told.
    bool device(std::uint64_t mount_id, unsigned& major_of, unsigned& minor_of) {
        read_all();
        const auto it = devices.find(mount_id);
        if (it == devices.end()) return false;
        major_of = it->second.first;
        minor_of = it->second.second;
        return true;
    }

    // Whether the mount `mount_id` is one of these.
    bool has(std::uint64_t mount_id) {
        read_all();
        return std::find(ids.begin(), ids.end(), mount_id) != ids.end();
    }

    // Whether the mount `mount_id` is a FUSE mount.
    bool fuse(std::uint64_t mount_id) {
        read_all();
        return std::find(fuse_ids.begin(), fuse_ids.end(), mount_id) != fuse_ids.end();
    }

    // Whether a FUSE mount is mounted on `path` (a folder or file), so that looking at it would ask
    // that FUSE program.
    bool fuse_at(const std::string& path) {
        read_all();
        return std::find(fuse_points.begin(), fuse_points.end(), path) != fuse_points.end();
    }

    // A mount point as mountinfo writes it, with "\040" for a space and so on.
    static std::string unescape(const std::string& text) {
        std::string out;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\\' && i + 3 < text.size()) {
                const std::string digits = text.substr(i + 1, 3);
                if (std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '7'; })) {
                    out.push_back(static_cast<char>(std::stoi(digits, nullptr, 8)));
                    i += 3;
                    continue;
                }
            }
            out.push_back(text[i]);
        }
        return out;
    }
};

// Whether /proc/locks may give another device than stat for the file of `fd` (see with_mount): on
// btrfs (each subvolume has a device of its own) and overlayfs (stat gives the overlay's device,
// /proc/locks the one of the file system below), or when that cannot be told.
bool device_may_differ(int fd) {
    constexpr long kBtrfs = 0x9123683e;
    constexpr long kOverlay = 0x794c7630;
    if (hooks().btrfs) return true;
    struct statfs fs{};
    if (::fstatfs(fd, &fs) != 0) return true;
    const long type = static_cast<long>(fs.f_type);
    return type == kBtrfs || type == kOverlay;
}

// The file `path` relative to `dirfd` (fstatat's `flags`), without the device of its mount (see
// with_mount, asked only for a file that is of use). statx takes what the system has at hand
// (AT_STATX_DONT_SYNC), so that a file of a network or FUSE file system (another account's, say)
// is never asked for. Where statx is refused (an older kernel, or a seccomp filter, as older Docker
// versions have), as fstatat gives it. False when it cannot be looked at.
bool lock_file_at(int dirfd, const char* path, int flags, LockFile& f) {
    f = LockFile{};
    struct statx sx{};
    const int r = hooks().no_statx ? (errno = EPERM, -1)
                                   : ::statx(dirfd, path, flags | AT_STATX_DONT_SYNC, STATX_BASIC_STATS | STATX_MNT_ID, &sx);
    if (r != 0) {
        if (errno != ENOSYS && errno != EPERM) return false;
        struct stat st{};
        if (::fstatat(dirfd, path, &st, flags) != 0) return false;
        f.dev = st.st_dev;
        f.ino = st.st_ino;
        f.uid = st.st_uid;
        f.mode = st.st_mode;
        f.links = st.st_nlink;
        return true;
    }
    f.dev = makedev(sx.stx_dev_major, sx.stx_dev_minor);
    f.ino = static_cast<ino_t>(sx.stx_ino);
    f.uid = sx.stx_uid;
    f.mode = sx.stx_mode;
    f.links = sx.stx_nlink;
    f.has_mount_id = (sx.stx_mask & STATX_MNT_ID) != 0;
    f.mount_id = sx.stx_mnt_id;
    return true;
}

// Adds to `f` (of lock_file_at on `fd`) the device of its mount, where statx and
// /proc/self/mountinfo (`mounts`) tell it (Linux 5.8 and later). Where they cannot, the device stat
// gives is the one /proc/locks gives, except on file systems where the two may differ (see
// device_may_differ; without `fd`, -1: may differ, without asking the file system).
void with_mount(int fd, LockFile& f, Mounts& mounts) {
    const testing::DriveLockHooks set = hooks();
    if (f.has_mount_id && !set.no_mount_device)
        f.mount_known = mounts.device(f.mount_id, f.mount_major, f.mount_minor);
    if (f.mount_known && set.locks_other_device) {  // as on btrfs, where it is /proc/locks's
        f.mount_major ^= 0x800;
        f.mount_minor ^= 0x800;
    }
    if (!f.mount_known && fd >= 0 && !device_may_differ(fd)) {
        f.mount_known = true;
        f.mount_major = major(f.dev);
        f.mount_minor = minor(f.dev);
    }
}

// The file of `fd`, with the device of its mount (see lock_file_at and with_mount).
bool lock_file_of(int fd, LockFile& f, Mounts& mounts) {
    if (!lock_file_at(fd, "", AT_EMPTY_PATH, f)) return false;
    with_mount(fd, f, mounts);
    return true;
}

// The regular file at `path`, never following a link, with the device of its mount: opened with
// O_PATH first and then only looked at through that descriptor, so that it is the same file both
// times, also when its owner swaps it for a link in between. False when it cannot be looked at, is
// no regular file or is on a FUSE mount (see Mounts), whose file system is then never asked. On a
// mount that `mounts` does not show (one mounted since they were read: a FUSE file system another
// account mounted with allow_other on its own file in /run/lock, say), the device of the file's
// mount is left unknown: its file system is not asked either (statfs would ask its program).
bool lock_file_named(const std::string& path, LockFile& f, Mounts& mounts) {
    const int fd = ::open(path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok =
        lock_file_at(fd, "", AT_EMPTY_PATH, f) && S_ISREG(f.mode) && !(f.has_mount_id && mounts.fuse(f.mount_id));
    if (ok) with_mount(f.has_mount_id && !mounts.has(f.mount_id) ? -1 : fd, f, mounts);
    ::close(fd);
    f.path = path;
    return ok;
}

// What trying a flock on a file told (see flock_busy).
enum class Probe {
    free,      // taken and let go of at once
    busy,      // held by another
    untried,   // cannot be tried where statx gives no mount ID (before Linux 5.8, or refused)
    unknown,   // could not be tried for another reason (the file was swapped or removed meanwhile,
               // or is on a mount root's mount table does not show, or the open or flock failed)
};

// Root's eject: whether a process holds `file` (found by its name, see lock_file_named), told by
// trying a flock on it that never waits and is let go of at once. This sees a holder in any pid
// namespace (another container that shares the folder), which /proc/locks leaves out. The file is
// opened only when it is still the same regular file, on a mount in root's mount table that is no
// FUSE mount (whose program could keep the open waiting), never made, and never through a link.
// That needs the mount ID of statx (Linux 5.8 and later, where statx is allowed): without it, the
// file is never tried (untried), and only /proc/locks tells.
Probe flock_busy(const LockFile& file, Mounts& mounts) {
    if (!file.has_mount_id) return Probe::untried;
    const int at = ::open(file.path.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC);
    if (at < 0) return Probe::unknown;
    LockFile now;
    int fd = -1;
    if (lock_file_at(at, "", AT_EMPTY_PATH, now) && S_ISREG(now.mode) && now.dev == file.dev && now.ino == file.ino &&
        now.has_mount_id && mounts.has(now.mount_id) && !mounts.fuse(now.mount_id))
        fd = ::open(("/proc/self/fd/" + std::to_string(at)).c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    ::close(at);
    if (fd < 0) return Probe::unknown;
    Probe probe = Probe::free;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) ::flock(fd, LOCK_UN);
    else probe = errno == EWOULDBLOCK ? Probe::busy : Probe::unknown;
    ::close(fd);
    return probe;
}

// How a line of /proc/locks is about a file.
enum class Match {
    no,     // about another file
    exact,  // the device and inode stat gives: this file
    mount,  // the inode on the device of the file's mount, which is not the one stat gives (btrfs):
            // this file, or the one with that inode in another subvolume
    maybe,  // the inode on another device, where the device of the file's mount cannot be told
};

Match match(const HeldFlock& f, const LockFile& file) {
    if (f.inode != file.ino) return Match::no;
    if (f.dev_major == major(file.dev) && f.dev_minor == minor(file.dev)) return Match::exact;
    if (!file.mount_known) return Match::maybe;
    return f.dev_major == file.mount_major && f.dev_minor == file.mount_minor ? Match::mount : Match::no;
}

bool is_file(const HeldFlock& f, const LockFile& file) {
    return match(f, file) == Match::exact;
}

// The files process `pid` (of account `account`) has open, from /proc/<pid>/fd (readable for the
// processes of this account, and for root), without the devices of their mounts, each descriptor
// within the account's budget (see ScanBudget). False when they cannot be read, or not within it.
bool open_files_of(long pid, long long account, ScanBudget& budget, std::vector<LockFile>& files) {
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    while (const dirent* e = ::readdir(d)) {
        const std::string entry = dir + "/" + e->d_name;
        if (e->d_name[0] == '.') continue;
        if (!budget.take(account, true)) {
            ::closedir(d);
            return false;
        }
        LockFile f;
        if (!lock_file_at(AT_FDCWD, entry.c_str(), 0, f)) continue;  // the file itself, not the link
        files.push_back(f);
    }
    ::closedir(d);
    return true;
}

// The files named `name` that process `pid` (of account `account`) has open (root only; see
// open_files_of) that the account owns and no other account may open (as open_in_dir and
// open_in_run_user make them): its drive or claims file in its settings folder or runtime folder.
// Each file is looked at only once its name matches, so a process with many files open costs a
// readlink for each, within the account's budget (see ScanBudget; `unsure` is set when it runs
// out). One on a FUSE mount (see Mounts) is not looked at, which /proc/<pid>/fdinfo tells without
// asking the FUSE program: its mount is looked up in this process's mounts, else in those of the
// holder (another mount namespace); one in neither (a mount taken away with umount -l, which may
// still be a FUSE one) is not looked at either, and leaves `unsure` set. A file swapped in after
// that (dup2) is not asked for its details either (see lock_file_at), and one on another mount
// than fdinfo said leaves `unsure` set too; the file system of these files is never asked about
// (see with_mount).
std::vector<LockFile> private_files_of(long pid, long long account, const std::string& name, Mounts& mounts,
                                       ScanBudget& budget, bool& unsure) {
    std::vector<LockFile> files;
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return files;
    Mounts theirs;  // the holder's mounts, read when needed
    theirs.file.clear();
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (!budget.take(account, true)) {
            unsure = true;
            break;
        }
        const std::string entry = dir + "/" + e->d_name;
        char target[4096];
        const ssize_t n = ::readlink(entry.c_str(), target, sizeof target - 1);
        const std::string path(target, n > 0 ? static_cast<std::size_t>(n) : 0);
        if (path.size() <= name.size() || path.compare(path.size() - name.size(), name.size(), name) != 0 ||
            path[path.size() - name.size() - 1] != '/')
            continue;
        std::ifstream info("/proc/" + std::to_string(pid) + "/fdinfo/" + e->d_name);
        std::string line;
        bool known = false;
        std::uint64_t mount_id = 0;
        while (std::getline(info, line))
            if (line.rfind("mnt_id:", 0) == 0) {
                known = true;
                mount_id = std::strtoull(line.c_str() + 7, nullptr, 10);
                break;
            }
        Mounts* table = nullptr;
        if (known && mounts.has(mount_id)) {
            table = &mounts;
        } else if (known) {
            if (theirs.file.empty()) theirs.file = "/proc/" + std::to_string(pid) + "/mountinfo";
            if (theirs.has(mount_id)) table = &theirs;
        }
        if (table == nullptr) {
            unsure = true;
            continue;
        }
        if (table->fuse(mount_id)) continue;
        LockFile f;
        if (!lock_file_at(AT_FDCWD, entry.c_str(), 0, f) || !S_ISREG(f.mode) ||
            static_cast<long long>(f.uid) != account || (f.mode & 077) != 0)
            continue;
        if (f.has_mount_id && f.mount_id != mount_id) {
            unsure = true;
            continue;
        }
        with_mount(-1, f, *table);
        files.push_back(f);
    }
    ::closedir(d);
    return files;
}

// Whether process `pid` (of account `account`) holds a flock (`write_only`: holds it alone) on
// `file`, as /proc/locks (`held`) names it: on the device and inode stat gives, or, where
// /proc/locks gives the file system's device (btrfs), on that one when the process has this very
// file open (the inode alone may be another subvolume's file; read within `budget`). `unsure` is
// set where it may but that cannot be told (the device of the file's mount cannot be told, or the
// open files of `pid` cannot be read, or not within the budget).
bool holds(long pid, long long account, const LockFile& file, const std::vector<HeldFlock>& held, bool write_only,
           ScanBudget& budget, bool& unsure) {
    bool on_mount = false;
    for (const HeldFlock& f : held) {
        if (f.pid != pid || (write_only && !f.write)) continue;
        const Match m = match(f, file);
        if (m == Match::exact) return true;
        if (m == Match::mount) on_mount = true;
        if (m == Match::maybe) unsure = true;
    }
    if (!on_mount) return false;
    std::vector<LockFile> open;
    if (!open_files_of(pid, account, budget, open)) {
        unsure = true;
        return false;
    }
    return std::any_of(open.begin(), open.end(),
                       [&](const LockFile& o) { return o.dev == file.dev && o.ino == file.ino; });
}

// Root's mark files `name`, in root's folder or, as earlier versions kept it, in /run/lock: only
// the ones root owns that only root may open, and the one in /run/lock only when it is no hard link
// either (see private_by_name; only root can make a name in its folder), so only root can hold
// them. For an eject (`eject`), every file of root's by these names (fail closed: another account
// can add a hard link to root's mark at any time).
std::vector<LockFile> root_mark_files(const std::string& name, Mounts& mounts, bool eject) {
    std::vector<LockFile> files;
    for (const std::string& dir : {root_dir(), std::string("/run/lock")}) {
        const std::string path = dir + "/" + name;
        LockFile f;
        if (!lock_file_named(path, f, mounts) || f.uid != hooks().root_uid) continue;
        const std::uint64_t links = dir == "/run/lock" ? f.links : 1;  // see above
        if (!eject && !private_by_name(f.uid, f.mode, links, hooks().root_uid)) continue;
        files.push_back(f);
    }
    return files;
}

// Whether root holds its mark `name` (`write_only`: holds it alone, as an eject does); every
// account can see in /proc/locks that it is held. False when that cannot be told. `eject`: asked
// for an eject (see root_mark_files). For an eject and a run, /proc/locks is read within the time
// of a look (see ScanBudget), as any account can make it long and a run asks again every 250 ms:
// cut short, the mark counts as held for an eject (fail closed) and as not held for a run, which
// then waits for root's run only as for another account's (kOtherAccountWait), so that no account
// can keep it waiting longer that way. A claim waiting for root's eject (`write_only`) reads it
// whole, as before: cut short, it would either let a command start while root's eject is under way
// or let another account delay every command by the claim's whole wait (kEjectWait).
bool root_holds(const std::string& name, bool write_only, bool eject) {
    Mounts mounts;
    const std::vector<LockFile> files = root_mark_files(name, mounts, eject);
    if (files.empty()) return false;
    const ScanBudget budget;
    const std::vector<HeldFlock> held = held_flocks(nullptr, write_only ? nullptr : &budget);
    if (!write_only && !budget.in_time()) return eject;
    for (const HeldFlock& f : held)
        for (const LockFile& id : files)
            if (is_file(f, id) && (!write_only || f.write)) return true;
    return false;
}

// The parent of process `pid`, from /proc/<pid>/stat ("pid (name) state ppid ..."; the name may
// hold spaces and parentheses, so it is read from after the last ')'). 0 when it cannot be told.
long parent_of(long pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string stat;
    if (!std::getline(in, stat)) return 0;
    const std::size_t close = stat.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream rest(stat.substr(close + 1));
    std::string state;
    long ppid = 0;
    if (!(rest >> state >> ppid)) return 0;
    return ppid;
}

// The accounts of process `pid` (real, effective, saved, file system), from the Uid: line of
// /proc/<pid>/status (readable by every account, and true also for a process that made itself not
// dumpable, which /proc/<pid> itself shows as root's). Empty when they cannot be told.
std::vector<long long> accounts_of(long pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Uid:", 0) != 0) continue;
        std::istringstream words(line.substr(4));
        std::vector<long long> ids;
        long long id = -1;
        while (words >> id) ids.push_back(id);
        if (ids.size() != 4 || !words.eof()) return {};
        return ids;
    }
    return {};
}

// The effective account of process `pid`; -1 when it cannot be told.
long long account_of(long pid) {
    const std::vector<long long> ids = accounts_of(pid);
    return ids.empty() ? -1 : ids[1];
}

// Whether process `pid` is root's through and through: its real account too, not only the
// effective one, which a setuid-root program started by another account has as well (passwd or su
// waiting at their prompt, with a descriptor of that account's kept open).
bool root_process(long pid) {
    const std::vector<long long> ids = accounts_of(pid);
    const auto root = static_cast<long long>(hooks().root_uid);
    return !ids.empty() && std::all_of(ids.begin(), ids.end(), [&](long long id) { return id == root; });
}

// The runtime folder of account `uid` (see run_user_dir) as far as this process can tell: empty
// when it is no folder that account owns alone, or a FUSE mount (see Mounts), which is not looked
// at.
std::string run_user_dir_of(long long uid, Mounts& mounts) {
    if (uid == static_cast<long long>(::geteuid())) return run_user_dir();
    const std::string dir = run_user_root() + "/" + std::to_string(uid);
    if (mounts.fuse_at(dir)) return {};
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || static_cast<long long>(st.st_uid) != uid ||
        (st.st_mode & 07777) != 0700)
        return {};
    return dir;
}

// The own lock files of account `uid` for `name` (see open_account_files) that this process can
// see: the one in its runtime folder and its file in /run/lock, each only when that account owns it
// and no other account may open it, and the one in /run/lock only when it is no hard link either
// (see private_by_name; only the account can make a name in its runtime folder), so that no process
// of another account can hold it. For an eject (`eject`), each one that account owns (fail closed:
// another account can add a hard link to the account's own file at any time, also while it is
// held).
std::vector<LockFile> own_files_of(long long uid, const std::string& name, Mounts& mounts, bool eject) {
    std::vector<LockFile> files;
    if (uid < 0) return files;
    const std::string stem = name.substr(0, name.size() - std::string(".lock").size());
    std::vector<std::string> paths = {"/run/lock/" + stem + "." + std::to_string(uid) + ".lock"};
    if (const std::string dir = run_user_dir_of(uid, mounts); !dir.empty()) paths.push_back(dir + "/" + name);
    for (const std::string& path : paths) {
        if (mounts.fuse_at(path)) continue;
        LockFile f;
        if (!lock_file_named(path, f, mounts)) continue;
        const std::uint64_t links = path.rfind("/run/lock/", 0) == 0 ? f.links : 1;  // see above
        if (static_cast<long long>(f.uid) != uid) continue;
        if (!eject && !private_by_name(f.uid, f.mode, links, static_cast<uid_t>(uid))) continue;
        files.push_back(f);
    }
    return files;
}

// Root only: the own lock files for `name` of every other account (see own_files_of; `eject`: for
// an eject) that are there: each vaultik-...<uid>.lock in /run/lock and `name` in each account's
// runtime folder.
// Within the look's time (`budget`; `unsure` is set when it runs out): any account can make many
// names in /run/lock, so a name there counts only for the account that owns its file, the one
// its own file can be, and each account adds one name at most however many it makes.
std::vector<LockFile> accounts_own_files(const std::string& name, Mounts& mounts, const ScanBudget& budget,
                                         bool eject, bool& unsure) {
    std::vector<long long> uids;
    const auto add = [&](const std::string& text) {
        if (text.empty() || text.size() > 10 || !std::all_of(text.begin(), text.end(), [](char c) {
                return c >= '0' && c <= '9';
            }))
            return;
        if (const long long uid = std::stoll(text); uid != static_cast<long long>(::geteuid())) uids.push_back(uid);
    };
    const std::string prefix = name.substr(0, name.size() - std::string(".lock").size()) + ".";
    const std::string suffix = ".lock";
    const auto out_of_time = [&] {
        if (budget.in_time()) return false;
        unsure = true;
        return true;
    };
    if (DIR* d = ::opendir("/run/lock")) {
        while (const dirent* e = ::readdir(d)) {
            if (out_of_time()) break;
            const std::string entry = e->d_name;
            if (entry.size() <= prefix.size() + suffix.size() || entry.compare(0, prefix.size(), prefix) != 0 ||
                entry.compare(entry.size() - suffix.size(), suffix.size(), suffix) != 0)
                continue;
            const std::string uid = entry.substr(prefix.size(), entry.size() - prefix.size() - suffix.size());
            // Never a file with a FUSE mount on it (see own_files_of): where statx is refused, its
            // details would be asked of its program.
            if (mounts.fuse_at("/run/lock/" + entry)) continue;
            LockFile f;
            if (lock_file_at(::dirfd(d), entry.c_str(), AT_SYMLINK_NOFOLLOW, f) && std::to_string(f.uid) == uid) add(uid);
        }
        ::closedir(d);
    }
    if (DIR* d = ::opendir(run_user_root().c_str())) {
        while (const dirent* e = ::readdir(d)) {
            if (out_of_time()) break;
            add(e->d_name);
        }
        ::closedir(d);
    }
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
    std::vector<LockFile> files;
    for (const long long uid : uids) {
        if (out_of_time()) break;
        const std::vector<LockFile> own = own_files_of(uid, name, mounts, eject);
        files.insert(files.end(), own.begin(), own.end());
    }
    return files;
}

// Whether another account's run, claim or eject may hold one of its own files for `name` (root
// only; see accounts_own_files), which it may do without the file all accounts share (another
// account held that one when it started, or made it first): busy when /proc/locks shows one held
// (`write_only`: held alone, as by a run or an eject), unsure when that cannot be told
// (/proc/locks cannot be read, or the device of a file's mount cannot). For an eject (not
// `write_only`) these are all the files of each account's own names that the account owns (see
// own_files_of), also one with a hard link or that other accounts may open, so that no other
// account can hide a held one from root's eject by linking it. /proc/locks leaves out a flock whose
// holder is in another pid namespace (another container that shares /run/lock), so for an eject
// each of these files is also tried itself (see flock_busy); one that cannot be tried is unsure,
// except where statx gives no mount ID at all (then /proc/locks alone tells, which sees no holder
// in another pid namespace).
//
// Another account may also make an account's file in /run/lock first, so that a run of that
// account without its runtime folder (started by cron, without a login) holds only the file in its
// settings folder, which root cannot find by name: so every process that /proc/locks shows holding
// a flock is looked into as well (/proc/<pid>/fd), and one holding a file `name` there that its
// account owns and no other account may open (see private_files_of) counts as busy too, within
// each account's budget (see ScanBudget; an account over it is unsure). The whole look, from the
// names in /run/lock and /proc/locks on, stays within the budget's time (over it is unsure). A
// line without a process (pid 0, which mainline Linux never shows: it leaves such a holder out)
// cannot be looked into, and is unsure too. Never this process, whose own locks and claims are
// counted apart.
enum class Use { free, busy, unsure };

Use accounts_hold(const std::string& name, bool write_only) {
    ScanBudget budget;
    Mounts mounts;
    bool unsure = false;
    const std::vector<LockFile> files = accounts_own_files(name, mounts, budget, !write_only, unsure);
    if (!write_only)
        for (const LockFile& file : files) {
            const Probe probe = flock_busy(file, mounts);
            if (probe == Probe::busy) return Use::busy;
            if (probe == Probe::unknown) unsure = true;
        }
    bool readable = false;
    const std::vector<HeldFlock> held = held_flocks(&readable, &budget);
    if (!readable) return files.empty() && !unsure ? Use::free : Use::unsure;
    if (!budget.in_time()) unsure = true;  // /proc/locks may have been cut short
    std::vector<long> pids;
    for (const HeldFlock& f : held) {
        if (write_only && !f.write) continue;
        for (const LockFile& file : files) {
            const Match m = match(f, file);
            if (m == Match::exact || m == Match::mount) return Use::busy;
            if (m == Match::maybe) unsure = true;
        }
        if (f.pid <= 0) unsure = true;  // a holder without a process to look into
        else if (f.pid != static_cast<long>(::getpid())) pids.push_back(f.pid);
    }
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    for (const long pid : pids) {
        const long long account = account_of(pid);
        if (!budget.take(account, false)) {
            unsure = true;
            if (!budget.in_time()) break;
            continue;
        }
        bool maybe = false;
        for (const LockFile& f : private_files_of(pid, account, name, mounts, budget, maybe)) {
            if (!budget.in_time()) {
                maybe = true;
                break;
            }
            if (holds(pid, account, f, held, write_only, budget, maybe)) return Use::busy;
        }
        if (maybe) unsure = true;
    }
    return unsure ? Use::unsure : Use::free;
}

// An account's eject (not root): whether a process other than this one holds one of this
// account's own files for `name` that it finds by its name (see own_files_of, for an eject: any
// link count, also one its runs and claims no longer use as it has another name), as /proc/locks
// shows (never by taking the flock, so that a name another account linked to another drive's file
// never makes the eject hold that file). Fail closed: a run or claim of the account may hold such a
// file, which another account linked under a second name after it was taken. Files the eject holds
// itself (`held`, its descriptors; told by device and inode) are left out, and when no other is
// left, /proc/locks is not read at all (an eject where /proc is missing still goes on). Otherwise
// busy also where /proc/locks cannot be read, where reading it was cut short (it is read within
// the time of a look, see ScanBudget, and any account can make it long), and where a line may be
// about such a file but that cannot be told (Match::maybe). Not with own_in_folder (tests only),
// whose own lock is the folder's alone.
bool own_held_elsewhere(const std::string& name, std::initializer_list<int> held) {
    if (hooks().own_in_folder) return false;
    Mounts mounts;
    std::vector<LockFile> files = own_files_of(static_cast<long long>(::geteuid()), name, mounts, true);
    files.erase(std::remove_if(files.begin(), files.end(),
                               [&](const LockFile& file) {
                                   return std::any_of(held.begin(), held.end(), [&](int fd) {
                                       struct stat st{};
                                       return fd >= 0 && ::fstat(fd, &st) == 0 && st.st_dev == file.dev &&
                                              st.st_ino == file.ino;
                                   });
                               }),
                files.end());
    if (files.empty()) return false;
    const ScanBudget budget;
    bool readable = false;
    const std::vector<HeldFlock> flocks = held_flocks(&readable, &budget);
    if (!readable || !budget.in_time()) return true;
    for (const HeldFlock& f : flocks) {
        if (f.pid == static_cast<long>(::getpid())) continue;
        for (const LockFile& file : files)
            if (match(f, file) != Match::no) return true;
    }
    return false;
}

// Whether a holder of `file` may be trusted as the run that started this command: a file this
// account owns that no other account may open (root's mark and fallback, an account's own files),
// whatever its path. Any other file (the file all accounts share, or one that another account
// made in its place and then moved away) any account may hold.
bool only_mine(const LockFile& file) {
    return file.uid == ::geteuid() && (file.mode & 077) == 0;
}

// Whether the file of `fd` is the file `name` in /run/lock that every account shares (root uses it
// as its own lock where it has no mark).
bool run_lock_file(int fd, const std::string& name) {
    struct stat mine{}, there{};
    return ::fstat(fd, &mine) == 0 && ::lstat(("/run/lock/" + name).c_str(), &there) == 0 &&
           mine.st_dev == there.st_dev && mine.st_ino == there.st_ino;
}

// Tests only (own_in_folder, where this account's processes stand in for other accounts): whether
// the file of `fd` is that shared file, which then counts as one other accounts may open.
bool shared_stand_in(int fd, const std::string& name) {
    return hooks().own_in_folder && run_lock_file(fd, name);
}

// What /proc tells about who holds a lock (see held_by_ancestor).
enum class Holder {
    parent,   // a process this one descends from
    other,    // only processes that are not
    unknown,  // no answer to use: the file shows no holder at all, or the processes above this one
              // cannot all be read (another pid namespace, /proc mounted with hidepid), or one of
              // them may hold a lock that /proc/locks cannot tell from another file's, or the busy
              // file is an own lock of this account that other accounts may open too or that has
              // another name (never the file all accounts share, which root uses as its own where
              // it has no mark)
};

// Whether a process this one descends from (its parent, or one further up: a run's before or
// after command reaches vaultik through a shell, sudo or pkexec, which keep no environment of
// it) holds a flock on the file of `fd` (none when -1), on root's mark `mark` or on one of its own
// account's own files (see own_files_of): then the run that started this command holds the drive,
// and waits for the command to end. Never this process itself (another DriveLock of it on the
// drive counts as busy), and never a process that is no ancestor (a command that outlived its
// run, or another account's run).
//
// /proc/locks names the process that took a flock, also after it ended while another process still
// holds the file open: so an account's own file counts only for a process whose account is that one
// now (another account may have taken a lock on its own file in a process whose number then goes to
// a process the victim's commands descend from, its login shell or sudo, say), and any account may
// hold the file `name` all accounts share (`fd` may be that one): a holder of that file never
// counts on its own. Which file `fd` is, is told by the file itself, never by its path: it counts
// as a lock no other account can take only when this account owns it, no other account may open
// it (see only_mine) and it has no other name, as another account may make the shared file first,
// hold it and then move it away, so that it is no longer the one at its path, or make the name
// first as a hard link to another lock file of this account's (where fs.protected_hardlinks is
// off), which a run above this one holds for another drive. One of this run's own locks
// (`own_file`, not the file all accounts share) that this account owns but other accounts may open
// too (its settings folder on FAT, exFAT, NTFS or CIFS, whose mode bits cannot be set so) or that
// has another name is not trusted either, but leaves no answer (unknown) where none of the files
// above makes the holder a parent, so that the marker tells (never for an eject) instead of the
// command waiting for its own run.
// Root's own lock where it has no mark is the file all accounts share (every account may open it):
// a holder of it that is no parent is another (other), as for any holder of that file. `unseen` is
// set when the busy file shows no holder at all in /proc/locks.
// A process counts when it holds a lock no other account can take, one of its account's own files,
// root's mark or, for root (a command run with sudo; only root may look at the open files of
// another account's process), a file of that name it
// has open (/proc/<pid>/fd) that its account owns and no other account may open: the settings
// folder's, the only own file of a run without its runtime folder where only root may write to
// /run/lock or where another account made its file there first, also when another account holds the
// shared file. Otherwise another account could take the shared file from a process whose number
// then goes to a process the victim's commands descend from: they would all take it for their
// parent run and drop their turns. Holders of root's mark (only root may open it) count as they
// are. A holder of the shared file that cannot be trusted so is an answer (other), never unknown.
//
// Each lock counts only on the file's own device and inode (see Match): a line of /proc/locks with
// the same inode on another device may be any file of any account (tmpfs inode numbers are easy to
// match), so it never makes a parent; where the file's device cannot be told, such a line held by
// a process above this one leaves no answer (unknown).
// /proc/locks is read whole here, not within a look's time: cut short, the command would have to
// either wait for its own run (another account could then keep it waiting the whole wait) or leave
// it to the marker (which may then make it inherit a drive its run no longer holds). A run asks
// this on each 250 ms turn of its wait, so where another account makes /proc/locks long, each turn
// takes that much longer: a cancel is seen later, and the wait may end past its deadline by up to
// one read.
Holder held_by_ancestor(int fd, bool own_file, const std::string& name, const std::string& mark, bool& unseen) {
    const std::vector<HeldFlock> held = held_flocks();
    Mounts mounts;
    std::vector<LockFile> trusted = root_mark_files(mark, mounts, false);
    bool shared = false;
    bool loose = false;  // an own lock of this account that other accounts may open too
    LockFile target;
    const bool busy_file = fd >= 0 && lock_file_of(fd, target, mounts);
    if (busy_file) {
        // Trusted only with no other name: another account may have made the busy file's name (an
        // own file's or the shared one's) first as a hard link to another lock file of this
        // account's (where fs.protected_hardlinks is off), which a run above this one holds for
        // another drive.
        shared = !only_mine(target) || target.links != 1 || shared_stand_in(fd, name);
        loose = own_file && shared && target.uid == ::geteuid() && !run_lock_file(fd, name);
        if (!shared) trusted.push_back(target);
    }
    // The file of `fd` is busy (it is asked about only then), so a holder that does not show is
    // one /proc cannot tell.
    unseen = busy_file && std::none_of(held.begin(), held.end(), [&](const HeldFlock& f) {
                 const Match m = match(f, target);
                 return m == Match::exact || m == Match::mount;
             });
    bool unknown = unseen;
    // The processes that hold a flock, on any file (one may be an own file of the holder's
    // account); each is asked about strictly when it is one above this process.
    std::vector<long> holders;
    for (const HeldFlock& f : held) {
        if (f.pid > 0) {
            holders.push_back(f.pid);
            continue;
        }
        if (std::any_of(trusted.begin(), trusted.end(), [&](const LockFile& t) { return match(f, t) != Match::no; }))
            unknown = true;  // a line without a process to look into
    }
    // Whether process `pid` holds one of the trusted files or one of its account's own files, or
    // (root) a file of that name it has open that only its account may open, whether or not it
    // also holds the shared file (another account may hold that one). One budget for the whole
    // look (see ScanBudget), however many processes above this one are asked about.
    ScanBudget budget;
    const auto holds_drive = [&](long pid) {
        bool unsure = false;
        const long long account = account_of(pid);
        for (const LockFile& t : trusted)
            if (holds(pid, account, t, held, false, budget, unsure)) return true;
        for (const LockFile& own : own_files_of(account, name, mounts, false))
            if (holds(pid, account, own, held, false, budget, unsure)) return true;
        if (::geteuid() == 0) {
            for (const LockFile& f : private_files_of(pid, account, name, mounts, budget, unsure))
                if (!(shared && f.dev == target.dev && f.ino == target.ino) &&
                    holds(pid, account, f, held, false, budget, unsure))
                    return true;
        }
        if (unsure) unknown = true;
        return false;
    };
    if (!holders.empty()) {
        long pid = static_cast<long>(::getppid());
        bool whole = false;  // read up to the first process
        for (int depth = 0; depth < 64 && pid > 0; ++depth) {
            if (std::find(holders.begin(), holders.end(), pid) != holders.end() && holds_drive(pid))
                return Holder::parent;
            if (pid == 1) {
                whole = true;
                break;
            }
            pid = parent_of(pid);
        }
        if (!whole) unknown = true;
    }
    return unknown || loose ? Holder::unknown : Holder::other;
}

// Whether /proc/locks shows a flock held on the file of `fd`, by any process; also when that
// cannot be told (fail closed): /proc/locks cannot be read, reading it was cut short (within the
// time of a look, see ScanBudget, as any account can make it long), or a line may be about the
// file (Match::maybe).
bool flock_held(int fd) {
    LockFile id;
    Mounts mounts;
    if (!lock_file_of(fd, id, mounts)) return true;
    const ScanBudget budget;
    bool readable = false;
    const std::vector<HeldFlock> held = held_flocks(&readable, &budget);
    if (!readable || !budget.in_time()) return true;
    return std::any_of(held.begin(), held.end(), [&](const HeldFlock& f) { return match(f, id) != Match::no; });
}

// Whether /proc/locks shows the file of `fd` held only by processes of other accounts (none of
// whose four accounts, see accounts_of, is this one): the settings folder's file where other
// accounts may open it (see open_account_files), which a run or claim may then pass by. False
// where that cannot be told (fail safe: it then keeps waiting): no holder shows, /proc/locks
// cannot be read or reading it was cut short (within the time of a look, see ScanBudget, as any
// account can make it long), a line may be about the file but that cannot be told (Match::mount or
// Match::maybe), or a holder's accounts cannot be read.
bool other_account_holds(int fd) {
    LockFile id;
    Mounts mounts;
    if (!lock_file_of(fd, id, mounts)) return false;
    const ScanBudget budget;
    bool readable = false;
    const std::vector<HeldFlock> held = held_flocks(&readable, &budget);
    if (!readable || !budget.in_time()) return false;
    const auto me = static_cast<long long>(::geteuid());
    bool seen = false;
    for (const HeldFlock& f : held) {
        const Match m = match(f, id);
        if (m == Match::no) continue;
        if (m != Match::exact || f.pid <= 0) return false;
        const std::vector<long long> ids = accounts_of(f.pid);
        if (ids.empty() || std::find(ids.begin(), ids.end(), me) != ids.end()) return false;
        seen = true;
    }
    return seen;
}

// Whether a process of root's (see root_process) holds the file of `fd` alone (as root's eject
// holds the claims file); only root can make that so. /proc/locks is read within the time of a
// look (see ScanBudget), as any account can make it long: cut short, it may be so (fail closed),
// which costs a claim kRootLetGo once (see DriveClaim).
bool root_holds_alone(int fd) {
    LockFile id;
    Mounts mounts;
    if (!lock_file_of(fd, id, mounts)) return false;
    const ScanBudget budget;
    const std::vector<HeldFlock> held = held_flocks(nullptr, &budget);
    if (!budget.in_time()) return true;
    for (const HeldFlock& f : held)
        if (is_file(f, id) && f.write && root_process(f.pid)) return true;
    return false;
}

// How long a claim of an account still waits for the claims file after root's eject let go of its
// mark (see DriveClaim).
constexpr std::chrono::seconds kRootLetGo{2};

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
    const bool fail_closed = mode == DriveLockMode::eject;
#ifdef _WIN32
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
#else
    // Linux: a lock that is busy is asked who holds it. A run this command descends from (see
    // held_by_ancestor), whatever its rights and whatever environment reached this command: that
    // run holds the drive and waits for this command to end, which inherits the drive instead
    // (an eject throws DriveBusy, fail closed). Free, or held by anybody else, the lock is taken
    // or waited for as usual.
    const std::string name = "vaultik-drive-" + hash + ".lock";
    const std::string mark = "vaultik-drive-" + hash + ".root.lock";
    constexpr bool parent = false;
    // Where /proc gives no answer to use (see Holder::unknown), the marker the run passes on to
    // its commands tells instead (sudo and pkexec drop it, but they are rarely in between then),
    // never for an eject, which then waits as usual. With `taken` (an own lock): where its file
    // shows no holder at all, its flock is tried once more after a moment first, as root's eject
    // may have held it just then to see whether it is held (see flock_busy), which a command that
    // outlived its run would otherwise take for its run's; `taken` is set when that got the lock.
    const auto by_parent = [&](int fd, bool own_file, bool* taken = nullptr) {
        bool unseen = false;
        const Holder holder = held_by_ancestor(fd, own_file, name, mark, unseen);
        if (holder == Holder::unknown && unseen && taken != nullptr) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                *taken = true;
                return false;
            }
        }
        if (holder == Holder::other) return false;
        if (holder == Holder::unknown && (fail_closed || !named_by_parent(hash))) return false;
        if (!fail_closed) inherited_ = true;
        return true;
    };
    const auto parent_busy = [&] {
        if (fail_closed) throw DriveBusy("the run that started this command uses the backup drive");
    };
#endif
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
    // Another DriveLock of this process first (it may be this very thread). Linux: never the
    // run that started this command, which is another process.
    while (!take_in_process_lock(hash)) {
#ifdef _WIN32
        if (busy_now()) return;
#endif
        if (!waiting()) return;
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
        // run goes on without the lock. So does a name taken by another kind of object, behind
        // which runs go on without the lock too.
        if (busy_for_eject(opened)) {
            if (busy_now()) return;
            if (fail_closed) throw DriveBusy("a run this account cannot see into uses the backup drive");
            if (not_to_share(opened)) not_shared_ = true;
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
    const bool root = ::geteuid() == 0;
    int own = -1;
    int own2 = -1;  // an account's second and third own locks (see open_account_files)
    int own3 = -1;
    int shared = -1;
    AccountFiles files;
    if (root) {
        // Its own lock first, which only root may open (and the mark of a root run), then the one
        // all accounts share. A mark in /run/lock that root cannot use while a process holds it
        // (another run of root's may) fails an eject closed; a run goes on as without a mark.
        bool mark_held = false;
        own = open_root_mark(mark, mark_held);
        if (mark_held && fail_closed) throw DriveBusy("another Vaultik run uses the backup drive");
        shared = open_in_run_lock(name, 0644, own_or_roots);
        if (own < 0) {
            own = shared >= 0 ? shared : open_lock_file(name, lock_dir);
            shared = -1;
        }
    } else {
        // The account's own locks first, then the one all accounts share (see open_account_files).
        files = open_account_files(name, lock_dir);
        int* const slots[] = {&own, &own2, &own3};
        for (std::size_t i = 0; i < files.own.size() && i < 3; ++i) *slots[i] = files.own[i];
        shared = files.shared;
    }
    const auto close_all = [&] {
        for (const int fd : {own, own2, own3, shared})
            if (fd >= 0) ::close(fd);
        own = own2 = own3 = shared = -1;
    };
    if (own < 0 && shared < 0 && root) return;
    // The end of the short wait for a lock another account may hold (see other_account_wait),
    // from the first such wait on (the settings folder's file where other accounts may open it,
    // then the shared one), so that the two together delay a run no longer than one.
    const auto bound = other_account_wait();
    std::chrono::steady_clock::time_point other_deadline{};
    // Waits for `fd` (an own lock) and takes it; false when this returns without the lock (all
    // closed). The settings folder's file where its file system refuses flocks: let go of (-1).
    // That file where other accounts may open it (see open_account_files), another account may
    // hold for good: a run that already holds another own lock (the one in its runtime folder or
    // in /run/lock, which the account's ejects see) goes on without it (-1) once the short wait
    // has ended, and only while /proc/locks shows it held by another account's processes alone
    // (see other_account_holds). Held by a process of this account, or where that cannot be told,
    // and for a run with no other own lock (it would hold nothing an eject of the account sees),
    // it is waited for the whole wait, as any own lock. An eject counts it as busy.
    const auto take_own = [&](int& fd) {
        const int* const slots[] = {&own, &own2, &own3};
        const bool other_own = std::any_of(std::begin(slots), std::end(slots),
                                           [&](const int* o) { return o != &fd && *o >= 0; });
        const bool loose = fd == files.folder && files.loose && !fail_closed && other_own;
        for (;;) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
            if (folder_refused(fd, files)) {
                ::close(fd);
                fd = -1;
                return true;
            }
            if (errno != EWOULDBLOCK && errno != EINTR) {
                close_all();
                if (fail_closed) throw DriveBusy("the backup drive's lock cannot be checked");
                return false;
            }
            bool taken = false;
            const bool parent_holds = by_parent(fd, true, &taken);
            if (taken) return true;
            if (parent_holds) {
                close_all();
                parent_busy();
                return false;
            }
            if (loose) {
                const auto now = std::chrono::steady_clock::now();
                if (other_deadline == std::chrono::steady_clock::time_point{})
                    other_deadline = std::min(deadline, now + bound);
                if (now >= other_deadline && other_deadline < deadline && other_account_holds(fd)) {
                    ::close(fd);
                    fd = -1;
                    skipped_ = true;
                    return true;
                }
            }
            try {
                if (!waiting()) {
                    close_all();
                    return false;
                }
            } catch (...) {
                close_all();
                throw;
            }
            pause(std::chrono::milliseconds(250));
        }
    };
    for (int* fd : {&own, &own2, &own3})
        if (*fd >= 0 && !take_own(*fd)) return;
    // Where other accounts may write to the settings folder (see AccountFiles), they may rename its
    // file away, so that a run that holds it and an eject that makes it anew do not meet there: an
    // eject of the account then counts the shared file as busy (fail closed) instead of passing it
    // by, and a run that holds no other own file waits for the shared file its whole wait, so that
    // the account's ejects still see it there.
    const int held_own[] = {own, own2, own3};
    const bool folder_only =
        files.exposed && std::none_of(std::begin(held_own), std::end(held_own),
                                      [&](int o) { return o >= 0 && o != files.folder; });
    if (shared >= 0 || !root) {
        // Root: every root run took root's own lock first, so whoever holds this one now is another
        // account. An account: this account's runs took its own first, so whoever holds it is root
        // (while root's mark is held) or another account. Another account is waited for only a
        // short while (or for the whole wait, when that is shorter); an eject of an account passes
        // it by at once (not where other accounts may write to its settings folder, see
        // folder_only), while root's eject counts it as busy (fail closed, root's runs take turns
        // with every account's). An account also waits while root's mark is held with this one
        // free: a run of root's that passed by an account's run on it is still under way. So does
        // an account that cannot use the shared file at all (another account made it first): it
        // still waits for root's run, and its eject fails closed while root's mark is held.
        if (other_deadline == std::chrono::steady_clock::time_point{})
            other_deadline = std::min(deadline, std::chrono::steady_clock::now() + bound);
        for (;;) {
            // Asked once a turn: root's run may end between two looks, which would then disagree
            // (an eject of an account's would pass by a lock it just saw root's run hold).
            bool by_root = false;
            bool busy = true;  // the shared lock itself (else only root's mark is held)
            if (shared < 0) {  // an account without the shared file: root's mark alone
                by_root = root_holds(mark, false, fail_closed);
                if (!by_root) break;
                busy = false;
            } else if (::flock(shared, LOCK_EX | LOCK_NB) == 0) {
                if (root) break;
                by_root = root_holds(mark, false, fail_closed);
                if (!by_root) break;
                ::flock(shared, LOCK_UN);
                busy = false;
            } else if (errno != EWOULDBLOCK && errno != EINTR) {
                ::close(shared);
                shared = -1;
                if (root) break;
                continue;  // root's mark still counts
            } else {
                by_root = !root && root_holds(mark, false, fail_closed);
            }
            // Held by the run that started this command (or root's mark is): whatever its own
            // lock, the command inherits the drive (one with root rights that an account's run
            // started, or the other way round) instead of waiting for its own parent.
            if (by_parent(busy ? shared : -1, false)) {
                close_all();
                parent_busy();
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (by_root) other_deadline = std::min(deadline, now + bound);
            const bool waited_out = now >= other_deadline && other_deadline < deadline;
            const bool pass =
                !by_root && (fail_closed ? !root && !files.exposed : !folder_only && waited_out);
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
    // Root's eject: an account's run may hold its own locks without the shared one (another
    // account held that one when the run started, or made it first), so those count as busy too,
    // also where that cannot be told (fail closed). An account's eject: a run of its own may hold
    // its own file in /run/lock that this eject did not use (another account gave it a second
    // name), so that one counts as busy too (see own_held_elsewhere).
    if (fail_closed &&
        (root ? accounts_hold(name, false) != Use::free : own_held_elsewhere(name, {own, own2, own3, shared}))) {
        close_all();
        throw DriveBusy("another Vaultik run uses the backup drive");
    }
    if (own < 0) {
        if (shared < 0) return;  // passed by, and no lock of its own (the runs then overlap)
        own = shared;
        shared = -1;
    }
    handle_ = handle_of(own);
    if (own2 >= 0) own2_ = handle_of(own2);
    if (own3 >= 0) own3_ = handle_of(own3);
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
    for (void* h : {shared_, own3_, own2_}) {
        if (h == nullptr) continue;
        ::flock(fd_of(h), LOCK_UN);
        ::close(fd_of(h));
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
            // Not there (no eject), or the name taken by another kind of object: not waited for.
            // There but not to be shared (made by an eject this account may not open, or owned by
            // another account): waited for, at most until the claim's wait ends.
            if (!not_to_share(eject) || std::chrono::steady_clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        handle_ = held;
#else
        const std::string name = "vaultik-use-" + hash + ".lock";
        const std::string mark = "vaultik-use-" + hash + ".root.lock";
        int own = -1;
        int own2 = -1;
        int own3 = -1;
        int shared = -1;
        AccountFiles files;
        if (::geteuid() == 0) {
            // Root's mark first (an eject of root's holds it alone), then the claims file. A mark in
            // /run/lock that root cannot use while a process holds it: the claims file alone, as
            // without a mark (an eject of root's that holds that mark holds the claims file too).
            bool mark_held = false;
            own = open_root_mark(mark, mark_held);
            shared = open_lock_file(name, lock_dir);
            if (own < 0) std::swap(own, shared);
        } else {
            files = open_account_files(name, lock_dir);
            int* const slots[] = {&own, &own2, &own3};
            for (std::size_t i = 0; i < files.own.size() && i < 3; ++i) *slots[i] = files.own[i];
            shared = files.shared;
        }
        // Shared with every other claim; only an eject holds one alone, until the drive is gone.
        // False when it gave up on `fd` (closed, -1) other than as the settings folder's file that
        // its file system refuses flocks on (see folder_refused), or as one it may pass by
        // (`passable`) that /proc/locks shows held by other accounts alone when the wait ends (see
        // other_account_holds).
        const auto take = [&](int& fd, bool bounded, bool passable) {
            // Root's eject may let go of its mark a moment before the claims file (one that ends
            // without letting go, killed say, leaves both in the order it opened them, the mark
            // first): the file is waited for a little longer after the mark was last seen held, so
            // that the claim is not taken without it, unseen by root's next eject. A claim that
            // first looks in between sees root's eject hold the claims file alone (only root's own
            // processes count, so no other account can delay a command that way) and waits as
            // long from then.
            auto root_until = std::chrono::steady_clock::time_point::min();
            bool first = true;
            for (;;) {
                if (::flock(fd, LOCK_SH | LOCK_NB) == 0) return true;
                const bool refused = folder_refused(fd, files);
                const bool busy = errno == EWOULDBLOCK || errno == EINTR;
                const auto now = std::chrono::steady_clock::now();
                // An account (bounded) waits on the file of another account only while root's eject
                // holds its mark (and a moment after): another account may hold it alone for good.
                // root_holds reads /proc/locks whole on each 50 ms turn, so a long one (another
                // account can make it so) makes each turn that much longer, and the wait may end
                // past `until` and root_until by up to one read.
                if (bounded && root_holds(mark, true, false)) root_until = now + kRootLetGo;
                else if (bounded && busy && first && root_holds_alone(fd)) root_until = now + kRootLetGo;
                first = false;
                if (!busy || now >= until || (bounded && now >= root_until)) {
                    const bool passed = busy && passable && other_account_holds(fd);
                    ::close(fd);
                    fd = -1;
                    return refused || passed;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        // An eject kept one for the whole wait: no claim, except where that is the settings
        // folder's file that other accounts may open (see open_account_files), which another
        // account may hold for good: the claim then goes on with the others, but only where it
        // already holds another own file (the one in its runtime folder or in /run/lock, which the
        // account's ejects see) and only another account's processes hold that file (see take).
        for (int* fd : {&own, &own2, &own3}) {
            const int held[] = {own, own2, own3};
            const bool loose = *fd >= 0 && *fd == files.folder && files.loose &&
                               std::any_of(std::begin(held), std::end(held),
                                           [&](int o) { return o >= 0 && o != *fd; });
            if (*fd >= 0 && !take(*fd, false, loose)) {
                for (const int other : {own, own2, own3, shared})
                    if (other >= 0) ::close(other);
                return;
            }
        }
        if (shared >= 0) {
            take(shared, ::geteuid() != 0, false);
        } else if (::geteuid() != 0) {
            // No claims file to share (another account made it first): root's eject is still
            // waited for while it holds its mark, at most until the claim's wait ends.
            while (root_holds(mark, true, false) && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (::geteuid() == 0) {
            // Root: an eject of an account's may hold its own claims files alone without the shared
            // one (see EjectHold), and is waited for as well, at most until the claim's wait ends.
            while (accounts_hold(name, true) == Use::busy && std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (own < 0) std::swap(own, shared);
        if (own < 0) return;
        handle_ = handle_of(own);
        if (own2 >= 0) own2_ = handle_of(own2);
        if (own3 >= 0) own3_ = handle_of(own3);
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
    for (void* h : {shared_, own3_, own2_}) {
        if (h == nullptr) continue;
        ::flock(fd_of(h), LOCK_UN);
        ::close(fd_of(h));
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
    } else if (busy_for_eject(eject)) {
        busy_ = true;  // one this account cannot see into: as if another eject were under way
    }
    for (int slot = 0; slot < kClaimSlots && !busy_; ++slot) {
        const Mutex slot_mutex = open_mutex(slot_name(hash, slot), false);
        const HANDLE m = slot_mutex.handle;
        if (m == nullptr) {
            // Never made: free. There but not to be shared with this account (another account made
            // it, or this one may not open it), or the name taken by another kind of object: in use
            // (fail closed).
            if (busy_for_eject(slot_mutex)) busy_ = true;
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
    int own2 = -1;
    int own3 = -1;
    int shared = -1;
    AccountFiles files;
    if (root) {
        // Root's mark first (root's claims hold it), then the claims file of every account. A mark
        // in /run/lock that root cannot use while a process holds it (a claim of root's may): busy.
        bool mark_held = false;
        own = open_root_mark(mark, mark_held);
        if (mark_held) {
            busy_ = true;
            return;
        }
        shared = open_lock_file(name, lock_dir);
        if (own < 0) std::swap(own, shared);
    } else {
        files = open_account_files(name, lock_dir);
        int* const slots[] = {&own, &own2, &own3};
        for (std::size_t i = 0; i < files.own.size() && i < 3; ++i) *slots[i] = files.own[i];
        shared = files.shared;
    }
    const auto close_all = [&] {
        for (const int fd : {own, own2, own3, shared})
            if (fd >= 0) ::close(fd);
        own = own2 = own3 = shared = -1;
    };
    // Alone on them: no claim is held, and none can be taken until this lets go. The settings
    // folder's file where its file system refuses flocks is let go of (see folder_refused).
    for (int* fd : {&own, &own2, &own3}) {
        if (*fd >= 0 && ::flock(*fd, LOCK_EX | LOCK_NB) != 0) {
            if (folder_refused(*fd, files)) {
                ::close(*fd);
                *fd = -1;
                continue;
            }
            busy_ = true;  // held by a claim or eject, or it cannot be told (fail closed)
            close_all();
            return;
        }
    }
    if (shared >= 0 && ::flock(shared, LOCK_EX | LOCK_NB) != 0) {
        // Root: a claim of any account (fail closed). An account: a claim or eject of root's while
        // root's mark is held, else one of another account, passed by (the account's own files are
        // still looked at below, also when this eject holds none); but where other accounts may
        // write to the settings folder (see AccountFiles), busy too (fail closed: a claim of the
        // account may hold it and a settings folder's file that another account renamed away).
        const bool busy =
            errno == EWOULDBLOCK && (root || files.exposed || root_holds(mark, false, true));
        ::close(shared);
        shared = -1;
        if (busy) {
            busy_ = true;
            close_all();
            return;
        }
    }
    // An account: a claim or eject of root's while root's mark is held, whatever the shared file's
    // state (also when it cannot use that file at all, another account having made it first), and
    // a claim or eject of its own on its own file in /run/lock that this eject did not use (see
    // own_held_elsewhere). Root: a claim or eject of any account on its own files, which it may
    // hold without the shared one (another account held that one when it started, or made it
    // first), also where that cannot be told (fail closed). Asked once root holds its own: a claim
    // that starts later waits for this eject.
    if (root ? accounts_hold(name, false) != Use::free
             : root_holds(mark, false, true) || own_held_elsewhere(name, {own, own2, own3, shared})) {
        busy_ = true;
        close_all();
        return;
    }
    if (own < 0) std::swap(own, shared);
    if (own < 0) return;
    handle_ = handle_of(own);
    if (own2 >= 0) own2_ = handle_of(own2);
    if (own3 >= 0) own3_ = handle_of(own3);
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
    for (void* h : {shared_, own3_, own2_}) {
        if (h == nullptr) continue;
        ::flock(fd_of(h), LOCK_UN);
        ::close(fd_of(h));
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
