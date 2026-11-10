#include "vaultik/jobs/profile_items.hpp"

namespace vaultik::jobs {

const std::vector<ProfileItem>& profile_items() {
    using F = os::UserFolder;
    static const std::vector<ProfileItem> items = {
        {"documents", "documents", F::Documents, "", {"Documents", "OneDrive/Documents"}},
        {"desktop", "desktop", F::Desktop, "", {"Desktop", "OneDrive/Desktop"}},
        {"pictures", "pictures", F::Pictures, "", {"Pictures", "OneDrive/Pictures"}},
        {"music", "music", F::Music, "", {"Music"}},
        {"videos", "videos", F::Videos, "", {"Videos"}},
        {"downloads", "downloads", F::Downloads, "", {"Downloads"}},
        // Browser profiles: bookmarks, passwords (sealed for the old account: browsers ask to sign
        // in again), history and extensions.
        {"chrome", "browsers", F::LocalAppData, "Google/Chrome/User Data", {"AppData/Local/Google/Chrome/User Data"}},
        {"edge", "browsers", F::LocalAppData, "Microsoft/Edge/User Data", {"AppData/Local/Microsoft/Edge/User Data"}},
        {"brave", "browsers", F::LocalAppData, "BraveSoftware/Brave-Browser/User Data",
         {"AppData/Local/BraveSoftware/Brave-Browser/User Data"}},
        {"firefox", "browsers", F::RoamingAppData, "Mozilla/Firefox", {"AppData/Roaming/Mozilla/Firefox"}},
        {"opera", "browsers", F::RoamingAppData, "Opera Software", {"AppData/Roaming/Opera Software"}},
        // Email: Thunderbird's profiles and Outlook's data files (.pst, .ost).
        {"thunderbird", "email", F::RoamingAppData, "Thunderbird", {"AppData/Roaming/Thunderbird"}},
        {"outlook", "email", F::LocalAppData, "Microsoft/Outlook", {"AppData/Local/Microsoft/Outlook"}},
        {"outlook_files", "email", F::Documents, "Outlook Files", {"Documents/Outlook Files"}},
        // Game saves: Windows' own folder for them, and the one many games use in Documents.
        {"saved_games", "games", F::SavedGames, "", {"Saved Games"}},
        {"my_games", "games", F::Documents, "My Games", {"Documents/My Games"}},
    };
    return items;
}

const std::vector<std::string>& profile_groups() {
    static const std::vector<std::string> groups = {"documents", "desktop", "pictures", "music", "videos",
                                                    "downloads", "browsers", "email",    "games"};
    return groups;
}

std::string current_place(const ProfileItem& item) {
    std::string base = os::user_folder(item.base);
    if (base.empty()) return {};
    std::string out = base;
    std::size_t start = 0;
    while (start < item.under.size()) {
        std::size_t end = item.under.find('/', start);
        if (end == std::string::npos) end = item.under.size();
        out = os::join(out, item.under.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::vector<std::string> existing_paths(const std::string& group) {
    std::vector<std::string> out;
    for (const ProfileItem& item : profile_items()) {
        if (item.group != group) continue;
        const std::string place = current_place(item);
        if (!place.empty() && os::is_directory(place)) out.push_back(place);
    }
    return out;
}

}  // namespace vaultik::jobs
