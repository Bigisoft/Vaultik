#include "vaultik/ops/apps.hpp"

namespace vaultik {

// Programs installed by the system's package manager are not listed (yet).
std::vector<UninstallEntry> read_uninstall_entries() {
    return {};
}

std::string winget_program() {
    return {};
}

}  // namespace vaultik
