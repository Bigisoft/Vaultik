#include "vaultik/ops/repair.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/repo/pack.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {


std::string short_hex(const Id& id) {
    return id_hex(id).substr(0, 8);
}

// Whether a failure means the data is damaged (it failed authentication or its hash, is
// malformed, or is gone from the packs), rather than that it could not be read just now.
bool is_damage(const std::exception& e) {
    if (dynamic_cast<const AuthError*>(&e) != nullptr) return true;
    const std::string what = e.what();
    for (const char* sign : {"malformed", "blob not found", "unsupported tree version", "unsupported snapshot version",
                             "unexpected type", "does not match its id", "pack file too small", "not a pack file",
                             "zstd decompression", "Unknown frame descriptor", "Data corruption detected",
                             "Src size is incorrect", "Destination buffer is too small"})
        if (what.find(sign) != std::string::npos) return true;
    return false;
}

// What became of a folder listing: unreadable, unchanged, or saved again without what was lost.
struct TreeFix {
    bool ok = false;
    bool changed = false;
    Id id{};
    std::uint64_t files_lost = 0;
    std::uint64_t folders_lost = 0;
};

class TreeFixer {
public:
    TreeFixer(Repository& repo, bool dry_run) : repo_(repo), dry_run_(dry_run) {}

    TreeFix fix(const Id& id) {
        if (const auto it = done_.find(id); it != done_.end()) return it->second;
        TreeFix r;
        Tree tree;
        try {
            tree = parse_tree(repo_.load_blob(BlobType::Tree, id));
        } catch (const std::exception& e) {
            // Only damage makes a folder lost; a read that failed now stops the repair (and is not
            // remembered for the other versions sharing the folder).
            if (!is_damage(e))
                throw Error(std::string("repair stopped, nothing was changed: reading failed (") + e.what() +
                            "); try again when the destination can be read");
            return done_[id] = r;  // not ok
        }
        r.ok = true;
        Tree out;
        for (const Node& n : tree.nodes) {
            if (n.kind == NodeKind::Dir) {
                const TreeFix sub = fix(n.subtree);
                if (!sub.ok) {
                    r.changed = true;
                    ++r.folders_lost;
                    continue;
                }
                r.files_lost += sub.files_lost;
                r.folders_lost += sub.folders_lost;
                Node m = keep_intact_extras(n, r);  // a folder's own descriptor may be lost too
                if (sub.changed) {
                    m.subtree = sub.id;
                    r.changed = true;
                }
                out.nodes.push_back(std::move(m));
            } else if (n.kind == NodeKind::File) {
                bool whole = true;
                for (const Id& c : n.content)
                    if (!repo_.has_blob(c)) {
                        whole = false;
                        break;
                    }
                if (!whole) {
                    r.changed = true;
                    ++r.files_lost;
                    continue;
                }
                out.nodes.push_back(keep_intact_extras(n, r));
            } else {
                out.nodes.push_back(keep_intact_extras(n, r));
            }
        }
        r.id = id;
        if (r.changed && !dry_run_) repo_.save_blob(BlobType::Tree, serialize_tree(out), r.id);
        return done_[id] = r;
    }

private:
    // A node without the streams and the security descriptor whose data is gone.
    Node keep_intact_extras(Node n, TreeFix& r) {
        const auto present = [this](const std::vector<Id>& ids) {
            return std::all_of(ids.begin(), ids.end(), [this](const Id& c) { return repo_.has_blob(c); });
        };
        const std::size_t before = n.streams.size();
        std::erase_if(n.streams, [&](const DataStream& s) { return !present(s.content); });
        if (n.streams.size() != before) r.changed = true;
        if (n.security && !repo_.has_blob(*n.security)) {
            n.security.reset();
            r.changed = true;
        }
        return n;
    }

    Repository& repo_;
    bool dry_run_;
    std::unordered_map<Id, TreeFix, IdHash> done_;
};

}  // namespace

