#include "vaultik/ops/backup.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>

#include "vaultik/chunker/fastcdc.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

bool wildcard_match(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

class Excluder {
public:
    explicit Excluder(const std::vector<std::string>& patterns) {
        for (std::string p : patterns) {
#ifdef _WIN32
            std::replace(p.begin(), p.end(), '/', '\\');
#endif
            patterns_.push_back(os::fold_case(p));
        }
    }

    bool excluded(std::string_view name, const std::string& full) const {
        if (patterns_.empty()) return false;
        const std::string lname = os::fold_case(name);
        std::string lfull;
        for (const std::string& p : patterns_) {
            if (p.find(os::kSeparator) != std::string::npos) {
                if (lfull.empty()) lfull = os::fold_case(full);
                if (wildcard_match(p, lfull)) return true;
            } else if (wildcard_match(p, lname)) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> patterns_;
};

Node node_from_info(const os::FileInfo& info, std::string name) {
    Node n;
    n.name = std::move(name);
    n.attributes = info.attributes;
    n.mode = info.mode;
    n.uid = info.uid;
    n.gid = info.gid;
    n.ctime = info.ctime;
    n.mtime = info.mtime;
    n.size = info.size;
    return n;
}

// Virtual directories above the backed-up sources (drive letter and parent folders).
struct VirtualDir {
    std::map<std::string, VirtualDir> dirs;
    std::vector<Node> leaves;
};

std::string paths_key(const std::vector<std::string>& paths) {
    std::vector<std::string> folded;
    folded.reserve(paths.size());
    for (const auto& p : paths) folded.push_back(os::fold_case(p));
    std::sort(folded.begin(), folded.end());
    std::string key;
    for (const auto& p : folded) key += p + "\n";
    return key;
}

class BackupEngine {
public:
    BackupEngine(Repository& repo, const BackupOptions& options)
        : repo_(repo), options_(options), excluder_(options.excludes) {}

    BackupResult run();

private:
    struct Source {
        std::string full;                // absolute
        std::vector<std::string> comps;  // tree components
    };

    std::vector<Source> resolve_sources();
    std::optional<Id> find_parent(const std::vector<std::string>& paths);
    std::optional<Tree> load_tree(const Id& id);
    std::optional<Node> lookup(const Tree& root, const std::vector<std::string>& comps);
    Tree scan_children(const std::string& dir, std::uint64_t device, const Tree* parent);
    Id scan_dir(const std::string& dir, std::uint64_t device, const Tree* parent);
    bool scan_entry(const std::string& path, const os::FileInfo& info, std::uint64_t parent_device, Node& node,
                    const Node* pnode);
    bool scan_file(const std::string& path, Node& node, const Node* parent);
    bool all_present(const std::vector<Id>& ids) const;
    Id save_virtual(const VirtualDir& dir);
    void warn(const std::string& message);
    void report(bool force = false);
    void check_cancel();
    // The path to do I/O on for a real path (a shadow copy, or the path itself).
    std::string io(const std::string& path) const { return options_.read_path ? options_.read_path(path) : path; }

    Repository& repo_;
    const BackupOptions& options_;
    Excluder excluder_;
    Bytes buffer_;
    SnapshotStats stats_;
    ChangeAlarm alarm_;
    BackupProgress progress_;

    void suspicious(const std::string& path) {
        if (alarm_.examples.size() < 5) alarm_.examples.push_back(path);
    }
    // Whether the stored chunks of a file's earlier content compressed well (index sizes).
    bool was_compressible(const std::vector<Id>& content) const;
    std::chrono::steady_clock::time_point last_report_{};
};

bool BackupEngine::was_compressible(const std::vector<Id>& content) const {
    std::uint64_t raw = 0, stored = 0;
    for (std::size_t i = 0; i < content.size() && i < 4; ++i) {
        const BlobLocation* loc = repo_.index().find(content[i]);
        if (loc == nullptr) return false;
        raw += loc->raw_len;
        stored += loc->length;
    }
    return raw >= 16384 && stored * 10 < raw * 9;  // saved more than 10%
}

void BackupEngine::warn(const std::string& message) {
    ++stats_.errors;
    if (options_.on_warning) options_.on_warning(message);
}

void BackupEngine::report(bool force) {
    if (!options_.on_progress) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_report_ < std::chrono::milliseconds(200)) return;
    last_report_ = now;
    progress_.files = stats_.files;
    progress_.dirs = stats_.dirs;
    progress_.bytes = stats_.bytes;
    progress_.new_bytes = stats_.new_bytes;
    options_.on_progress(progress_);
}

void BackupEngine::check_cancel() {
    if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
}

std::vector<BackupEngine::Source> BackupEngine::resolve_sources() {
    if (options_.paths.empty()) throw Error("nothing to back up: no paths given");
    std::vector<Source> sources;
    for (const std::string& p : options_.paths) {
        Source s;
        s.full = os::absolute_path(p);
        s.comps = os::path_components(s.full);
        sources.push_back(std::move(s));
    }
    // Reject duplicates and nesting (C:\a together with C:\a\b): one would hide the other.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        for (std::size_t j = 0; j < sources.size(); ++j) {
            if (i == j) continue;
            const auto& a = sources[i].comps;
            const auto& b = sources[j].comps;
            if (a.size() > b.size()) continue;
            bool prefix = true;
            for (std::size_t k = 0; k < a.size() && prefix; ++k) prefix = os::fold_case(a[k]) == os::fold_case(b[k]);
            if (prefix) throw Error("overlapping backup paths: " + sources[i].full + " and " + sources[j].full);
        }
    }
    return sources;
}

