#include "vaultik/ops/copy.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"  // Cancelled
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

constexpr char kMapMagicV1[8] = {'V', 'K', 'C', 'O', 'P', 'Y', '0', '1'};
constexpr char kMapMagic[8] = {'V', 'K', 'C', 'O', 'P', 'Y', '0', '2'};

using IdMap = std::unordered_map<Id, Id, IdHash>;

// Which stored item became which in the target. A mapped data blob stands for itself only and is
// checked against the target's index before use. A mapped folder listing stands for everything
// below it, which only holds while the target has lost nothing since: a prune keeps folder
// listings no version uses in the packs it keeps while it deletes the packs with their files'
// data, and a repair indexes listings whose data is gone. Both replace every index file, so the
// folder listings are trusted only while every index file the target had when the map was saved
// is still there.
struct CopyMap {
    IdMap data;
    IdMap trees;
    std::vector<std::string> index_files;  // the target's, when the map was saved
};

void put_u32(Bytes& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

// The map file (version 2): the magic; the number of index file names, each as a 4-byte length
// and its bytes; the number of folder listing pairs; those pairs (from, to); then the data blob
// pairs up to the end. Version 1 (the magic, then pairs of any kind) is read as data pairs only.
// A damaged or foreign file is ignored.
CopyMap load_map(const std::string& path) {
    CopyMap map;
    if (path.empty()) return map;
    try {
        const Bytes data = os::read_file(path, 1u << 30);
        if (data.size() < 8) return map;
        std::size_t at = 8;
        const auto read_pairs = [&](IdMap& into, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i, at += 64) {
                Id from{}, to{};
                std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at), 32, from.begin());
                std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(at + 32), 32, to.begin());
                into.emplace(from, to);
            }
        };
        if (std::equal(kMapMagicV1, kMapMagicV1 + 8, data.begin())) {
            if ((data.size() - at) % 64 != 0) return map;
            read_pairs(map.data, (data.size() - at) / 64);
            return map;
        }
        if (!std::equal(kMapMagic, kMapMagic + 8, data.begin())) return map;
        const auto u32 = [&]() -> std::uint32_t {
            if (data.size() - at < 4) throw Error("short copy map");
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i) v |= std::uint32_t{data[at + static_cast<std::size_t>(i)]} << (8 * i);
            at += 4;
            return v;
        };
        CopyMap read;
        const std::uint32_t names = u32();
        for (std::uint32_t i = 0; i < names; ++i) {
            const std::uint32_t len = u32();
            if (data.size() - at < len) return map;
            read.index_files.emplace_back(reinterpret_cast<const char*>(data.data() + at), len);
            at += len;
        }
        const std::uint32_t trees = u32();
        if ((data.size() - at) / 64 < trees || (data.size() - at) % 64 != 0) return map;
        read_pairs(read.trees, trees);
        read_pairs(read.data, (data.size() - at) / 64);
        map = std::move(read);
    } catch (const Error&) {
    }
    return map;
}

void save_map(const std::string& path, const CopyMap& map) {
    if (path.empty()) return;
    Bytes data(kMapMagic, kMapMagic + sizeof kMapMagic);
    data.reserve(data.size() + (map.data.size() + map.trees.size()) * 64 + map.index_files.size() * 80 + 8);
    put_u32(data, static_cast<std::uint32_t>(map.index_files.size()));
    for (const std::string& name : map.index_files) {
        put_u32(data, static_cast<std::uint32_t>(name.size()));
        data.insert(data.end(), name.begin(), name.end());
    }
    put_u32(data, static_cast<std::uint32_t>(map.trees.size()));
    for (const IdMap* part : {&map.trees, &map.data})
        for (const auto& [from, to] : *part) {
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

    // Saves the map with the target's index files as they are now (everything copied so far is
    // flushed, so their blobs are in them).
    void save() {
        if (options_.map_file.empty()) return;
        map_.index_files = to_.list_index_files();
        save_map(options_.map_file, map_);
    }

    CopyResult run() {
        // The target lost or rearranged data since the map was saved (prune, repair): its folder
        // listings may stand for data that is gone, so every folder is walked again (each data
        // blob is still checked on its own, so only what is missing is read and stored).
        if (!map_.trees.empty()) {
            const std::vector<std::string> now = to_.list_index_files();
            const std::unordered_set<std::string> present(now.begin(), now.end());
            if (map_.index_files.empty() || !std::all_of(map_.index_files.begin(), map_.index_files.end(),
                                                         [&](const std::string& n) { return present.contains(n); }))
                map_.trees.clear();
        }
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
            copy.apps.reset();
            if (e.snapshot.apps && from_.has_blob(*e.snapshot.apps)) copy.apps = copy_blob(BlobType::Data, *e.snapshot.apps);
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
            save();  // progress survives a later failure
        }
        save();
        return result_;
    }

private:
    void check_cancel() {
        if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
    }

    // A mapped item the target still has (a prune may have removed it since).
    bool mapped(const IdMap& map, const Id& from, Id& to) const {
        const auto it = map.find(from);
        if (it == map.end() || !to_.has_blob(it->second)) return false;
        to = it->second;
        return true;
    }

    Id copy_blob(BlobType type, const Id& id) {
        Id out{};
        if (mapped(map_.data, id, out)) return out;
        const Bytes plain = from_.load_blob(type, id);
        if (to_.save_blob(type, plain, out)) {
            ++result_.blobs;
            result_.bytes += plain.size();
        }
        map_.data[id] = out;
        return out;
    }

    Id copy_tree(const Id& id, const std::string& path) {
        Id out{};
        if (mapped(map_.trees, id, out)) return out;  // the whole folder is already there
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
        map_.trees[id] = out;
        return out;
    }

    Repository& from_;
    Repository& to_;
    const CopyOptions& options_;
    CopyMap map_;
    CopyResult result_;
    std::string label_;
};

}  // namespace

CopyResult copy_snapshots(Repository& from, Repository& to, const CopyOptions& options) {
    Copier c(from, to, options);
    return c.run();
}

}  // namespace vaultik
