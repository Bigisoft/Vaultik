#include "vaultik/jobs/drive_lock.hpp"

#include <algorithm>
#include <cstdint>
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

bool mine(const struct stat& st) {
    return st.st_uid == ::geteuid();
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

// The account's own locks for `name` ("vaultik-drive-<hash>.lock"), which only runs of the
// account can hold, so that every run of the account takes turns there, whatever settings folder
// it uses (the installed app and a portable one, VAULTIK_CONFIG_DIR): the file `name` in its own
// runtime folder (see run_user_dir), where no other account can make one first, and the file
// "vaultik-drive-<hash>.<uid>.lock" in /run/lock, readable by the account alone and used only when
// it is a file the account owns (another account may have made one of that name first). Both
// where both can be used, in that order: a run without the runtime folder (one started outside a
// login, without systemd's user manager) still takes turns with the others in /run/lock. Where
// the file in /run/lock cannot be used (a distribution whose /run/lock only root may write to), the
// file in `lock_dir` too, after the one in the runtime folder: a run without the runtime folder
// (or with another one, as a Flatpak's) then takes turns with the others through the settings
// folder, as long as they use the same one. Every run takes them in this order, so none waits for
// another that waits for it. Empty when none can be opened.
std::vector<int> open_own_files(const std::string& name, const std::string& lock_dir) {
    std::vector<int> fds;
    if (!hooks().own_in_folder) {
        if (const int fd = open_in_run_user(name); fd >= 0) fds.push_back(fd);
        const std::string stem = name.substr(0, name.size() - std::string(".lock").size());
        if (const int fd = open_in_run_lock(stem + "." + std::to_string(::geteuid()) + ".lock", 0600, mine); fd >= 0) {
            fds.push_back(fd);
            return fds;
        }
    }
    if (const int fd = open_in_dir(name, lock_dir); fd >= 0) fds.push_back(fd);
    return fds;
}

// What a run of an account (not root) locks for `name`: `own` (see open_own_files), which only
// runs of this account can hold, then `shared`, the file all accounts share (when it can be used),
// which root's runs and those of other accounts may hold too and which is waited for only a short
// while. Either may be missing (-1, empty).
struct AccountFiles {
    std::vector<int> own;
    int shared = -1;
};

AccountFiles open_account_files(const std::string& name, const std::string& lock_dir) {
    AccountFiles f;
    f.own = open_own_files(name, lock_dir);
    f.shared = open_in_run_lock(name, 0600, own_or_roots);
    return f;
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
// from its mark). Where that folder cannot be used, in /run/lock as before. -1 when neither can.
int open_root_mark(const std::string& name) {
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
    return open_in_run_lock(name, 0600, roots);
}

// A flock held on a file, as /proc/locks shows it (every account may read it).
struct HeldFlock {
    unsigned dev_major = 0;
    unsigned dev_minor = 0;
    unsigned long long inode = 0;
    bool write = false;
    long pid = 0;  // the process that took it
};

// The flocks held now (not the ones waited for). Empty when /proc/locks cannot be read (then
// `readable`, when given, is false).
std::vector<HeldFlock> held_flocks(bool* readable = nullptr) {
    std::vector<HeldFlock> out;
    const testing::DriveLockHooks set = hooks();
    std::ifstream in(set.proc_locks.empty() ? std::string("/proc/locks") : set.proc_locks);
    if (readable != nullptr) *readable = in.is_open();
    std::string line;
    while (std::getline(in, line)) {
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
    bool mount_known = false;  // the device of its mount could be told
    unsigned mount_major = 0;
    unsigned mount_minor = 0;
};

// The device of the mount `mount_id`, from /proc/self/mountinfo ("36 35 98:0 / /mnt ..."). False
// when it cannot be told.
bool mount_device(std::uint64_t mount_id, unsigned& major_of, unsigned& minor_of) {
    std::ifstream in("/proc/self/mountinfo");
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream words(line);
        std::uint64_t id = 0;
        std::uint64_t parent = 0;
        std::string device;
        if (!(words >> id >> parent >> device) || id != mount_id) continue;
        return std::sscanf(device.c_str(), "%u:%u", &major_of, &minor_of) == 2;
    }
    return false;
}

// Whether /proc/locks may give another device than stat for the file `path` relative to `dirfd`
// (see lock_file_at): on btrfs (each subvolume has a device of its own) and overlayfs (stat gives
// the overlay's device, /proc/locks the one of the file system below), or when that cannot be told.
bool device_may_differ(int dirfd, const char* path, int flags) {
    constexpr long kBtrfs = 0x9123683e;
    constexpr long kOverlay = 0x794c7630;
    if (hooks().btrfs) return true;
    struct statfs fs{};
    const int r = (flags & AT_EMPTY_PATH) != 0 && path[0] == '\0' ? ::fstatfs(dirfd, &fs)
                  : dirfd == AT_FDCWD                              ? ::statfs(path, &fs)
                                                                   : -1;
    if (r != 0) return true;
    const long type = static_cast<long>(fs.f_type);
    return type == kBtrfs || type == kOverlay;
}

// The file `path` relative to `dirfd` (fstatat's `flags`), with the device of its mount where
// statx and /proc/self/mountinfo tell it (Linux 5.8 and later). Where they cannot (an older
// kernel, or a seccomp filter that refuses statx, as older Docker versions do), the device stat
// gives is the one /proc/locks gives, except on file systems where the two may differ (see
// device_may_differ). False when it cannot be looked at.
bool lock_file_at(int dirfd, const char* path, int flags, LockFile& f) {
    f = LockFile{};
    const testing::DriveLockHooks set = hooks();
    struct statx sx{};
    const int r = set.no_statx ? (errno = EPERM, -1) : ::statx(dirfd, path, flags, STATX_BASIC_STATS | STATX_MNT_ID, &sx);
    if (r != 0) {
        if (errno != ENOSYS && errno != EPERM) return false;
        struct stat st{};
        if (::fstatat(dirfd, path, &st, flags) != 0) return false;
        f.dev = st.st_dev;
        f.ino = st.st_ino;
        f.uid = st.st_uid;
        f.mode = st.st_mode;
    } else {
        f.dev = makedev(sx.stx_dev_major, sx.stx_dev_minor);
        f.ino = static_cast<ino_t>(sx.stx_ino);
        f.uid = sx.stx_uid;
        f.mode = sx.stx_mode;
        if ((sx.stx_mask & STATX_MNT_ID) != 0 && !set.no_mount_device)
            f.mount_known = mount_device(sx.stx_mnt_id, f.mount_major, f.mount_minor);
        if (f.mount_known && set.locks_other_device) {  // as on btrfs, where it is /proc/locks's
            f.mount_major ^= 0x800;
            f.mount_minor ^= 0x800;
        }
    }
    if (!f.mount_known && !device_may_differ(dirfd, path, flags)) {
        f.mount_known = true;
        f.mount_major = major(f.dev);
        f.mount_minor = minor(f.dev);
    }
    return true;
}

// The file of `fd` (see lock_file_at).
bool lock_file_of(int fd, LockFile& f) {
    return lock_file_at(fd, "", AT_EMPTY_PATH, f);
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

// The files process `pid` has open, from /proc/<pid>/fd (readable for the processes of this
// account, and for root). False when they cannot be read.
bool open_files_of(long pid, std::vector<LockFile>& files) {
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return false;
    while (const dirent* e = ::readdir(d)) {
        const std::string entry = dir + "/" + e->d_name;
        if (e->d_name[0] == '.') continue;
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
// readlink for each.
std::vector<LockFile> private_files_of(long pid, long long account, const std::string& name) {
    std::vector<LockFile> files;
    const std::string dir = "/proc/" + std::to_string(pid) + "/fd";
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return files;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        const std::string entry = dir + "/" + e->d_name;
        char target[4096];
        const ssize_t n = ::readlink(entry.c_str(), target, sizeof target - 1);
        const std::string path(target, n > 0 ? static_cast<std::size_t>(n) : 0);
        if (path.size() <= name.size() || path.compare(path.size() - name.size(), name.size(), name) != 0 ||
            path[path.size() - name.size() - 1] != '/')
            continue;
        LockFile f;
        if (lock_file_at(AT_FDCWD, entry.c_str(), 0, f) && S_ISREG(f.mode) && static_cast<long long>(f.uid) == account &&
            (f.mode & 077) == 0)
            files.push_back(f);
    }
    ::closedir(d);
    return files;
}

// Whether process `pid` holds a flock (`write_only`: holds it alone) on `file`, as /proc/locks
// (`held`) names it: on the device and inode stat gives, or, where /proc/locks gives the file
// system's device (btrfs), on that one when the process has this very file open (the inode alone
// may be another subvolume's file). `unsure` is set where it may but that cannot be told (the
// device of the file's mount cannot be told, or the open files of `pid` cannot be read).
bool holds(long pid, const LockFile& file, const std::vector<HeldFlock>& held, bool write_only, bool& unsure) {
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
    if (!open_files_of(pid, open)) {
        unsure = true;
        return false;
    }
    return std::any_of(open.begin(), open.end(),
                       [&](const LockFile& o) { return o.dev == file.dev && o.ino == file.ino; });
}

// Root's mark files `name`, in root's folder or, as earlier versions kept it, in /run/lock: only
// the ones root owns, which only root may open, so only root can hold them.
std::vector<LockFile> root_mark_files(const std::string& name) {
    std::vector<LockFile> files;
    for (const std::string& dir : {root_dir(), std::string("/run/lock")}) {
        LockFile f;
        if (lock_file_at(AT_FDCWD, (dir + "/" + name).c_str(), AT_SYMLINK_NOFOLLOW, f) && S_ISREG(f.mode) &&
            f.uid == hooks().root_uid)
            files.push_back(f);
    }
    return files;
}

// Whether root holds its mark `name` (`write_only`: holds it alone, as an eject does); every
// account can see in /proc/locks that it is held. False when that cannot be told.
bool root_holds(const std::string& name, bool write_only) {
    const std::vector<LockFile> files = root_mark_files(name);
    if (files.empty()) return false;
    for (const HeldFlock& f : held_flocks())
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
// when it is no folder that account owns alone.
std::string run_user_dir_of(long long uid) {
    if (uid == static_cast<long long>(::geteuid())) return run_user_dir();
    const std::string dir = run_user_root() + "/" + std::to_string(uid);
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || static_cast<long long>(st.st_uid) != uid ||
        (st.st_mode & 07777) != 0700)
        return {};
    return dir;
}

// The own lock files of account `uid` for `name` (see open_own_files) that this process can see:
// the one in its runtime folder and its file in /run/lock, each only when that account owns it,
// so that no process of another account can hold it.
std::vector<LockFile> own_files_of(long long uid, const std::string& name) {
    std::vector<LockFile> files;
    if (uid < 0) return files;
    const std::string stem = name.substr(0, name.size() - std::string(".lock").size());
    std::vector<std::string> paths = {"/run/lock/" + stem + "." + std::to_string(uid) + ".lock"};
    if (const std::string dir = run_user_dir_of(uid); !dir.empty()) paths.push_back(dir + "/" + name);
    for (const std::string& path : paths) {
        LockFile f;
        if (lock_file_at(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, f) && S_ISREG(f.mode) &&
            static_cast<long long>(f.uid) == uid)
            files.push_back(f);
    }
    return files;
}

// Root only: the own lock files for `name` of every other account (see own_files_of) that are
// there: each vaultik-...<uid>.lock in /run/lock and `name` in each account's runtime folder.
std::vector<LockFile> accounts_own_files(const std::string& name) {
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
    if (DIR* d = ::opendir("/run/lock")) {
        while (const dirent* e = ::readdir(d)) {
            const std::string entry = e->d_name;
            if (entry.size() > prefix.size() + suffix.size() && entry.compare(0, prefix.size(), prefix) == 0 &&
                entry.compare(entry.size() - suffix.size(), suffix.size(), suffix) == 0)
                add(entry.substr(prefix.size(), entry.size() - prefix.size() - suffix.size()));
        }
        ::closedir(d);
    }
    if (DIR* d = ::opendir(run_user_root().c_str())) {
        while (const dirent* e = ::readdir(d)) add(e->d_name);
        ::closedir(d);
    }
    std::sort(uids.begin(), uids.end());
    uids.erase(std::unique(uids.begin(), uids.end()), uids.end());
    std::vector<LockFile> files;
    for (const long long uid : uids) {
        const std::vector<LockFile> own = own_files_of(uid, name);
        files.insert(files.end(), own.begin(), own.end());
    }
    return files;
}

// Whether another account's run, claim or eject may hold one of its own files for `name` (root
// only; see accounts_own_files), which it may do without the file all accounts share (another
// account held that one when it started, or made it first): busy when /proc/locks shows one held
// (`write_only`: held alone, as by a run or an eject), unsure when that cannot be told
// (/proc/locks cannot be read, or the device of a file's mount cannot).
//
// Another account may also make an account's file in /run/lock first, so that a run of that
// account without its runtime folder (started by cron, without a login) holds only the file in its
// settings folder, which root cannot find by name: so every process that /proc/locks shows holding
// a flock is looked into as well (/proc/<pid>/fd), and one holding a file `name` there that its
// account owns and no other account may open (see private_files_of) counts as busy too. Never this
// process, whose own locks and claims are counted apart.
enum class Use { free, busy, unsure };

Use accounts_hold(const std::string& name, bool write_only) {
    const std::vector<LockFile> files = accounts_own_files(name);
    bool readable = false;
    const std::vector<HeldFlock> held = held_flocks(&readable);
    if (!readable) return files.empty() ? Use::free : Use::unsure;
    bool unsure = false;
    std::vector<long> pids;
    for (const HeldFlock& f : held) {
        if (write_only && !f.write) continue;
        for (const LockFile& file : files) {
            const Match m = match(f, file);
            if (m == Match::exact || m == Match::mount) return Use::busy;
            if (m == Match::maybe) unsure = true;
        }
        if (f.pid > 0 && f.pid != static_cast<long>(::getpid())) pids.push_back(f.pid);
    }
    std::sort(pids.begin(), pids.end());
    pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
    for (const long pid : pids) {
        bool maybe = false;
        for (const LockFile& f : private_files_of(pid, account_of(pid), name))
            if (holds(pid, f, held, write_only, maybe)) return Use::busy;
        if (maybe) unsure = true;
    }
    return unsure ? Use::unsure : Use::free;
}

// Whether the file of `fd` is the file `name` in /run/lock that every account shares.
bool is_shared_file(int fd, const std::string& name) {
    struct stat mine{}, there{};
    return ::fstat(fd, &mine) == 0 && ::lstat(("/run/lock/" + name).c_str(), &there) == 0 && S_ISREG(there.st_mode) &&
           mine.st_dev == there.st_dev && mine.st_ino == there.st_ino;
}

// What /proc tells about who holds a lock (see held_by_ancestor).
enum class Holder {
    parent,   // a process this one descends from
    other,    // only processes that are not
    unknown,  // no answer to use: the file shows no holder at all, or the processes above this one
              // cannot all be read (another pid namespace, /proc mounted with hidepid), or one of
              // them may hold a lock that /proc/locks cannot tell from another file's
};

// Whether a process this one descends from (its parent, or one further up: a run's before or
// after command reaches vaultik through a shell, sudo or pkexec, which keep no environment of
// it) holds a flock on the file of `fd` (none when -1), on root's mark `mark` or on one of its own
// account's own files (see own_files_of): then the run that started this command holds the drive,
// and waits for the command to end. Never this process itself (another DriveLock of it on the
// drive counts as busy), and never a process that is no ancestor (a command that outlived its
// run, or another account's run).
//
// /proc/locks names the process that took a flock, also after it ended while another process
// still holds the file open: so an account's own file counts only for a process whose account is
// that one now (another account may have taken a lock on its own file in a process whose number
// then goes to a process the victim's commands descend from, its login shell or sudo, say), and
// any account may hold the file `name` all accounts share (`fd` may be that one): a holder of
// that file never counts on its own: a process counts when it holds a lock no other account can
// take, one of its account's own files, root's mark or, for root (a command run with sudo; only
// root may look at the open files of another account's process), a file of that name it has open
// (/proc/<pid>/fd) that its account owns and no other account may open: the settings folder's, the
// only own file of a run without its runtime folder where only root may write to /run/lock or
// where another account made its file there first, also when another account holds the shared
// file. Otherwise another account could take the shared file from a process whose number then
// goes to a process the victim's commands descend from: they would all take it for their parent
// run and drop their turns. Holders of root's mark (only root may open it) count as they are. A
// holder of the shared file that cannot be trusted so is an answer (other), never unknown.
//
// Each lock counts only on the file's own device and inode (see Match): a line of /proc/locks with
// the same inode on another device may be any file of any account (tmpfs inode numbers are easy to
// match), so it never makes a parent; where the file's device cannot be told, such a line held by
// a process above this one leaves no answer (unknown).
Holder held_by_ancestor(int fd, const std::string& name, const std::string& mark) {
    const std::vector<HeldFlock> held = held_flocks();
    std::vector<LockFile> trusted = root_mark_files(mark);
    bool shared = false;
    LockFile target;
    const bool busy_file = fd >= 0 && lock_file_of(fd, target);
    if (busy_file) {
        shared = is_shared_file(fd, name);
        if (!shared) trusted.push_back(target);
    }
    // The file of `fd` is busy (it is asked about only then), so a holder that does not show is
    // one /proc cannot tell.
    bool unknown = busy_file && std::none_of(held.begin(), held.end(), [&](const HeldFlock& f) {
                       const Match m = match(f, target);
                       return m == Match::exact || m == Match::mount;
                   });
    // The processes that hold a flock, on any file (one may be an own file of the holder's
    // account); each is asked about strictly when it is one above this process.
    std::vector<long> holders;
    for (const HeldFlock& f : held) {
        if (f.pid > 0) {
            holders.push_back(f.pid);
            continue;
        }
        if (std::any_of(trusted.begin(), trusted.end(), [&](const LockFile& t) { return match(f, t) != Match::no; }))
            unknown = true;  // in another pid namespace
    }
    // Whether process `pid` holds one of the trusted files or one of its account's own files, or
    // (root) a file of that name it has open that only its account may open, whether or not it
    // also holds the shared file (another account may hold that one).
    const auto holds_drive = [&](long pid) {
        bool unsure = false;
        for (const LockFile& t : trusted)
            if (holds(pid, t, held, false, unsure)) return true;
        const long long account = account_of(pid);
        for (const LockFile& own : own_files_of(account, name))
            if (holds(pid, own, held, false, unsure)) return true;
        if (::geteuid() == 0) {
            for (const LockFile& f : private_files_of(pid, account, name))
                if (!(shared && f.dev == target.dev && f.ino == target.ino) && holds(pid, f, held, false, unsure))
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
    return unknown ? Holder::unknown : Holder::other;
}

// Whether a process of root's (see root_process) holds the file of `fd` alone (as root's eject
// holds the claims file); only root can make that so.
bool root_holds_alone(int fd) {
    LockFile id;
    if (!lock_file_of(fd, id)) return false;
    for (const HeldFlock& f : held_flocks())
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
    // never for an eject, which then waits as usual.
    const auto by_parent = [&](int fd) {
        const Holder holder = held_by_ancestor(fd, name, mark);
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
    int own2 = -1;  // an account's second own lock (see open_own_files)
    int shared = -1;
    if (root) {
        // Its own lock first, which only root may open (and the mark of a root run), then the one
        // all accounts share.
        own = open_root_mark(mark);
        shared = open_in_run_lock(name, 0644, own_or_roots);
        if (own < 0) {
            own = shared >= 0 ? shared : open_lock_file(name, lock_dir);
            shared = -1;
        }
    } else {
        // The account's own locks first, then the one all accounts share (see open_account_files).
        const AccountFiles files = open_account_files(name, lock_dir);
        if (!files.own.empty()) own = files.own[0];
        if (files.own.size() > 1) own2 = files.own[1];
        shared = files.shared;
    }
    const auto close_all = [&] {
        for (const int fd : {own, own2, shared})
            if (fd >= 0) ::close(fd);
        own = own2 = shared = -1;
    };
    if (own < 0 && shared < 0 && root) return;
    // Waits for `fd` (an own lock) and takes it; false when this returns without the lock (all
    // closed).
    const auto take_own = [&](int fd) {
        for (;;) {
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) return true;
            if (errno != EWOULDBLOCK && errno != EINTR) {
                close_all();
                return false;
            }
            if (by_parent(fd)) {
                close_all();
                parent_busy();
                return false;
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
    for (const int fd : {own, own2})
        if (fd >= 0 && !take_own(fd)) return;
    if (shared >= 0 || !root) {
        // Root: every root run took root's own lock first, so whoever holds this one now is another
        // account. An account: this account's runs took its own first, so whoever holds it is root
        // (while root's mark is held) or another account. Another account is waited for only a
        // short while (or for the whole wait, when that is shorter); an eject of an account passes
        // it by at once, while root's eject counts it as busy (fail closed, root's runs take turns
        // with every account's). An account also waits while root's mark is held with this one
        // free: a run of root's that passed by an account's run on it is still under way. So does
        // an account that cannot use the shared file at all (another account made it first): it
        // still waits for root's run, and its eject fails closed while root's mark is held.
        const auto bound = other_account_wait();
        auto other_deadline = std::min(deadline, std::chrono::steady_clock::now() + bound);
        for (;;) {
            // Asked once a turn: root's run may end between two looks, which would then disagree
            // (an eject of an account's would pass by a lock it just saw root's run hold).
            bool by_root = false;
            bool busy = true;  // the shared lock itself (else only root's mark is held)
            if (shared < 0) {  // an account without the shared file: root's mark alone
                by_root = root_holds(mark, false);
                if (!by_root) break;
                busy = false;
            } else if (::flock(shared, LOCK_EX | LOCK_NB) == 0) {
                if (root) break;
                by_root = root_holds(mark, false);
                if (!by_root) break;
                ::flock(shared, LOCK_UN);
                busy = false;
            } else if (errno != EWOULDBLOCK && errno != EINTR) {
                ::close(shared);
                shared = -1;
                if (root) break;
                continue;  // root's mark still counts
            } else {
                by_root = !root && root_holds(mark, false);
            }
            // Held by the run that started this command (or root's mark is): whatever its own
            // lock, the command inherits the drive (one with root rights that an account's run
            // started, or the other way round) instead of waiting for its own parent.
            if (by_parent(busy ? shared : -1)) {
                close_all();
                parent_busy();
                return;
            }
            const auto now = std::chrono::steady_clock::now();
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
    // Root's eject: an account's run may hold its own locks without the shared one (another
    // account held that one when the run started, or made it first), so those count as busy too,
    // also where that cannot be told (fail closed).
    if (root && fail_closed && accounts_hold(name, false) != Use::free) {
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
    for (void* h : {shared_, own2_}) {
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
        int shared = -1;
        if (::geteuid() == 0) {
            // Root's mark first (an eject of root's holds it alone), then the claims file.
            own = open_root_mark(mark);
            shared = open_lock_file(name, lock_dir);
            if (own < 0) std::swap(own, shared);
        } else {
            const AccountFiles files = open_account_files(name, lock_dir);
            if (!files.own.empty()) own = files.own[0];
            if (files.own.size() > 1) own2 = files.own[1];
            shared = files.shared;
        }
        // Shared with every other claim; only an eject holds one alone, until the drive is gone.
        const auto take = [&](int& fd, bool bounded) {
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
                if (::flock(fd, LOCK_SH | LOCK_NB) == 0) return;
                const bool busy = errno == EWOULDBLOCK || errno == EINTR;
                const auto now = std::chrono::steady_clock::now();
                // An account (bounded) waits on the file of another account only while root's eject
                // holds its mark (and a moment after): another account may hold it alone for good.
                if (bounded && root_holds(mark, true)) root_until = now + kRootLetGo;
                else if (bounded && busy && first && root_holds_alone(fd)) root_until = now + kRootLetGo;
                first = false;
                if (!busy || now >= until || (bounded && now >= root_until)) {
                    ::close(fd);
                    fd = -1;
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        };
        for (int* fd : {&own, &own2}) {
            if (*fd < 0) continue;
            take(*fd, false);
            if (*fd < 0) {  // an eject kept it for the whole wait
                for (const int other : {own, own2, shared})
                    if (other >= 0) ::close(other);
                return;
            }
        }
        if (shared >= 0) {
            take(shared, ::geteuid() != 0);
        } else if (::geteuid() != 0) {
            // No claims file to share (another account made it first): root's eject is still
            // waited for while it holds its mark, at most until the claim's wait ends.
            while (root_holds(mark, true) && std::chrono::steady_clock::now() < until)
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
    for (void* h : {shared_, own2_}) {
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
    int shared = -1;
    if (root) {
        // Root's mark first (root's claims hold it), then the claims file of every account.
        own = open_root_mark(mark);
        shared = open_lock_file(name, lock_dir);
        if (own < 0) std::swap(own, shared);
    } else {
        const AccountFiles files = open_account_files(name, lock_dir);
        if (!files.own.empty()) own = files.own[0];
        if (files.own.size() > 1) own2 = files.own[1];
        shared = files.shared;
    }
    const auto close_all = [&] {
        for (const int fd : {own, own2, shared})
            if (fd >= 0) ::close(fd);
        own = own2 = shared = -1;
    };
    // Alone on them: no claim is held, and none can be taken until this lets go.
    for (const int fd : {own, own2}) {
        if (fd >= 0 && ::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            busy_ = errno == EWOULDBLOCK;
            close_all();
            return;
        }
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
    // An account: a claim or eject of root's while root's mark is held, whatever the shared file's
    // state (also when it cannot use that file at all, another account having made it first).
    // Root: a claim or eject of any account on its own files, which it may hold without the shared
    // one (another account held that one when it started, or made it first), also where that
    // cannot be told (fail closed). Asked once root holds its own: a claim that starts later waits
    // for this eject.
    if (root ? accounts_hold(name, false) != Use::free : root_holds(mark, false)) {
        busy_ = true;
        close_all();
        return;
    }
    if (own < 0) std::swap(own, shared);
    if (own < 0) return;
    handle_ = handle_of(own);
    if (own2 >= 0) own2_ = handle_of(own2);
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
    for (void* h : {shared_, own2_}) {
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