RepairResult run_repair(Repository& repo, const RepairOptions& o) {
    auto progress = [&](const std::string& m) {
        if (o.on_progress) o.on_progress(m);
    };
    auto warn = [&](const std::string& m) {
        if (o.on_warning) o.on_warning(m);
    };
    auto cancelled = [&] { return o.should_cancel && o.should_cancel(); };
    // A failure to read (the network, the disk, a server) is not damage: stop, change nothing.
    const auto stop_unless_damage = [](const std::exception& e) {
        if (!is_damage(e))
            throw Error(std::string("repair stopped, nothing was changed: reading failed (") + e.what() +
                        "); try again when the destination can be read");
    };
    Backend& backend = repo.backend();
    RepairResult r;

    // A. Read and judge everything, writing nothing yet.
    // A1. The index, from the packs themselves.
    std::vector<std::string> pack_names = backend.list("data");
    {
        // The packs the old index files list but the listing did not show: each is checked
        // directly, since a short listing would otherwise make all their data lost. Index files
        // that cannot be read are what a repair is for, so they are skipped here.
        std::unordered_set<std::string> listed(pack_names.begin(), pack_names.end());
        std::vector<std::string> indexed;
        std::unordered_set<std::string> indexed_set;
        for (const std::string& name : repo.list_index_files()) {
            IndexFileContent content;
            try {
                content = repo.read_index_file(name);
            } catch (const std::exception& e) {
                stop_unless_damage(e);
                continue;
            }
            for (const auto& entry : content) {
                std::string pack = Repository::pack_name(entry.first);
                if (indexed_set.insert(pack).second) indexed.push_back(std::move(pack));
            }
        }
        const std::unordered_set<std::string> was_listed = listed;
        const std::vector<std::string> missing = confirm_missing_packs(backend, indexed, listed);
        for (const std::string& name : indexed)
            if (listed.contains(name) && !was_listed.contains(name))
                pack_names.push_back(name);  // there after all: read like the others
        std::sort(pack_names.begin(), pack_names.end());
        if (too_many_missing_packs(missing.size(), indexed.size()) && !o.accept_missing_packs)
            throw Error("repair stopped, nothing was changed: " + std::to_string(missing.size()) + " of the " +
                        std::to_string(indexed.size()) + " packs the index lists are missing from " +
                        backend.location() +
                        ". Check that this is the right destination and that it can be read; if the data is really "
                        "gone, run the repair again with --accept-missing-packs");
        r.packs_missing = missing.size();
        for (const std::string& name : missing)
            warn("pack " + name.substr(name.find_last_of('/') + 1, 8) + " is listed in the index but missing; its data is lost");
    }
    IndexFileContent fresh;
    std::unordered_set<Id, IdHash> seen;
    // A pack rebuilt from its repair data is written back at once: it then holds exactly the
    // content its id names, as it did before it was damaged, so this changes nothing any index or
    // version refers to, and the checks below read it as it should be. A dry run keeps it in
    // memory instead.
    // A damaged or unreadable pack, whole again from its repair data (when it has some).
    const auto rebuild = [&](const Id& id, Bytes& pack, bool read_failed) {
        Bytes work = std::move(pack);
        if (read_failed) {
            try {
                work = repo.read_pack_salvaging(id);
            } catch (const std::exception&) {
                return false;
            }
        }
        if (!repo.rebuild_pack(id, work)) {
            pack = std::move(work);
            return false;
        }
        pack = std::move(work);
        if (o.dry_run) {
            repo.use_pack_bytes(id, pack);
        } else {
            repo.write_pack(id, pack);
        }
        ++r.packs_rebuilt;
        warn("pack " + id_hex(id).substr(0, 8) + " was damaged and was rebuilt from its repair data");
        return true;
    };
    std::size_t n = 0;
    for (const std::string& name : pack_names) {
        if (cancelled()) throw Cancelled();
        ++n;
        progress("pack " + std::to_string(n) + "/" + std::to_string(pack_names.size()));
        const std::string hex = name.substr(name.find_last_of('/') + 1);
        if (hex.size() != 64) continue;  // not a pack (a temporary file left by a crash)
        ++r.packs;
        Bytes pack;
        std::vector<BlobEntry> entries;
        const Id pack_id = id_from_hex(hex);
        try {
            if (o.quick) {
                try {
                    entries = read_pack_header(backend, name, repo.aead());
                } catch (const std::exception&) {
                    // A damaged header: the whole pack, rebuilt if it can be.
                    bool failed = false;
                    try {
                        pack = backend.get(name);
                    } catch (const std::exception&) {
                        failed = true;
                    }
                    if (!rebuild(pack_id, pack, failed)) throw;
                    entries = parse_pack_header(pack, repo.aead());
                }
            } else {
                std::exception_ptr read_error;
                try {
                    pack = backend.get(name);
                } catch (const std::exception&) {
                    read_error = std::current_exception();
                }
                if (read_error || blake3(pack) != pack_id) {
                    if (!rebuild(pack_id, pack, read_error != nullptr) && read_error)
                        std::rethrow_exception(read_error);
                }
                entries = parse_pack_header(pack, repo.aead());
            }
        } catch (const std::exception& e) {
            stop_unless_damage(e);
            ++r.packs_unreadable;
            warn("pack " + hex.substr(0, 8) + " is damaged, its data is lost: " + e.what());
            continue;
        }
        std::vector<BlobEntry> good;
        for (const BlobEntry& e : entries) {
            if (seen.contains(e.id)) continue;  // the same blob stored twice: the first copy serves
            if (!o.quick) {
                try {
                    if (e.offset + e.length > pack.size()) throw Error("malformed pack: a blob lies outside it");
                    repo.decode_blob(e.type, e.id, ByteSpan(pack.data() + e.offset, e.length));
                } catch (const std::exception& ex) {
                    stop_unless_damage(ex);
                    ++r.blobs_damaged;
                    warn("damaged data in pack " + hex.substr(0, 8) + " (" + short_hex(e.id) + "): " + ex.what());
                    continue;
                }
            }
            seen.insert(e.id);
            good.push_back(e);
        }
        r.blobs += good.size();
        if (!good.empty()) fresh.emplace_back(pack_id, std::move(good));
    }
    repo.clear_index();
    for (const auto& [pack_id, entries] : fresh) repo.index_pack(pack_id, entries);

    // A2. The versions, against that index.
    progress("checking every version");
    struct Plan {
        Id id{};
        Snapshot snapshot;
        bool remove = false;  // else: save again without what was lost
        std::string lost;
    };
    std::vector<Plan> plans;
    {
        TreeFixer judge(repo, true);
        for (const Id& sid : repo.list_snapshot_ids()) {
            if (cancelled()) throw Cancelled();
            ++r.snapshots;
            Plan p;
            p.id = sid;
            try {
                p.snapshot = repo.load_snapshot(sid);
            } catch (const std::exception& e) {
                stop_unless_damage(e);
                p.remove = true;
                ++r.snapshots_removed;
                warn("version " + short_hex(sid) + " cannot be read and is removed: " + e.what());
                plans.push_back(std::move(p));
                continue;
            }
            const TreeFix root = judge.fix(p.snapshot.tree);
            if (!root.ok) {
                p.remove = true;
                ++r.snapshots_removed;
                warn("version " + short_hex(sid) + " lost its top folder listing and is removed");
                plans.push_back(std::move(p));
                continue;
            }
            const bool apps_lost = p.snapshot.apps && !repo.has_blob(*p.snapshot.apps);
            if (!root.changed && !apps_lost) continue;
            ++r.snapshots_repaired;
            r.files_lost += root.files_lost;
            r.folders_lost += root.folders_lost;
            if (root.changed)
                p.lost = std::to_string(root.files_lost) + " file(s) and " + std::to_string(root.folders_lost) +
                         " folder(s) were lost";
            if (apps_lost) p.lost += std::string(p.lost.empty() ? "" : "; ") + "the program list was lost";
            warn("version " + short_hex(sid) + ": " + p.lost + "; saved again without them");
            plans.push_back(std::move(p));
        }
    }
    const std::vector<std::string> old_index = repo.list_index_files();
    r.index_files_replaced = old_index.size();
    if (o.dry_run) return r;

    // B. Change: new index files first (a crash in between leaves both; the old ones only add
    // blobs listed again, or damaged ones a later repair drops again), then the old ones go, then
    // the versions. A failure now leaves an index that matches the packs; versions not reached
    // yet keep pointing at what was lost, and the next repair continues.
    progress("writing the new index");
    for (std::size_t i = 0; i < fresh.size(); i += kPacksPerIndexFile) {
        const auto end = fresh.begin() + static_cast<std::ptrdiff_t>(std::min(fresh.size(), i + kPacksPerIndexFile));
        repo.write_index_file(IndexFileContent(fresh.begin() + static_cast<std::ptrdiff_t>(i), end));
    }
    for (const std::string& name : old_index) backend.remove(name);

    TreeFixer fixer(repo, false);
    for (const Plan& p : plans) {
        if (cancelled()) throw Cancelled();
        if (p.remove) {
            backend.remove(Repository::snapshot_name(p.id));
            continue;
        }
        const TreeFix root = fixer.fix(p.snapshot.tree);
        if (!root.ok) continue;  // (judged before; not removed now without a fresh judgement)
        Snapshot fixed = p.snapshot;
        fixed.tree = root.id;
        if (fixed.apps && !repo.has_blob(*fixed.apps)) fixed.apps.reset();
        fixed.note = (p.snapshot.note.empty() ? std::string() : p.snapshot.note + " ") + "(repaired: " + p.lost + ")";
        repo.save_snapshot(fixed);  // flushes the new folder listings first
        backend.remove(Repository::snapshot_name(p.id));
    }
    repo.flush();
    return r;
}

}  // namespace vaultik
