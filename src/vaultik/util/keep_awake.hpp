#pragma once

// Keeps the computer from going to sleep while a long run (a backup, a health check, a prune, a
// restore) goes on, so a run of several hours is not cut off by the power settings. Only sleep is
// held off: the display still turns off and the screen still locks as usual.
//
// Windows: a power request (PowerCreateRequest, PowerRequestSystemRequired) with a reason that
// "powercfg /requests" shows. Linux: a logind sleep inhibitor (org.freedesktop.login1.Manager.
// Inhibit "sleep", mode "block", through systemd-inhibit) when logind is there; without it the run
// goes on without one. The request ends when the run ends, however it ends (and with the process,
// should it be killed).

#include <memory>
#include <string>

namespace vaultik {

// The system's way of keeping the computer awake; a fake one in tests.
class PowerRequester {
public:
    virtual ~PowerRequester() = default;
    // Starts holding off sleep; false when the system offers no way to (the run goes on anyway).
    virtual bool acquire(const std::string& reason) = 0;
    // Ends what acquire started. Called once after each acquire that returned true.
    virtual void release() = 0;
};

// The platform's requester (see above).
std::unique_ptr<PowerRequester> system_power_requester();

// Holds the computer awake from construction to destruction (RAII): released on every way out of
// the scope, a return as much as an exception. Never throws.
class KeepAwake {
public:
    explicit KeepAwake(const std::string& reason, std::unique_ptr<PowerRequester> requester = system_power_requester());
    ~KeepAwake();
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;

    // Whether the system holds the computer awake for this run.
    bool active() const { return active_; }

private:
    std::unique_ptr<PowerRequester> requester_;
    bool active_ = false;
};

}  // namespace vaultik
