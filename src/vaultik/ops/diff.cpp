#include "vaultik/ops/diff.hpp"

#include "vaultik/ops/history.hpp"  // same_content

namespace vaultik {

namespace {

class Differ {
public:
    Differ(Repository& repo, std::size_t limit, const std::function<bool()>& cancelled)
        : repo_(repo), limit_(limit), cancelled_(cancelled) {}

    void compare(const Id& old_tree, const Id& new_tree, const std::string& prefix) {
        if (old_tree == new_tree) return;  // the same listing, all the way down
        if (cancelled_ && cancelled_()) throw Error("cancelled");
        const Tree a = parse_tree(repo_.load_blob(BlobType::Tree, old_tree));
        const Tree b = parse_tree(repo_.load_blob(BlobType::Tree, new_tree));
        // Both are sorted by name (bytewise): walk them together.
        std::size_t i = 0, j = 0;
        while (i < a.nodes.size() || j < b.nodes.size()) {
            const Node* x = i < a.nodes.size() ? &a.nodes[i] : nullptr;
            const Node* y = j < b.nodes.size() ? &b.nodes[j] : nullptr;
            if (y == nullptr || (x != nullptr && x->name < y->name)) {
                add(Change::Kind::Removed, prefix + x->name, x, nullptr);
                ++i;
            } else if (x == nullptr || y->name < x->name) {
                add(Change::Kind::Added, prefix + y->name, nullptr, y);
                ++j;
            } else {
                const std::string path = prefix + x->name;
                if (x->kind == NodeKind::Dir && y->kind == NodeKind::Dir) {
                    compare(x->subtree, y->subtree, path + "/");
                } else if (x->kind != y->kind) {
                    add(Change::Kind::Removed, path, x, nullptr);
                    add(Change::Kind::Added, path, nullptr, y);
                } else if (!same_content(*x, *y)) {
                    add(Change::Kind::Changed, path, x, y);
                }
                ++i;
                ++j;
            }
        }
    }

    DiffResult result;

private:
    void add(Change::Kind kind, std::string path, const Node* old_node, const Node* new_node) {
        switch (kind) {
            case Change::Kind::Added: ++result.added; break;
            case Change::Kind::Removed: ++result.removed; break;
            case Change::Kind::Changed: ++result.changed; break;
        }
        if (result.changes.size() >= limit_) {
            result.truncated = true;
            return;
        }
        Change c;
        c.kind = kind;
        c.path = std::move(path);
        const Node& n = new_node != nullptr ? *new_node : *old_node;
        c.node_kind = n.kind;
        if (old_node != nullptr) {
            c.old_node = *old_node;
            if (old_node->kind == NodeKind::File) c.old_size = old_node->size;
        }
        if (new_node != nullptr) {
            c.new_node = *new_node;
            if (new_node->kind == NodeKind::File) c.new_size = new_node->size;
        }
        result.changes.push_back(std::move(c));
    }

    Repository& repo_;
    std::size_t limit_;
    const std::function<bool()>& cancelled_;
};

}  // namespace

DiffResult diff_snapshots(Repository& repo, const Snapshot& older, const Snapshot& newer, std::size_t limit,
                          const std::function<bool()>& cancelled) {
    Differ d(repo, limit, cancelled);
    d.compare(older.tree, newer.tree, std::string());
    return std::move(d.result);
}

}  // namespace vaultik
