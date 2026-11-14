#include "vaultik/jobs/drive_lock.hpp"

#include <algorithm>
#include <cstdint>
#include <thread>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/util/hex.hpp"

#ifdef _WIN32
#include <windows.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
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
    const auto deadline = std::chrono::steady_clock::now() + wait;
    const auto waiting = [&] {
        if (!waited_ && on_wait) on_wait();
        waited_ = true;
        if (cancelled && cancelled()) throw Error("cancelled");
        if (std::chrono::steady_clock::now() >= deadline)
            throw DriveBusy("another Vaultik run kept the backup drive busy for too long; try again later");
    };
#ifdef _WIN32
    (void)lock_dir;
    // Every account may wait for it and release it; only the system and administrators may change
    // it. (A run of another account that created it first must not lock this one out.)
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;AU)",
                                                             SDDL_REVISION_1, &sd, nullptr))
        sa.lpSecurityDescriptor = sd;
    const std::string hash = key_hash(key);
    const std::wstring name = L"Global\\Vaultik-drive-" + std::wstring(hash.begin(), hash.end());
    HANDLE m = CreateMutexExW(&sa, name.c_str(), 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
    if (m == nullptr) m = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, name.c_str());
    if (sd != nullptr) LocalFree(sd);
    if (m == nullptr) return;  // no lock to share (the runs then overlap, as before)
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const DWORD slice = static_cast<DWORD>(std::clamp<long long>(left.count(), 0, 1000));
        const DWORD r = WaitForSingleObject(m, waited_ ? slice : 0);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) {  // abandoned: its run ended without letting go
            handle_ = m;
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
    const std::string path = os::join(lock_dir, "drive-" + key_hash(key) + ".lock");
    (void)os::make_dir(lock_dir);  // the user's settings folder, which is there
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return;
    for (;;) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
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

}  // namespace vaultik::jobs