std::optional<Id> BackupEngine::find_parent(const std::vector<std::string>& paths) {
    if (options_.parent) return options_.parent;
    if (!options_.use_parent) return std::nullopt;
    const std::string key = paths_key(paths);
    const std::string host = os::hostname();
    std::optional<Id> best;
    std::uint64_t best_time = 0;
    for (const Id& id : repo_.list_snapshot_ids()) {
        Snapshot s;
        try {
            s = repo_.load_snapshot(id);
        } catch (const Error&) {
            continue;  // unreadable snapshots are reported by verify, not here
        }
        if (s.hostname == host && paths_key(s.paths) == key && s.time > best_time) {
            best = id;
            best_time = s.time;
        }
    }
    return best;
}

std::optional<Tree> BackupEngine::load_tree(const Id& id) {
    try {
        return parse_tree(repo_.load_blob(BlobType::Tree, id));
    } catch (const Error& e) {
        // Not fatal: the files are simply read again instead of being reused.
        if (options_.on_warning) options_.on_warning(std::string("parent snapshot tree unreadable: ") + e.what());
        return std::nullopt;
    }
}

std::optional<Node> BackupEngine::lookup(const Tree& root, const std::vector<std::string>& comps) {
    const Tree* tree = &root;
    std::optional<Tree> holder;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        const Node* n = tree->find(comps[i]);
        if (n == nullptr) return std::nullopt;
        if (i + 1 == comps.size()) return *n;
        if (n->kind != NodeKind::Dir) return std::nullopt;
        std::optional<Tree> next = load_tree(n->subtree);
        if (!next) return std::nullopt;
        holder = std::move(next);
        tree = &*holder;
    }
    return std::nullopt;
}

bool BackupEngine::all_present(const std::vector<Id>& ids) const {
    return std::all_of(ids.begin(), ids.end(), [&](const Id& id) { return repo_.has_blob(id); });
}

bool BackupEngine::scan_file(const std::string& path, Node& node, const Node* parent) {
    // Unchanged since the parent snapshot: reuse its chunk list without reading the file.
    if (parent != nullptr && parent->kind == NodeKind::File && parent->size == node.size &&
        parent->mtime == node.mtime && parent->ctime == node.ctime && all_present(parent->content)) {
        node.content = parent->content;
        stats_.bytes += node.size;
        report();
        return true;
    }

    os::ErrorCode err = 0;
    os::File file = os::File::open_source(io(path), err);
    if (!file.valid()) {
        warn("skipped " + path + ": " + os::error_message(err));
        return false;
    }
    const bool is_new = parent == nullptr || parent->kind != NodeKind::File;

    progress_.current = path;
    report();
    ChunkReader reader(
        repo_.chunker(), [&file](std::uint8_t* dst, std::size_t max) { return file.read(dst, max); }, buffer_);

    std::uint64_t total = 0;
    bool random_start = false;  // the file's first chunk looks like encrypted data
    try {
        ByteSpan chunk;
        while (reader.next(chunk)) {
            if (total == 0) random_start = byte_entropy(chunk.first(std::min<std::size_t>(chunk.size(), 65536))) > 7.9;
            Id id;
            if (repo_.save_blob(BlobType::Data, chunk, id)) stats_.new_bytes += chunk.size();
            node.content.push_back(id);
            total += chunk.size();
            stats_.bytes += chunk.size();
            report();
        }
    } catch (const os::OsError& e) {
        warn("skipped " + path + ": " + e.what());
        return false;
    }
    node.size = total;  // the file may have changed size since it was listed
    if (is_new) {
        ++stats_.files_new;
    } else if (node.content != parent->content) {  // a touched but identical file is not a change
        ++stats_.files_changed;
        if (random_start && total >= 16384 && was_compressible(parent->content)) {
            ++alarm_.became_random;
            suspicious(path);
        }
    }
    return true;
}

