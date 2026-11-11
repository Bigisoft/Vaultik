#include "vaultik/platform/usn_journal.hpp"

#include "vaultik/util/error.hpp"

// Linux: there is no change journal; callers fall back to a full scan.

namespace vaultik::os {

std::optional<UsnJournalState> usn_journal_state(const std::string&) {
    return std::nullopt;
}

std::optional<UsnChanges> usn_changes_since(const std::string&, std::uint64_t, std::int64_t, std::int64_t) {
    return std::nullopt;
}

FileRef file_ref(const std::string&) {
    throw Error("file ids are not implemented on this platform");
}

std::vector<FileRef> link_parents(const std::string&, const std::vector<FileRef>&) {
    return {};
}

}  // namespace vaultik::os
