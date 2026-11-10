#include "vaultik/ops/move_here.hpp"

#include <algorithm>
#include <cctype>

#include "vaultik/jobs/profile_items.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/restore.hpp"

namespace vaultik {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Folders below a drive's Users folder that are no person's own.
bool shared_account(const std::string& name) {
    const std::string l = lower(name);
    return l == "public" || l == "default" || l == "default user" || l == "all users" || l == "defaultapppool";
}

bool is_dir(Repository& repo, const Snapshot& snapshot, const std::string& path) {
    Node n;
    return resolve_snapshot_path(repo, snapshot, path, n) && n.kind == NodeKind::Dir;
}

}  // namespace

std::vector<MoveItem> plan_move(Repository& repo, const Snapshot& snapshot) {
    std::vector<MoveItem> plan;
    const Tree root = parse_tree(repo.load_blob(BlobType::Tree, snapshot.tree));
    for (const Node& drive : root.nodes) {
        if (drive.kind != NodeKind::Dir) continue;
        Node users;
        if (!resolve_snapshot_path(repo, snapshot, drive.name + "/Users", users) || users.kind != NodeKind::Dir)
            continue;
        const Tree accounts = parse_tree(repo.load_blob(BlobType::Tree, users.subtree));
        for (const Node& account : accounts.nodes) {
            if (account.kind != NodeKind::Dir || shared_account(account.name)) continue;
            const std::string profile = drive.name + "/Users/" + account.name;
            std::vector<MoveItem> found;
            for (const jobs::ProfileItem& item : jobs::profile_items()) {
                const std::string to = jobs::current_place(item);
                if (to.empty()) continue;
                for (const std::string& old : item.old_places) {
                    const std::string from = profile + "/" + old;
                    if (!is_dir(repo, snapshot, from)) continue;
                    found.push_back({item.id, item.group, account.name, from, to});
                    break;
                }
            }
            // Leave out what lies inside another item found: it comes along with that one.
            for (const MoveItem& m : found) {
                const bool inside = std::any_of(found.begin(), found.end(), [&](const MoveItem& o) {
                    return &o != &m && lower(m.from).starts_with(lower(o.from) + "/");
                });
                if (!inside) plan.push_back(m);
            }
        }
    }
    return plan;
}

std::vector<std::string> plan_accounts(const std::vector<MoveItem>& plan) {
    std::vector<std::string> out;
    for (const MoveItem& m : plan)
        if (std::find(out.begin(), out.end(), m.account) == out.end()) out.push_back(m.account);
    return out;
}

}  // namespace vaultik
