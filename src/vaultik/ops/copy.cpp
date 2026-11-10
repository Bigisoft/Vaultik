#include "vaultik/ops/copy.hpp"

#include <algorithm>
#include <unordered_map>

#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"  // Cancelled
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

constexpr char kMapMagic[8] = {'V', 'K', 'C', 'O', 'P', 'Y', '0', '1'};

using IdMap = std::unordered_map<Id, Id, IdHash>;

// The map file: the magic, then pairs of ids (from, to). A damaged or foreign file is ignored.
IdMap load_map(const std::string& path) {
    IdMap map;
    if (path.empty()) return map;
    try {
        const Bytes data = os::read_file(path, 1u << 30);
        if (data.size() < sizeof kMapMagic || !std::equal(kMapMagic, kMapMagic + sizeof kMapMagic, data.begin()) ||
            (data.size() - sizeof kMapMagic) % 64 != 0)
            return map;
        for (std::size_t at = sizeof kMapMagic; at < data.size(); at += 64) {
            Id from{}, to{};
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at), 32, from.begin());
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at + 32), 32, to.begin());
            map.emplace(from, to);
        }
    } catch (const Error&) {
    }
    return map;
}

void save_map(const std::string& path, const IdMap& map) {
    if (path.empty()) return;
    Bytes data(kMapMagic, kMapMagic + sizeof kMapMagic);
    data.reserve(data.size() + map.size() * 64);
    for (const auto& [from, to] : map) {
        data.insert(data.end(), from.begin(), from.end());
        data.insert(data.end(), to.begin(), to.end());
    }
    os::make_dirs(os::parent_of(path));
    os::write_file_atomic(path, data, 0600);
}

class Copier {
public:
    Copier(Repository& from, Repository& to, const CopyOptions& options)
        : from_(from), to_(to), options_(options), map_(load_map(options.map_file)) {}

    CopyResult run() {
        // The snapshots to copy, oldest first, so a copy's parent is copied before it.
        struct Entry {
            Id id;
            Snapshot snapshot;
        };
        std::vector<Entry> wanted;
        for (const Id& id : from_.list_snapshot_ids()) {
            Snapshot s = from_.load_snapshot(id);
            const bool tagged = std::all_of(options_.tags.begin(), options_.tags.end(), [&](const std::string& t) {
                return std::find(s.tags.begin(), s.tags.end(), t) != s.tags.end();
            });
            if (tagged) wanted.push_back({id, std::move(s)});
        }
        std::sort(wanted.begin(), wanted.end(), [](const Entry& a, const Entry& b) { return a.snapshot.time < b.snapshot.time; });

        // What the target already has: original id -> copy id (and the copy itself).
        IdMap copies;
        std::unordered_map<Id, Snapshot, IdHash> copy_snapshots;
        for (const Id& id : to_.list_snapshot_ids()) {
            Snapshot s = to_.load_snapshot(id);
            if (s.copy_of) {
                copies.emplace(*s.copy_of, id);
                copy_snapshots.emplace(id, std::move(s));
            }
        }

        std::size_t n = 0;
        for (const Entry& e : wanted) {
            ++n;
            if (const auto have = copies.find(e.id); have != copies.end()) {
                ++result_.present;
                // A pin or note added to the original later (an automatic pin after a ransomware
                // alert, say) reaches the copy too, so the copy's rules keep the same versions.
                // Only added, never taken away: a pin made on the copy itself stays.
                Snapshot& copy = copy_snapshots.at(have->second);
                const bool pin = e.snapshot.pinned && !copy.pinned;
                const bool note = !e.snapshot.note.empty() && copy.note.empty();
                if (pin || note) {
                    if (pin) copy.pinned = true;
                    if (note) copy.note = e.snapshot.note;
                    to_.update_snapshot(have->second, copy);
                    ++result_.updated;
                }
                continue;
            }
            label_ = "version " + std::to_string(n) + " of " + std::to_string(wanted.size());
            Snapshot copy = e.snapshot;
            copy.tree = copy_tree(e.snapshot.tree, std::string());
            copy.parent.reset();
            if (e.snapshot.parent) {
                const auto parent = copies.find(*e.snapshot.parent);
                if (parent != copies.end()) copy.parent = parent->second;
            }
            copy.copy_of = e.id;
            copy.stats.stored_bytes = 0;  // what the original added; the copy's own size differs
            to_.flush();
            const Id copy_id = to_.save_snapshot(copy);
            copies.emplace(e.id, copy_id);
            ++result_.copied;
            save_map(options_.map_file, map_);  // progress survives a later failure
        }
        save_map(options_.map_file, map_);
        return result_;
    }

private:
    void check_cancel() {
        if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
    }

    // A mapped item the target still has (a prune may have removed it since).
    bool mapped(const Id& from, Id& to) const {
        const auto it = map_.find(from);
        if (it == map_.end() || !to_.has_blob(it->second)) return false;
        to = it->second;
        return true;
    }

    Id copy_blob(BlobType type, const Id& id) {
        Id out{};
        if (mapped(id, out)) return out;
        const Bytes plain = from_.load_blob(type, id);
        if (to_.save_blob(type, plain, out)) {
            ++result_.blobs;
            result_.bytes += plain.size();
        }
        map_[id] = out;
        return out;
    }

    Id copy_tree(const Id& id, const std::string& path) {
        Id out{};
        if (mapped(id, out)) return out;  // the whole folder is already there
        Tree tree = parse_tree(from_.load_blob(BlobType::Tree, id));
        for (Node& node : tree.nodes) {
            check_cancel();
            const std::string child = path.empty() ? node.name : path + "/" + node.name;
            if (node.kind == NodeKind::Dir) {
                node.subtree = copy_tree(node.subtree, child);
            } else if (node.kind == NodeKind::File && !node.content.empty()) {
                if (options_.on_progress) options_.on_progress(label_ + ": " + child);
                for (Id& chunk : node.content) chunk = copy_blob(BlobType::Data, chunk);
            }
            for (DataStream& s : node.streams)
                for (Id& chunk : s.content) chunk = copy_blob(BlobType::Data, chunk);
            if (node.security) node.security = copy_blob(BlobType::Data, *node.security);
        }
        tree.sort();
        const Bytes serialized = serialize_tree(tree);
        if (to_.save_blob(BlobType::Tree, serialized, out)) {
            ++result_.blobs;
            result_.bytes += serialized.size();
        }
        map_[id] = out;
        return out;
    }

    Repository& from_;
    Repository& to_;
    const CopyOptions& options_;
    IdMap map_;
    CopyResult result_;
    std::string label_;
};

}  // namespace

CopyResult copy_snapshots(Repository& from, Repository& to, const CopyOptions& options) {
    Copier c(from, to, options);
    return c.run();
}

}  // namespace vaultik
