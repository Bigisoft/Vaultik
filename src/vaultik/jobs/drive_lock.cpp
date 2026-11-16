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

// The account's own runtime folder (/run/user/<uid>): a folder (not a link) that the account owns
// and nobody else may look into, so that no other account can make a file in it first. Empty when
// there is none such.
std::string run_user_dir() {
    const std::string set = hooks().run_user_dir;
    const std::string dir = set.empty() ? "/run/user/" + std::to_string(::geteuid()) : set;
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
// neither can be used, the file in `lock_dir` instead. Empty when none can be opened.
std::vector<int> open_own_files(const std::string& name, const std::string& lock_dir) {
    std::vector<int> fds;
    if (!hooks().own_in_folder) {
        if (const int fd = open_in_run_user(name); fd >= 0) fds.push_back(fd);
        const std::string stem = name.substr(0, name.size() - std::string(".lock").size());
        if (const int fd = open_in_run_lock(stem + "." + std::to_string(::geteuid()) + ".lock", 0600, mine); fd >= 0)
            fds.push_back(fd);
        if (!fds.empty()) return fds;
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

// The flocks held now (not the ones waited for). Empty when /proc/locks cannot be read.
std::vector<HeldFlock> held_flocks() {
    std::vector<HeldFlock> out;
    const testing::DriveLockHooks set = hooks();
    std::ifstream in(set.proc_locks.empty() ? std::string("/proc/locks") : set.proc_locks);
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

using FileId = std::pair<dev_t, ino_t>;

bool is_file(const HeldFlock& f, const FileId& id) {
    return f.dev_major == major(id.first) && f.dev_minor == minor(id.first) && f.inode == id.second;
}

// Whether the line `f` of `held` (all of /proc/locks) is about the file `id`. On btrfs stat gives a
// subvolume's device while /proc/locks gives the file system's: when no line carries the file's
// device at all, the inode alone tells.
bool is_file(const std::vector<HeldFlock>& held, const HeldFlock& f, const FileId& id) {
    if (f.inode != id.second) return false;
    if (is_file(f, id)) return true;
    return std::none_of(held.begin(), held.end(), [&](const HeldFlock& g) {
        return g.dev_major == major(id.first) && g.dev_minor == minor(id.first);
    });
}

// Root's mark files `name`, in root's folder or, as earlier versions kept it, in /run/lock: only
// the ones root owns, which only root may open, so only root can hold them.
std::vector<FileId> root_mark_files(const std::string& name) {
    std::vector<FileId> files;
    for (const std::string& dir : {root_dir(), std::string("/run/lock")}) {
        struct stat st{};
        if (::lstat((dir + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == hooks().root_uid)
            files.emplace_back(st.st_dev, st.st_ino);
    }
    return files;
}

// Whether root holds its mark `name` (`write_only`: holds it alone, as an eject does); every
// account can see in /proc/locks that it is held. False when that cannot be told.
bool root_holds(const std::string& name, bool write_only) {
    const std::vector<FileId> files = root_mark_files(name);
    if (files.empty()) return false;
    for (const HeldFlock& f : held_flocks())
        for (const FileId& id : files)
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

// The effective account of process `pid`, from /proc/<pid>/status (readable by every account, and
// true also for a process that made itself not dumpable, which /proc/<pid> itself shows as
// root's). -1 when it cannot be told.
long long account_of(long pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Uid:", 0) != 0) continue;
        std::istringstream ids(line.substr(4));
        long long real = -1, effective = -1;
        if (ids >> real >> effective) return effective;
        return -1;
    }
    return -1;
}

// The runtime folder of account `uid` (see run_user_dir) as far as this process can tell: empty
// when it is no folder that account owns alone.
std::string run_user_dir_of(long long uid) {
    if (uid == static_cast<long long>(::geteuid())) return run_user_dir();
    const std::string dir = "/run/user/" + std::to_string(uid);
    struct stat st{};
    if (::lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || static_cast<long long>(st.st_uid) != uid ||
        (st.st_mode & 07777) != 0700)
        return {};
    return dir;
}

// The own lock files of account `uid` for `name` (see open_own_files) that this process can see:
// the one in its runtime folder and its file in /run/lock, each only when that account owns it,
// so that no process of another account can hold it.
std::vector<FileId> own_files_of(long long uid, const std::string& name) {
    std::vector<FileId> files;
    if (uid < 0) return files;
    const std::string stem = name.substr(0, name.size() - std::string(".lock").size());
    std::vector<std::string> paths = {"/run/lock/" + stem + "." + std::to_string(uid) + ".lock"};
    if (const std::string dir = run_user_dir_of(uid); !dir.empty()) paths.push_back(dir + "/" + name);
    for (const std::string& path : paths) {
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && static_cast<long long>(st.st_uid) == uid)
            files.emplace_back(st.st_dev, st.st_ino);
    }
    return files;
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
              // cannot all be read (another pid namespace, /proc mounted with hidepid)
};

// Whether a process this one descends from (its parent, or one further up: a run's before or
// after command reaches vaultik through a shell, sudo or pkexec, which keep no environment of
// it) holds a flock on the file of `fd` (none when -1) or on root's mark `mark`: then the run
// that started this command holds the drive, and waits for the command to end. Never this
// process itself (another DriveLock of it on the drive counts as busy), and never a process that
// is no ancestor (a command that outlived its run, or another account's run).
//
// /proc/locks names the process that took a flock, also after it ended while another process
// still holds the file open, and any account may hold the file `name` all accounts share (`fd`
// may be that one): a holder of that file counts only when it holds it alone (as a run does, not
// a claim's shared lock) and the same process also holds a lock no other account can take, one of
// its account's own files (see own_files_of) or root's mark. Otherwise another account could take
// that file from a process whose number then goes to a process the victim's commands descend
// from (its login shell, say): they would all take it for their parent run and drop their turns.
// Holders of root's mark and of the account's own files (only their owners may open them) count
// as they are. A holder of the shared file that cannot be trusted so is an answer (other), never
// unknown.
Holder held_by_ancestor(int fd, const std::string& name, const std::string& mark) {
    const std::vector<HeldFlock> held = held_flocks();
    std::vector<FileId> trusted = root_mark_files(mark);
    bool shared = false;
    bool target = false;
    FileId id_of_fd{};
    struct stat st{};
    if (fd >= 0 && ::fstat(fd, &st) == 0) {
        target = true;
        id_of_fd = FileId(st.st_dev, st.st_ino);
        shared = is_shared_file(fd, name);
        if (!shared) trusted.push_back(id_of_fd);
    }
    // The file of `fd` is busy (it is asked about only then), so a holder that does not show is
    // one /proc cannot tell.
    bool unknown = target && std::none_of(held.begin(), held.end(),
                                          [&](const HeldFlock& f) { return is_file(held, f, id_of_fd); });
    std::vector<long> holders;
    for (const HeldFlock& f : held)
        for (const FileId& id : trusted)
            if (is_file(held, f, id)) {
                if (f.pid <= 0) unknown = true;  // in another pid namespace
                else holders.push_back(f.pid);
            }
    if (shared) {
        // Each holder of the shared file alone, if it also holds one of its account's own files.
        for (const HeldFlock& f : held) {
            if (!is_file(held, f, id_of_fd) || !f.write || f.pid <= 0) continue;
            const std::vector<FileId> own = own_files_of(account_of(f.pid), name);
            const bool holds_own = std::any_of(held.begin(), held.end(), [&](const HeldFlock& g) {
                return g.pid == f.pid &&
                       std::any_of(own.begin(), own.end(), [&](const FileId& id) { return is_file(held, g, id); });
            });
            if (holds_own) holders.push_back(f.pid);
        }
    }
    if (!holders.empty()) {
        long pid = static_cast<long>(::getppid());
        bool whole = false;  // read up to the first process
        for (int depth = 0; depth < 64 && pid > 0; ++depth) {
            if (std::find(holders.begin(), holders.end(), pid) != holders.end()) return Holder::parent;
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

// Whether a process of root's holds the file of `fd` alone (as root's eject holds the claims
// file); only root can make that so.
bool root_holds_alone(int fd) {
    struct stat st{};
    if (::fstat(fd, &st) != 0) return false;
    const FileId id(st.st_dev, st.st_ino);
    for (const HeldFlock& f : held_flocks())
        if (is_file(f, id) && f.write && account_of(f.pid) == static_cast<long long>(hooks().root_uid)) return true;
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
    if (own < 0 && shared < 0) return;
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
    if (shared >= 0) {
        // Root: every root run took root's own lock first, so whoever holds this one now is another
        // account. An account: this account's runs took its own first, so whoever holds it is root
        // (while root's mark is held) or another account. Another account is waited for only a
        // short while (or for the whole wait, when that is shorter); an eject of an account passes
        // it by at once, while root's eject counts it as busy (fail closed, root's runs take turns
        // with every account's). An account also waits while root's mark is held with this one
        // free: a run of root's that passed by an account's run on it is still under way.
        const auto bound = other_account_wait();
        auto other_deadline = std::min(deadline, std::chrono::steady_clock::now() + bound);
        for (;;) {
            // Asked once a turn: root's run may end between two looks, which would then disagree
            // (an eject of an account's would pass by a lock it just saw root's run hold).
            bool by_root = false;
            bool busy = true;  // the shared lock itself (else only root's mark is held)
            if (::flock(shared, LOCK_EX | LOCK_NB) == 0) {
                if (root) break;
                by_root = root_holds(mark, false);
                if (!by_root) break;
                ::flock(shared, LOCK_UN);
                busy = false;
            } else if (errno != EWOULDBLOCK && errno != EINTR) {
                ::close(shared);
                shared = -1;
                break;
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
        if (shared >= 0) take(shared, ::geteuid() != 0);
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
