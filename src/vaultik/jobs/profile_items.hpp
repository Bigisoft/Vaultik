#pragma once

// The things people usually want backed up from their Windows account, and where each lives:
// the ready-made choices of what to back up, and what "Move to this PC" carries over from an old
// computer's backup to this one (each to the same kind of folder here, wherever Windows keeps it).

#include <string>
#include <vector>

#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

struct ProfileItem {
    std::string id;     // "documents", "chrome", ...
    std::string group;  // the ready-made choice it belongs to: "documents", "browsers", "email", "games", ...
    // Where it is on this computer: `under` below the account's `base` folder ("" for the folder itself).
    os::UserFolder base = os::UserFolder::Profile;
    std::string under;
    // Where it was below an old account's own folder (C:\Users\<name>), '/'-separated, in order of
    // preference: the usual place first, then where OneDrive puts it.
    std::vector<std::string> old_places;
};

// Every item, in the order they are shown.
const std::vector<ProfileItem>& profile_items();

// The ready-made choices of what to back up, by group, in the order they are shown.
const std::vector<std::string>& profile_groups();

// The folders of `group` that exist on this computer for the current account.
std::vector<std::string> existing_paths(const std::string& group);

// Where `item` is on this computer (whether or not it exists yet). Empty when the account has no
// such folder.
std::string current_place(const ProfileItem& item);

}  // namespace vaultik::jobs
