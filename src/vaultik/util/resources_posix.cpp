// POSIX (Linux) resource controls: nice value, I/O scheduling class, an address-space limit for
// the memory cap, and a duty-cycle limiter for the CPU cap (cgroups would need root delegation).

#include <malloc.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "vaultik/util/error.hpp"
#include "vaultik/util/resources_detail.hpp"

namespace vaultik {

namespace {

int nice_value(CpuPriority p) {
    switch (p) {
        case CpuPriority::Idle: return 19;
        case CpuPriority::Low: return 10;
        case CpuPriority::Normal: return 0;
        case CpuPriority::High: return -5;
    }
    return 0;
}

#ifdef __linux__
// From linux/ioprio.h, which is not always installed.
constexpr int kIoprioWhoProcess = 1;
constexpr int kIoprioClassShift = 13;
constexpr int kIoprioClassBe = 2;
constexpr int kIoprioClassIdle = 3;

void set_io_priority(IoPriority io) {
    int value = 0;  // class 0: derived from the nice value (the kernel default)
    if (io == IoPriority::Background) value = kIoprioClassIdle << kIoprioClassShift;
    if (io == IoPriority::Low) value = (kIoprioClassBe << kIoprioClassShift) | 7;  // lowest best-effort level
    if (::syscall(SYS_ioprio_set, kIoprioWhoProcess, 0, value) != 0 && io != IoPriority::Normal)
        throw Error(std::string("cannot set I/O priority: ") + std::strerror(errno));
}
#else
void set_io_priority(IoPriority) {}
#endif

double process_cpu_seconds() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Keeps CPU time at or below `share` of the wall-clock time by sleeping at the throttle points.
class CpuCap {
public:
    void set(unsigned percent) {
        std::lock_guard lock(mu_);
        const long cpus = ::sysconf(_SC_NPROCESSORS_ONLN);
        // The cap is a share of the whole machine, as on Windows.
        share_ = percent == 0 ? 0.0 : static_cast<double>(percent) / 100.0 * static_cast<double>(cpus > 0 ? cpus : 1);
        start_wall_ = std::chrono::steady_clock::now();
        start_cpu_ = process_cpu_seconds();
    }

    void tick() {
        std::unique_lock lock(mu_);
        if (share_ <= 0.0 || share_ >= 1.0) return;  // a single thread cannot exceed one core
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_wall_).count();
        const double used = process_cpu_seconds() - start_cpu_;
        const double excess = used / share_ - wall;  // seconds of wall time we are ahead
        if (excess <= 0.005) return;
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::duration<double>(excess));
    }

private:
    std::mutex mu_;
    double share_ = 0.0;
    std::chrono::steady_clock::time_point start_wall_{};
    double start_cpu_ = 0.0;
};

CpuCap g_cpu_cap;

// Room for thread stacks (8 MiB each by default) on top of the memory cap.
constexpr std::uint64_t kThreadStackAllowance = 128ull << 20;

}  // namespace

namespace detail {

void apply_platform_policy(const ResourcePolicy& p) {
    // Raising priority needs CAP_SYS_NICE; without it "high" quietly stays at normal.
    errno = 0;
    if (::setpriority(PRIO_PROCESS, 0, nice_value(p.cpu)) != 0 && p.cpu != CpuPriority::High &&
        p.cpu != CpuPriority::Normal)
        throw Error(std::string("cannot set CPU priority: ") + std::strerror(errno));
    set_io_priority(p.io);
    // MemoryPriority has no Linux equivalent for an unprivileged process; it is ignored.

    // The memory cap is RLIMIT_DATA (heap and private writable memory, since Linux 4.7), the
    // closest to the committed memory Windows caps. RLIMIT_AS would also count address space that
    // is only reserved (64 MiB per malloc arena and thread), so a cap of a few hundred MiB would
    // fail at once. Fewer malloc arenas keep reserved-but-unused heap down when a cap is set.
    static bool capped = false;
    struct rlimit rl{};
    ::getrlimit(RLIMIT_DATA, &rl);
    if (p.max_memory != 0) {
        // Only the soft limit, so a later call can lift it again. Thread stacks count too, so the
        // cap leaves room for them.
        rl.rlim_cur = static_cast<rlim_t>(p.max_memory + kThreadStackAllowance);
        if (rl.rlim_max != RLIM_INFINITY && rl.rlim_cur > rl.rlim_max) rl.rlim_cur = rl.rlim_max;
        if (::setrlimit(RLIMIT_DATA, &rl) != 0)
            throw Error(std::string("cannot set memory limit: ") + std::strerror(errno));
#ifdef M_ARENA_MAX
        ::mallopt(M_ARENA_MAX, 2);
#endif
        capped = true;
    } else if (capped) {
        rl.rlim_cur = rl.rlim_max;  // lift the cap set earlier
        (void)::setrlimit(RLIMIT_DATA, &rl);
        capped = false;
    }
    g_cpu_cap.set(p.cpu_limit_percent);
}

void cpu_cap_tick() {
    g_cpu_cap.tick();
}

}  // namespace detail

void apply_io_priority(void* /*handle*/) {}  // per process on Linux, set in apply_platform_policy

}  // namespace vaultik
