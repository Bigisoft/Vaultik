#pragma once

// Archive storage (S3 GLACIER and DEEP_ARCHIVE, see Backend::data_class): packs of file data must
// be thawed before a restore can read them. This works out which packs a restore needs; the
// folder listings it reads for that are in the standard class.

#include <string>
#include <vector>

#include "vaultik/repo/repository.hpp"

namespace vaultik {

// The packs holding the file data (content, streams and permissions) that restoring `paths` of
// `snapshot` reads: '/'-separated paths as for restore's --path, or the whole version when
// `paths` is empty. Sorted, each pack once. Throws Error for a path the version does not have, or
// a folder listing or chunk that is not in the index.
std::vector<Id> packs_for_restore(Repository& repo, const Snapshot& snapshot, const std::vector<std::string>& paths);

}  // namespace vaultik
