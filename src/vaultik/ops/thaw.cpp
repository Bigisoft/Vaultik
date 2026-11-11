#include "vaultik/ops/thaw.hpp"

#include <algorithm>
#include <unordered_set>

#include "vaultik/model/tree.hpp"
#include "vaultik/ops/restore.hpp"

namespace vaultik {

std::vector<Id> packs_for_restore(Repository& repo, const Snapshot& snapshot, const std::vector<std::string>& paths) {
    std::vector<Node> starts;
    if (paths.empty()) {
        Node root;
        root.kind = NodeKind::Dir;
        root.subtree = snapshot.tree;
        starts.push_back(std::move(root));
    }
    for (const std::string& path : paths) {
        Node n;
        if (!resolve_snapshot_path(repo, snapshot, path, n)) throw Error("path not found in snapshot: " + path);
        starts.push_back(std::move(n));
    }

    const Index& index = repo.index();
    std::vector<bool> needed(index.packs().size(), false);
    std::unordered_set<Id, IdHash> trees_seen;
    const auto add_data = [&](const Node& n) {
        for_each_data_blob(n, [&](const Id& chunk) {
            const BlobLocation* loc = index.find(chunk);
            if (loc == nullptr) throw Error("data blob not found: " + id_hex(chunk));
            needed[loc->pack] = true;
        });
    };
    std::vector<Id> pending;
    for (const Node& n : starts) {
        add_data(n);
        if (n.kind == NodeKind::Dir && trees_seen.insert(n.subtree).second) pending.push_back(n.subtree);
    }
    while (!pending.empty()) {
        const Id tree_id = pending.back();
        pending.pop_back();
        const Tree tree = parse_tree(repo.load_blob(BlobType::Tree, tree_id));
        for (const Node& n : tree.nodes) {
            add_data(n);
            if (n.kind == NodeKind::Dir && trees_seen.insert(n.subtree).second) pending.push_back(n.subtree);
        }
    }

    std::vector<Id> out;
    for (std::size_t p = 0; p < needed.size(); ++p)
        if (needed[p]) out.push_back(index.packs()[p]);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace vaultik
