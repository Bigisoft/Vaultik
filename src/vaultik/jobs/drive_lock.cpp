#include "vaultik/jobs/drive_lock.hpp"

#include <algorithm>
#include <cstdint>
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
bool held_by_parent(const std::string& hash) {
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
// account, the system account and administrators may open it. Null when it cannot be used.
HANDLE open_mutex(const std::wstring& name, bool create) {
    const std::vector<std::uint8_t> me = user_sid();
    constexpr DWORD kAccess = SYNCHRONIZE | MUTEX_MODIFY_STATE | READ_CONTROL;
    HANDLE m = nullptr;
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
        if (sd != nullptr) LocalFree(sd);
    }
    if (m == nullptr) m = OpenMutexW(kAccess, FALSE, name.c_str());
    if (m != nullptr && !trusted_owner(m, me)) {
        CloseHandle(m);
        m = nullptr;
    }
    return m;
}

std::wstring wide(const std::string& ascii) {
    return std::wstring(ascii.begin(), ascii.end());
}
#else
// The lock file `name`: in /run/lock, shared by every account, when it belongs to root or this
// account (root takes over one another account made); otherwise in `fallback_dir`. -1 when
// neither can be opened.
int open_lock_file(const std::string& name, const std::string& fallback_dir) {
    struct stat dir{};
    if (::lstat("/run/lock", &dir) == 0 && S_ISDIR(dir.st_mode)) {
        const std::string path = "/run/lock/" + name;
        for (int attempt = 0; attempt < 2; ++attempt) {
            // Read only is enough for flock, so another account's file needs no write access.
            const int fd = ::open(path.c_str(), O_RDONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
            if (fd < 0) break;
            struct stat st{};
            if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_uid == 0 || st.st_uid == ::geteuid())) {
                if (st.st_uid == ::geteuid()) (void)::fchmod(fd, 0644);  // whatever the umask: others share it
                return fd;
            }
            ::close(fd);
            if (::geteuid() != 0 || attempt > 0 || ::unlink(path.c_str()) != 0) break;
        }
    }
    (void)os::make_dir(fallback_dir);  // the user's settings folder, which is there
    return ::open(os::join(fallback_dir, name).c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
}
#endif

}  // namespace

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
                     const std::function<void()>& on_wait, const std::function<bool()>& cancelled) {
    if (key.empty()) return;
    const std::string hash = key_hash(key);
    if (held_by_parent(hash)) {
        // The run that started this command holds the drive and waits for this command to end.
        inherited_ = true;
        return;
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    const auto waiting = [&] {
        if (!waited_ && on_wait) on_wait();
        waited_ = true;
        if (cancelled && cancelled()) throw Error("cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw DriveBusy("another Vaultik run kept the backup drive busy for too long; try again later");
    };
    std::uint8_t rnd[8];
    random_bytes(rnd);
    const std::string token = hash + ":" + to_hex(ByteSpan(rnd, sizeof rnd));
#ifdef _WIN32
    (void)lock_dir;
    HANDLE m = open_mutex(L"Global\\Vaultik-drive-" + wide(hash), true);
    if (m == nullptr) return;  // no lock to share (the runs then overlap, as before)
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const DWORD slice = static_cast<DWORD>(std::clamp<long long>(left.count(), 0, 1000));
        const DWORD r = WaitForSingleObject(m, waited_ ? slice : 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {  // abandoned: its run ended without letting go
            handle_ = m;
            token_ = token;
            return;
        }
        if (r == WAIT_FAILED) {
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
    const int fd = open_lock_file("vaultik-drive-" + hash + ".lock", lock_dir);
    if (fd < 0) return;
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
            token_ = token;
            return;
        }
        if (errno != EWOULDBLOCK && errno != EINTR) {
            ::close(fd);
            return;
        }
        try {
            waiting();
        } catch (...) {
            ::close(fd);
            throw;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::clamp(left, std::chrono::milliseconds(0), std::chrono::milliseconds(250)));
    }
#endif
}

DriveLock::~DriveLock() {
    if (handle_ == nullptr) return;
#ifdef _WIN32
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_) - 1);
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
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
#ifdef _WIN32
        (void)lock_dir;
        // A claim takes one of a few slots; an eject looks at all of them.
        for (int slot = 0; slot < kClaimSlots; ++slot) {
            HANDLE m = open_mutex(L"Global\\Vaultik-use-" + wide(hash) + L"-" + std::to_wstring(slot), true);
            if (m == nullptr) continue;
            const DWORD r = WaitForSingleObject(m, 0);
            if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {
                handle_ = m;
                return;
            }
            CloseHandle(m);
        }
#else
        const int fd = open_lock_file("vaultik-use-" + hash + ".lock", lock_dir);
        if (fd < 0) return;
        // Shared with every other claim; only an eject's short look holds it alone.
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (::flock(fd, LOCK_SH | LOCK_NB) == 0) {
                handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
                return;
            }
            if (errno != EWOULDBLOCK && errno != EINTR) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        ::close(fd);
#endif
    } catch (const std::exception&) {
    }
}

DriveClaim::~DriveClaim() {
    if (handle_ == nullptr) return;
#ifdef _WIN32
    ReleaseMutex(static_cast<HANDLE>(handle_));
    CloseHandle(static_cast<HANDLE>(handle_));
#else
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_) - 1);
    ::flock(fd, LOCK_UN);
    ::close(fd);
#endif
}

bool drive_claimed(const std::string& key, const std::string& lock_dir) {
    if (key.empty()) return false;
    const std::string hash = key_hash(key);
#ifdef _WIN32
    (void)lock_dir;
    for (int slot = 0; slot < kClaimSlots; ++slot) {
        HANDLE m = open_mutex(L"Global\\Vaultik-use-" + wide(hash) + L"-" + std::to_wstring(slot), false);
        if (m == nullptr) continue;  // never made, or by nobody to trust
        const DWORD r = WaitForSingleObject(m, 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(m);
        CloseHandle(m);
        if (r == WAIT_TIMEOUT) return true;
    }
    return false;
#else
    const int fd = open_lock_file("vaultik-use-" + hash + ".lock", lock_dir);
    if (fd < 0) return false;
    const bool claimed = ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd);
    return claimed;
#endif
}

}  // namespace vaultik::jobs
