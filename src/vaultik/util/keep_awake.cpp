#include "vaultik/util/keep_awake.hpp"

namespace vaultik {

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