bool BackupEngine::scan_entry(const std::string& path, const os::FileInfo& info, std::uint64_t parent_device,
                              Node& node, const Node* pnode) {
    switch (info.type) {
        case os::EntryType::Other:
            // Stored as metadata only; following links could loop or leave the backup set.
            node.kind = NodeKind::Reparse;
            node.size = 0;
            ++stats_.skipped;
            return true;
        case os::EntryType::Symlink: {
            node.kind = NodeKind::Symlink;
            node.size = 0;
            const os::ErrorCode err = os::read_symlink(io(path), node.link_target);
            if (err != 0) {
                warn("cannot read link " + path + ": " + os::error_message(err));
                return false;
            }
            ++stats_.files;
            return true;
        }
        case os::EntryType::Dir: {
            node.kind = NodeKind::Dir;
            node.size = 0;
            // Another file system mounted here: skip kernel pseudo file systems such as /proc.
            if (info.device != parent_device && os::is_pseudo_filesystem(io(path))) {
                ++stats_.skipped;
                node.subtree = save_virtual(VirtualDir{});
                return true;
            }
            std::optional<Tree> ptree;
            if (pnode != nullptr && pnode->kind == NodeKind::Dir) ptree = load_tree(pnode->subtree);
            node.subtree = scan_dir(path, info.device, ptree ? &*ptree : nullptr);
            ++stats_.dirs;
            return true;
        }
        case os::EntryType::File:
            node.kind = NodeKind::File;
            if (!scan_file(path, node, pnode)) return false;
            ++stats_.files;
            return true;
    }
    return false;
}

Tree BackupEngine::scan_children(const std::string& dir, std::uint64_t device, const Tree* parent) {
    Tree tree;
    os::DirReader reader(io(dir));
    if (!reader.ok()) {
        warn("cannot list " + dir + ": " + os::error_message(reader.error()));
        return tree;
    }
    std::string name;
    os::FileInfo info;
    while (reader.next(name, info)) {
        check_cancel();
        const std::string child = os::join(dir, name);
        if (excluder_.excluded(name, child)) continue;
        Node node = node_from_info(info, name);
        const Node* pnode = parent != nullptr ? parent->find(node.name) : nullptr;
        if (scan_entry(child, info, device, node, pnode)) tree.nodes.push_back(std::move(node));
    }
    if (!reader.ok()) warn("listing " + dir + " stopped early: " + os::error_message(reader.error()));
    tree.sort();
    if (parent != nullptr) {
        for (const Node& old : parent->nodes)
            if (tree.find(old.name) == nullptr) ++stats_.items_removed;
        // A file gone and a new one with its name plus an extension: "a.docx" -> "a.docx.locked".
        for (const Node& n : tree.nodes) {
            if (n.kind != NodeKind::File || parent->find(n.name) != nullptr) continue;
            const std::size_t dot = n.name.rfind('.');
            if (dot == std::string::npos || dot == 0) continue;
            const std::string before = n.name.substr(0, dot);
            const Node* old = parent->find(before);
            if (old != nullptr && old->kind == NodeKind::File && tree.find(before) == nullptr &&
                before.find('.') != std::string::npos) {
                ++alarm_.renamed;
                suspicious(os::join(dir, n.name));
            }
        }
    }
    return tree;
}

Id BackupEngine::scan_dir(const std::string& dir, std::uint64_t device, const Tree* parent) {
    const Tree tree = scan_children(dir, device, parent);
    Id id;
    repo_.save_blob(BlobType::Tree, serialize_tree(tree), id);
    return id;
}

Id BackupEngine::save_virtual(const VirtualDir& dir) {
    Tree tree;
    for (const auto& [name, child] : dir.dirs) {
        Node n;
        n.name = name;
        n.kind = NodeKind::Dir;
        n.subtree = save_virtual(child);
        tree.nodes.push_back(std::move(n));
    }
    for (const Node& leaf : dir.leaves) tree.nodes.push_back(leaf);
    tree.sort();
    Id id;
    repo_.save_blob(BlobType::Tree, serialize_tree(tree), id);
    return id;
}

