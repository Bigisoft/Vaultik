// Windows resource controls: priority class, per-handle I/O priority hints, memory priority, and
// a Job Object for the hard CPU and memory caps.

#include <windows.h>

#include "vaultik/util/keep_awake.hpp"
#include "vaultik/util/resources_detail.hpp"
#include "vaultik/util/win.hpp"

namespace vaultik {

namespace {

HANDLE g_job = nullptr;  // lives until the process exits
bool g_in_job = false;

DWORD priority_class(CpuPriority p) {
    switch (p) {
        case CpuPriority::Idle: return IDLE_PRIORITY_CLASS;
        case CpuPriority::Low: return BELOW_NORMAL_PRIORITY_CLASS;
        case CpuPriority::Normal: return NORMAL_PRIORITY_CLASS;
        case CpuPriority::High: return ABOVE_NORMAL_PRIORITY_CLASS;
    }
    return NORMAL_PRIORITY_CLASS;
}

void apply_job_limits(const ResourcePolicy& p) {
    if (p.cpu_limit_percent == 0 && p.max_memory == 0) return;
    if (g_job == nullptr) {
        g_job = CreateJobObjectW(nullptr, nullptr);
        if (g_job == nullptr) throw_last_error("cannot create job object for resource limits");
    }
    if (p.cpu_limit_percent != 0) {
        JOBOBJECT_CPU_RATE_CONTROL_INFORMATION cpu{};
        cpu.ControlFlags = JOB_OBJECT_CPU_RATE_CONTROL_ENABLE | JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP;
        cpu.CpuRate = p.cpu_limit_percent * 100;  // in 1/100 of a percent
        if (!SetInformationJobObject(g_job, JobObjectCpuRateControlInformation, &cpu, sizeof(cpu)))
            throw_last_error("cannot set CPU limit");
    }
    if (p.max_memory != 0) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION ext{};
        ext.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        ext.ProcessMemoryLimit = static_cast<SIZE_T>(p.max_memory);
        if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &ext, sizeof(ext)))
            throw_last_error("cannot set memory limit");
    }
    if (!g_in_job) {
        if (!AssignProcessToJobObject(g_job, GetCurrentProcess()))
            throw_last_error("cannot apply CPU/memory limits to this process");
        g_in_job = true;
    }
}

}  // namespace

namespace detail {

void apply_platform_policy(const ResourcePolicy& p) {
    const HANDLE self = GetCurrentProcess();
    if (!SetPriorityClass(self, priority_class(p.cpu))) throw_last_error("cannot set CPU priority");

    MEMORY_PRIORITY_INFORMATION mem{};
    mem.MemoryPriority = p.memory == MemoryPriority::Low ? MEMORY_PRIORITY_LOW : MEMORY_PRIORITY_NORMAL;
    if (!SetProcessInformation(self, ProcessMemoryPriority, &mem, sizeof(mem)))
        throw_last_error("cannot set memory priority");

    apply_job_limits(p);
}

void cpu_cap_tick() {}  // the Job Object enforces the cap

}  // namespace detail

void apply_io_priority(void* handle) {
    const IoPriority io = current_resource_policy().io;
    if (io == IoPriority::Normal) return;
    FILE_IO_PRIORITY_HINT_INFO hint{};
    hint.PriorityHint = io == IoPriority::Background ? IoPriorityHintVeryLow : IoPriorityHintLow;
    SetFileInformationByHandle(static_cast<HANDLE>(handle), FileIoPriorityHintInfo, &hint, sizeof(hint));
}

namespace {

// A power request that keeps the system (not the display) awake while it is set.
class WinPowerRequester : public PowerRequester {
public:
    bool acquire(const std::string& reason) override {
        reason_ = widen(reason);
        REASON_CONTEXT context{};
        context.Version = POWER_REQUEST_CONTEXT_VERSION;
        context.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        context.Reason.SimpleReasonString = reason_.data();
        request_ = PowerCreateRequest(&context);
        if (request_ == INVALID_HANDLE_VALUE) {
            request_ = nullptr;
        } else if (!PowerSetRequest(request_, PowerRequestSystemRequired)) {
            CloseHandle(request_);
            request_ = nullptr;
        }
        if (request_ != nullptr) return true;
        // Without a power request: the older way, for this thread (the one that runs the work).
        using_state_ = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED) != 0;
        return using_state_;
    }
    void release() override {
        if (request_ != nullptr) {
            PowerClearRequest(request_, PowerRequestSystemRequired);
            CloseHandle(request_);
            request_ = nullptr;
        }
        if (using_state_) {
            SetThreadExecutionState(ES_CONTINUOUS);
            using_state_ = false;
        }
    }

private:
    std::wstring reason_;
    HANDLE request_ = nullptr;
    bool using_state_ = false;
};

}  // namespace

std::unique_ptr<PowerRequester> system_power_requester() {
    return std::make_unique<WinPowerRequester>();
}

}  // namespace vaultik
