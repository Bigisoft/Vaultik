#include "vaultik/ops/mount.hpp"

// Linux: comes with the port (FUSE).

namespace vaultik {

struct SnapshotMount::Impl {
    std::string folder;
};

bool SnapshotMount::available() {
    return false;
}

const char* SnapshotMount::how_to_enable() {
    return "browsing a version as a folder is not available on this system yet";
}

SnapshotMount::SnapshotMount(Repository&, const Snapshot&, std::string) {
    throw Error(how_to_enable());
}

SnapshotMount::~SnapshotMount() = default;

const std::string& SnapshotMount::folder() const {
    return impl_->folder;
}

bool SnapshotMount::remove_leftover(const std::string&) {
    return false;
}

}  // namespace vaultik