BackupResult BackupEngine::run() {
    os::enable_backup_privileges();  // lets an elevated run read files regardless of their ACLs
    const std::vector<Source> sources = resolve_sources();

    Snapshot snap;
    snap.time = os::now();
    snap.hostname = os::hostname();
    snap.username = os::username();
    for (const Source& s : sources) snap.paths.push_back(s.full);
    snap.tags = options_.tags;

    std::optional<Tree> parent_root;
    snap.parent = find_parent(snap.paths);
    if (snap.parent) {
        try {
            const Snapshot ps = repo_.load_snapshot(*snap.parent);
            alarm_.parent_files = ps.stats.files;
            parent_root = load_tree(ps.tree);
        } catch (const Error& e) {
            if (options_.on_warning) options_.on_warning(std::string("parent snapshot unreadable: ") + e.what());
            snap.parent.reset();
        }
    }

    const WriteStats before = repo_.write_stats();
    VirtualDir root;
    for (const Source& s : sources) {
        os::FileInfo info;
        os::ErrorCode err = 0;
        if (!os::stat(io(s.full), info, err)) {
            warn("cannot back up " + s.full + ": " + os::error_message(err));
            continue;
        }
        // A source named explicitly is followed even if it is a link to a folder.
        if (info.type != os::EntryType::Dir && info.type != os::EntryType::File && os::is_directory(io(s.full)))
            info.type = os::EntryType::Dir;

        if (s.comps.empty()) {
            // The file system root ("/"): its entries go directly under the snapshot root.
            if (info.type != os::EntryType::Dir) continue;
            Tree tree = scan_children(s.full, info.device, parent_root ? &*parent_root : nullptr);
            for (Node& n : tree.nodes) root.leaves.push_back(std::move(n));
            ++stats_.dirs;
            continue;
        }

        Node node = node_from_info(info, s.comps.back());
        std::optional<Node> pnode;
        if (parent_root) pnode = lookup(*parent_root, s.comps);
        if (!scan_entry(s.full, info, info.device, node, pnode ? &*pnode : nullptr)) continue;

        VirtualDir* cur = &root;
        for (std::size_t i = 0; i + 1 < s.comps.size(); ++i) cur = &cur->dirs[s.comps[i]];
        cur->leaves.push_back(std::move(node));
    }

    snap.tree = save_virtual(root);
    repo_.flush();
    if (options_.before_save) options_.before_save();
    stats_.stored_bytes = repo_.write_stats().stored_bytes_added - before.stored_bytes_added;
    snap.stats = stats_;
    snap.stats.changes_known = true;
    report(true);

    BackupResult result;
    result.snapshot_id = repo_.save_snapshot(snap);
    result.snapshot = std::move(snap);
    result.alarm = std::move(alarm_);
    return result;
}

}  // namespace

SourceEstimate estimate_sources(const std::vector<std::string>& paths, const std::vector<std::string>& excludes,
                                const std::function<bool()>& cancelled,
                                const std::function<void(const SourceEstimate&)>& progress) {
    const Excluder excluder(excludes);
    SourceEstimate e;
    auto last = std::chrono::steady_clock::now();
    const auto tick = [&] {
        if (cancelled && cancelled()) throw Cancelled();
        const auto now = std::chrono::steady_clock::now();
        if (progress && now - last > std::chrono::milliseconds(200)) {
            last = now;
            progress(e);
        }
    };
    std::vector<std::string> pending;
    for (const std::string& p : paths) {
        const std::string full = os::absolute_path(p);
        os::FileInfo info;
        os::ErrorCode err = 0;
        if (!os::stat(full, info, err)) {
            ++e.unreadable;
            continue;
        }
        if (info.type == os::EntryType::File) {
            ++e.files;
            e.bytes += info.size;
        } else if (info.type == os::EntryType::Dir || os::is_directory(full)) {
            ++e.dirs;
            pending.push_back(full);
        }
    }
    while (!pending.empty()) {
        const std::string dir = std::move(pending.back());
        pending.pop_back();
        os::DirReader reader(dir);
        std::string name;
        os::FileInfo info;
        while (reader.next(name, info)) {
            tick();
            const std::string child = os::join(dir, name);
            if (excluder.excluded(name, child)) continue;
            if (info.type == os::EntryType::Dir) {
                ++e.dirs;
                pending.push_back(child);
            } else if (info.type == os::EntryType::File) {
                ++e.files;
                e.bytes += info.size;
            }
        }
        if (!reader.ok()) ++e.unreadable;
    }
    return e;
}

double byte_entropy(ByteSpan data) {
    if (data.empty()) return 0;
    std::array<std::uint32_t, 256> counts{};
    for (const std::uint8_t b : data) ++counts[b];
    double h = 0;
    const double n = static_cast<double>(data.size());
    for (const std::uint32_t c : counts) {
        if (c == 0) continue;
        const double p = c / n;
        h -= p * std::log2(p);
    }
    return h;
}

BackupResult run_backup(Repository& repo, const BackupOptions& options) {
    BackupEngine engine(repo, options);
    return engine.run();
}

}  // namespace vaultik
