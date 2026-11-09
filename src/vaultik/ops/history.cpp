#include "vaultik/ops/history.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>

#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

class Cancelled : public Error {
public:
    Cancelled() : Error("cancelled") {}
};

// Folder listings by id, kept while one search or history runs.
class TreeCache {
public:
    explicit TreeCache(Repository& repo) : repo_(repo) {}

    const Tree& get(const Id& id) {
        auto it = trees_.find(id);
        if (it != trees_.end()) return *it->second;
        if (trees_.size() > 20000) trees_.clear();  // bound memory on huge histories
        auto tree = std::make_shared<Tree>(parse_tree(repo_.load_blob(BlobType::Tree, id)));
        return *trees_.emplace(id, std::move(tree)).first->second;
    }

private:
    Repository& repo_;
    std::unordered_map<Id, std::shared_ptr<Tree>, IdHash> trees_;
};

std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        if (end > pos) out.push_back(path.substr(pos, end - pos));
        pos = end + 1;
    }
    return out;
}

std::optional<Node> lookup(TreeCache& cache, const Id& root, const std::vector<std::string>& parts) {
    if (parts.empty()) return std::nullopt;
    const Tree* tree = &cache.get(root);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const Node* n = tree->find(parts[i]);
        if (n == nullptr) return std::nullopt;
        if (i + 1 == parts.size()) return *n;
        if (n->kind != NodeKind::Dir) return std::nullopt;
        tree = &cache.get(n->subtree);
    }
    return std::nullopt;
}

bool glob(const std::string& p, std::size_t pi, const std::string& s, std::size_t si) {
    while (pi < p.size()) {
        if (p[pi] == '*') {
            while (pi < p.size() && p[pi] == '*') ++pi;
            if (pi == p.size()) return true;
            for (std::size_t k = si; k <= s.size(); ++k)
                if (glob(p, pi, s, k)) return true;
            return false;
        }
        if (si >= s.size()) return false;
        if (p[pi] != '?' && p[pi] != s[si]) return false;
        ++pi;
        ++si;
    }
    return si == s.size();
}

}  // namespace

bool name_matches(const std::string& pattern, const std::string& name) {
    const std::string p = os::fold_case(pattern);
    const std::string n = os::fold_case(name);
    if (p.find_first_of("*?") == std::string::npos) return n.find(p) != std::string::npos;
    return glob(p, 0, n, 0);
}

bool same_content(const Node& a, const Node& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case NodeKind::File: return a.size == b.size && a.content == b.content;
        case NodeKind::Dir: return a.subtree == b.subtree;
        case NodeKind::Symlink: return a.link_target == b.link_target;
        case NodeKind::Reparse: return a.mtime == b.mtime && a.attributes == b.attributes;
    }
    return false;
}

std::vector<ItemVersion> item_history(Repository& repo, const std::vector<SnapshotRecord>& newest_first,
                                      const std::string& path, const std::function<bool()>& cancelled) {
    TreeCache cache(repo);
    const std::vector<std::string> parts = split(path);
    std::vector<ItemVersion> found;
    for (std::size_t i = 0; i < newest_first.size(); ++i) {
        if (cancelled && cancelled()) throw Error("cancelled");
        std::optional<Node> node;
        try {
            node = lookup(cache, newest_first[i].snapshot.tree, parts);
        } catch (const Error&) {
            continue;  // an unreadable snapshot does not hide the others; verify reports it
        }
        if (node) found.push_back({i, std::move(*node), false});
    }
    // Oldest to newest: a version is a change when it differs from the one before it.
    for (std::size_t k = found.size(); k-- > 0;) {
        found[k].changed = k + 1 == found.size() || !same_content(found[k].node, found[k + 1].node);
    }
    return found;
}

std::vector<FoundItem> find_items(Repository& repo, const std::vector<SnapshotRecord>& newest_first,
                                  const std::string& pattern, std::size_t limit, bool* truncated,
                                  const std::function<bool()>& cancelled) {
    if (truncated != nullptr) *truncated = false;
    TreeCache cache(repo);
    struct Match {
        std::string path;
        NodeKind kind;
        std::uint64_t size;
    };
    // Matches below each folder listing, by its id: unchanged folders are shared between
    // snapshots, so each is searched once.
    std::unordered_map<Id, std::shared_ptr<std::vector<Match>>, IdHash> memo;
    std::size_t stored = 0;
    const std::size_t store_limit = std::max<std::size_t>(limit * 50, 20000);
    bool too_many = false;

    const std::function<std::shared_ptr<std::vector<Match>>(const Id&)> matches = [&](const Id& tree_id) {
        if (auto it = memo.find(tree_id); it != memo.end()) return it->second;
        if (cancelled && cancelled()) throw Cancelled();
        auto out = std::make_shared<std::vector<Match>>();
        const Tree tree = cache.get(tree_id);  // a copy: the cache may drop entries while recursing
        for (const Node& n : tree.nodes) {
            if (name_matches(pattern, n.name)) out->push_back({n.name, n.kind, n.size});
            if (n.kind == NodeKind::Dir && !too_many) {
                for (const Match& m : *matches(n.subtree)) out->push_back({n.name + "/" + m.path, m.kind, m.size});
            }
            if (out->size() + stored > store_limit) {
                too_many = true;
                break;
            }
        }
        stored += out->size();
        memo.emplace(tree_id, out);
        return out;
    };

    std::map<std::string, FoundItem> items;
    for (std::size_t i = 0; i < newest_first.size() && !too_many; ++i) {
        std::shared_ptr<std::vector<Match>> list;
        try {
            list = matches(newest_first[i].snapshot.tree);
        } catch (const Cancelled&) {
            throw;
        } catch (const Error&) {
            continue;
        }
        for (const Match& m : *list) {
            FoundItem& item = items[m.path];
            if (item.snapshots.empty()) {
                item.path = m.path;
                item.kind = m.kind;
                item.size = m.size;
            }
            item.snapshots.push_back(i);
        }
    }
    std::vector<FoundItem> out;
    for (auto& [path, item] : items) {
        if (out.size() >= limit) {
            if (truncated != nullptr) *truncated = true;
            break;
        }
        out.push_back(std::move(item));
    }
    if (too_many && truncated != nullptr) *truncated = true;
    return out;
}

}  // namespace vaultik
