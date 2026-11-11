#include "vaultik/ops/history.hpp"

#include <algorithm>
#include <cstdint>
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

// Lower case for searching, which ignores case on every system (Linux file names are case
// sensitive, but someone looking for "report" wants "Report.docx" too). Windows: the system's
// own folding. Elsewhere: ASCII, Latin-1, Latin Extended-A, Greek and Cyrillic, which covers the
// languages Vaultik is translated into; other letters compare as they are.
std::string search_fold(std::string_view s) {
#ifdef _WIN32
    return os::fold_case(s);
#else
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        std::size_t len = 1;
        if (c >= 0xC0 && c < 0xE0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        }
        if (len == 1) {
            out.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c));
            i += 1;
            continue;
        }
        std::uint32_t lower = cp;
        if ((cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) || (cp >= 0x391 && cp <= 0x3AB && cp != 0x3A2) ||
            (cp >= 0x410 && cp <= 0x42F))
            lower = cp + 0x20;
        else if (cp >= 0x400 && cp <= 0x40F)
            lower = cp + 0x50;
        else if ((cp >= 0x100 && cp <= 0x137 && cp != 0x130 && cp % 2 == 0) ||
                 (cp >= 0x139 && cp <= 0x148 && cp % 2 == 1) || (cp >= 0x14A && cp <= 0x177 && cp % 2 == 0) ||
                 (cp >= 0x179 && cp <= 0x17E && cp % 2 == 1))
            lower = cp + 1;
        else if (cp == 0x178)
            lower = 0xFF;
        if (lower >= 0x800) {
            out.append(s.substr(i, len));  // not reached for the ranges above; kept as is
        } else {
            out.push_back(static_cast<char>(0xC0 | (lower >> 6)));
            out.push_back(static_cast<char>(0x80 | (lower & 0x3F)));
        }
        i += len;
    }
    return out;
#endif
}

}  // namespace

bool name_matches(const std::string& pattern, const std::string& name) {
    const std::string p = search_fold(pattern);
    const std::string n = search_fold(name);
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
