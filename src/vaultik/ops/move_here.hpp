#pragma once

// "Move to this PC": the folders of an old computer's account found in a backup (Documents,
// Desktop, browser profiles, email, game saves; see jobs/profile_items.hpp), each restored into
// the same kind of folder of the current account here, wherever Windows keeps it now (another
// user name, Documents moved into OneDrive, another drive letter).

#include <string>
#include <vector>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/repository.hpp"

namespace vaultik {

struct MoveItem {
    std::string id;       // the profile item ("documents", "chrome", ...)
    std::string group;    // its ready-made choice ("documents", "browsers", ...)
    std::string account;  // the old account's folder name ("anna")
    std::string from;     // its path in the backup, '/'-separated ("C/Users/anna/Documents")
    std::string to;       // the folder on this computer it goes into
};

// What the backup holds of each old account, in the order of jobs::profile_items(). An item
// inside another one found (Documents\Outlook Files inside Documents) is left out: it comes along.
std::vector<MoveItem> plan_move(Repository& repo, const Snapshot& snapshot);

// The old accounts in a plan, each once, in order.
std::vector<std::string> plan_accounts(const std::vector<MoveItem>& plan);

}  // namespace vaultik
