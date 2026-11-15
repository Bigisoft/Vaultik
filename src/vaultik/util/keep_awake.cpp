#include "vaultik/util/keep_awake.hpp"

#include <set>

namespace vaultik {

bool command_keeps_awake(const std::string& command, const std::string& sub) {
    static const std::set<std::string> kLongRuns = {"run-job", "check-job",   "test-restore", "copy",
                                                    "backup",  "restore",     "verify",       "prune",
                                                    "forget",  "repair",      "repair-data",  "move-here",
                                                    "image"};
    // Erasing a disk or the free space of a drive takes hours on a large one: a computer that
    // sleeps part way cuts it off (a USB disk's handle is then gone, and the erase fails).
    if (command == "disk") return sub == "wipe" || sub == "wipe-free";
    return kLongRuns.contains(command);
}

KeepAwake::KeepAwake(const std::string& reason, std::unique_ptr<PowerRequester> requester)
    : requester_(std::move(requester)) {
    if (!requester_) return;
    try {
        active_ = requester_->acquire(reason);
    } catch (...) {
        active_ = false;  // the run goes on without it
    }
}

KeepAwake::~KeepAwake() {
    if (!active_) return;
    try {
        requester_->release();
    } catch (...) {
    }
}

}  // namespace vaultik
