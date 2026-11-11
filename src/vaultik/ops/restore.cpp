#include "vaultik/ops/restore.hpp"

#include <cctype>
#include <chrono>
#include <optional>

#include "vaultik/ops/backup.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik {

// ERROR_STOPPED_ON_SYMLINK: what the platform layer reports for a link where a real item belongs.
constexpr os::ErrorCode kStoppedOnLink = 1281;

namespace {

std::vector<std::string> split_snapshot_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) parts.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(std::move(cur));
    return parts;
}

// Type of what is at `path` without following links; nothing when there is nothing.
std::optional<os::EntryType> entry_type(const std::string& path) {
    os::FileInfo info;
    os::ErrorCode err = 0;
    if (!os::stat(path, info, err)) return std::nullopt;
    return info.type;
}

os::FileInfo info_of(const Node& node) {
    os::FileInfo info;
    switch (node.kind) {
        case NodeKind::Dir: info.type = os::EntryType::Dir; break;
        case NodeKind::Symlink: info.type = os::EntryType::Symlink; break;
        case NodeKind::File: info.type = os::EntryType::File; break;
        case NodeKind::Reparse: info.type = os::EntryType::Other; break;
    }
    info.attributes = node.attributes;
    info.mode = node.mode;
    info.uid = node.uid;
    info.gid = node.gid;
    info.ctime = node.ctime;
    info.mtime = node.mtime;
    info.size = node.size;
    return info;
}

class RestoreEngine {
public:
    RestoreEngine(Repository& repo, const RestoreOptions& options) : repo_(repo), options_(options) {}

    RestoreResult run(const Snapshot& snapshot);

private:
    void restore_node(const Node& node, const std::string& parent_dir);
    void restore_children(const Id& tree_id, const std::string& dir);
    void restore_file(const Node& node, const std::string& path);
    void restore_symlink(const Node& node, const std::string& path);
    // `id` is the restored item as created; the writes are refused when the path now names
    // anything else (see os::write_security).
    void restore_streams(const Node& node, const std::string& path, const std::optional<os::FileId>& id);
    void restore_security(const Node& node, const std::string& path, const std::optional<os::FileId>& id);
    void restore_link(const Node& node, const std::string& path);
    void ensure_dir(const std::string& path);
    void ensure_dir_chain(const std::string& path);
    // Whether a real path (File::final_path) lies in the target folder; true when it cannot be
    // told. Every item is checked once made, so nothing is written where a link led it.
    bool inside(const std::string& final) const;
    // Deletes what is at `path` (a file or link, never a folder's content) for an overwrite,
    // through a handle on exactly that item, once it is known to be in the target folder.
    // Whether nothing is there now.
    bool remove_existing(const std::string& path);
    void warn(const std::string& message);
    void report();
    void check_cancel();

    Repository& repo_;
    const RestoreOptions& options_;
    RestoreResult result_;
    std::uint64_t kept_permissions_ = 0;  // existing folders whose stored permissions were not applied
    std::string root_;                    // the target's real path, for inside()
    RestoreProgress progress_;
    std::chrono::steady_clock::time_point last_report_{};
};

bool RestoreEngine::inside(const std::string& final) const {
    if (final.empty() || root_.empty()) return true;
    const auto lower = [](std::string t) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    const std::string f = lower(final);
    const std::string r = lower(root_);
    if (f.compare(0, r.size(), r) != 0) return false;
    return f.size() == r.size() || f[r.size()] == '\\' || f[r.size()] == '/' || r.back() == '\\' || r.back() == '/';
}

bool RestoreEngine::remove_existing(const std::string& path) {
    os::ErrorCode err = 0;
    os::File item = os::File::open_for_delete(path, err);
    if (!item.valid()) {
        if (os::is_not_supported(err)) {
            os::make_writable(path);
            os::remove_file(path);
            return !entry_type(path);
        }
        return !entry_type(path);
    }
    if (!inside(item.final_path())) {
        warn("skipped " + path + ": it leads outside the folder restored into");
        return false;
    }
    const os::ErrorCode e = item.discard();
    item.close();
    return e == 0;
}

void RestoreEngine::warn(const std::string& message) {
    ++result_.errors;
    if (options_.on_warning) options_.on_warning(message);
}

void RestoreEngine::report() {
    if (!options_.on_progress) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_report_ < std::chrono::milliseconds(200)) return;
    last_report_ = now;
    progress_.files = result_.files;
    progress_.bytes = result_.bytes;
    options_.on_progress(progress_);
}

void RestoreEngine::check_cancel() {
    if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
}

void RestoreEngine::ensure_dir(const std::string& path) {
    const os::ErrorCode err = os::make_dir(path);
    if (err == 0) return;
    if (os::is_exists(err)) {
        // Must be a real folder: a link to a folder elsewhere would let the restore write there.
        if (entry_type(path) == os::EntryType::Dir) return;
        throw Error("cannot create folder " + path + ": a file or link with that name exists");
    }
    os::throw_error("cannot create folder " + path, err);
}

void RestoreEngine::ensure_dir_chain(const std::string& path) {
    if (os::exists(path)) {
        if (!os::is_directory(path)) throw Error("restore target is a file: " + path);
        return;
    }
    const std::string parent = os::parent_of(path);
    if (parent.empty()) throw Error("restore target does not exist: " + path);
    ensure_dir_chain(parent);
    ensure_dir(path);
}

void RestoreEngine::restore_file(const Node& node, const std::string& path) {
    if (options_.overwrite) {
        // Replace only regular files. A link is removed rather than written through (it could
        // point anywhere); a folder is left alone.
        const std::optional<os::EntryType> existing = entry_type(path);
        if (existing && *existing == os::EntryType::Dir) {
            warn("cannot replace " + path + ": a folder with that name exists");
            return;
        }
        if (existing && !remove_existing(path)) {
            warn("cannot replace " + path);
            return;
        }
    }
    os::ErrorCode err = 0;
    // Private until the content and final permissions are in place; snapshots from Windows carry
    // no permissions, so those get the umask default.
    os::File file = os::File::create(path, options_.overwrite, err, node.mode != 0 ? 0600 : 0666);
    if (file.valid() && !inside(file.final_path())) {
        file.discard();
        file.close();
        warn("skipped " + path + ": a link led it outside the folder restored into");
        return;
    }
    if (!file.valid()) {
        if (os::is_exists(err)) {
            ++result_.skipped;
            if (options_.on_warning) options_.on_warning("exists, not overwritten: " + path);
            return;
        }
        warn("cannot create " + path + ": " + os::error_message(err));
        return;
    }
    file.preallocate(node.size);

    progress_.current = path;
    std::uint64_t written = 0;
    try {
        for (const Id& chunk_id : node.content) {
            const Bytes data = repo_.load_blob(BlobType::Data, chunk_id);
            file.write_all(data);
            written += data.size();
            result_.bytes += data.size();
            report();
        }
        if (written != node.size) throw Error("restored size does not match the snapshot");
    } catch (const ArchivedError&) {
        // Every file after it would fail the same way: stop with the error that says what to do.
        file.close();
        os::remove_file(path);
        throw;
    } catch (const Error& e) {
        file.close();
        os::remove_file(path);
        warn(std::string("failed to restore ") + path + ": " + e.what());
        return;
    }
    const std::optional<os::FileId> id = file.id();
    file.close();
    restore_streams(node, path, id);
    os::set_metadata(path, info_of(node));
    restore_security(node, path, id);
    ++result_.files;
    report();
}

void RestoreEngine::restore_streams(const Node& node, const std::string& path, const std::optional<os::FileId>& id) {
    if (!node.streams.empty() && !id) {
        warn("cannot restore the streams of " + path + ": the restored file cannot be identified");
        return;
    }
    for (const DataStream& s : node.streams) {
        if (s.name.empty() || s.name.find_first_of(":/\\") != std::string::npos) {
            warn("skipped a stream with an invalid name on " + path);
            continue;
        }
        os::ErrorCode err = 0;
        os::File f = os::File::create_stream(path, s.name, *id, err);
        if (!f.valid()) {
            warn("cannot restore the stream " + s.name + " of " + path + ": " + os::error_message(err));
            continue;
        }
        try {
            std::uint64_t written = 0;
            for (const Id& chunk : s.content) {
                const Bytes data = repo_.load_blob(BlobType::Data, chunk);
                f.write_all(data);
                written += data.size();
            }
            if (written != s.size) throw Error("restored size does not match the snapshot");
        } catch (const ArchivedError&) {
            throw;
        } catch (const Error& e) {
            warn("failed to restore the stream " + s.name + " of " + path + ": " + e.what());
        }
    }
}

void RestoreEngine::restore_security(const Node& node, const std::string& path, const std::optional<os::FileId>& id) {
    if (!options_.permissions || !node.security) return;
    if (!id) {
        warn("cannot restore the permissions of " + path + ": the restored item cannot be identified");
        return;
    }
    try {
        const Bytes sd = repo_.load_blob(BlobType::Data, *node.security);
        const os::ErrorCode err = os::write_security(path, sd, &*id);
        if (err != 0) warn("cannot restore the permissions of " + path + ": " + os::error_message(err));
    } catch (const ArchivedError&) {
        throw;
    } catch (const Error& e) {
        warn("cannot restore the permissions of " + path + ": " + e.what());
    }
}

void RestoreEngine::restore_link(const Node& node, const std::string& path) {
    if (!options_.links || !node.link) {
        ++result_.skipped;  // placeholders and other reparse points carry no content
        return;
    }
    if (options_.overwrite) {
        const std::optional<os::EntryType> existing = entry_type(path);
        if (existing && *existing != os::EntryType::Dir) remove_existing(path);
    }
    constexpr std::uint32_t kDirectoryAttribute = 0x10;
    const os::ReparseLink link{node.link->tag, node.link->target, node.link->relative};
    const os::ErrorCode err = os::create_link(path, link, (node.attributes & kDirectoryAttribute) != 0);
    if (err != 0) {
        if (os::is_exists(err)) {
            ++result_.skipped;
            if (options_.on_warning) options_.on_warning("exists, not overwritten: " + path);
            return;
        }
        warn("cannot create link " + path + " -> " + node.link->target + ": " + os::error_message(err));
        return;
    }
    ++result_.files;
}

void RestoreEngine::restore_symlink(const Node& node, const std::string& path) {
    if (options_.overwrite) {
        const std::optional<os::EntryType> existing = entry_type(path);
        if (existing && *existing != os::EntryType::Dir) remove_existing(path);
    }
    const os::ErrorCode err = os::make_symlink(node.link_target, path);
    if (err != 0) {
        if (os::is_exists(err)) {
            ++result_.skipped;
            if (options_.on_warning) options_.on_warning("exists, not overwritten: " + path);
            return;
        }
        warn("cannot create link " + path + " -> " + node.link_target + ": " + os::error_message(err));
        return;
    }
    os::set_metadata(path, info_of(node));
    ++result_.files;
}

void RestoreEngine::restore_children(const Id& tree_id, const std::string& dir) {
    Tree tree;
    try {
        tree = parse_tree(repo_.load_blob(BlobType::Tree, tree_id));
    } catch (const ArchivedError&) {
        throw;
    } catch (const Error& e) {
        warn("cannot read folder listing for " + dir + ": " + e.what());
        return;
    }
    for (const Node& child : tree.nodes) restore_node(child, dir);
}

void RestoreEngine::restore_node(const Node& node, const std::string& parent_dir) {
    check_cancel();
    // Names come from an authenticated snapshot, but never let one escape the target folder.
    if (!is_safe_name(node.name)) {
        warn("skipped name that is not valid here: " + node.name);
        return;
    }
    const std::string path = os::join(parent_dir, node.name);
    switch (node.kind) {
        case NodeKind::Dir: {
            // A new folder is made and kept open, so its permissions go on exactly that folder.
            // A folder that is there already keeps its own: it is not this restore's to change,
            // and its path could lead somewhere else by now.
            os::ErrorCode err = 0;
            os::File made = os::File::create_directory(path, err);
            if (made.valid() && !inside(made.final_path())) {
                made.discard();
                warn("skipped " + path + ": a link led it outside the folder restored into");
                return;
            }
            os::File existing;
            if (!made.valid()) {
                // Only a folder that is there already (or a system without the call) goes on
                // without a handle; never make the folder some other way without its permissions.
                if (!os::is_exists(err) && !os::is_not_supported(err)) {
                    warn("cannot create folder " + path + ": " + os::error_message(err));
                    return;
                }
                try {
                    ensure_dir(path);
                } catch (const ArchivedError&) {
                    throw;
                } catch (const Error& e) {
                    warn(e.what());
                    return;
                }
                if (os::is_exists(err) && options_.permissions && node.security) ++kept_permissions_;
                // Keep the existing folder from being swapped for a link while its content is
                // restored (an elevated restore must not be led into another folder).
                os::ErrorCode held_err = 0;
                existing = os::File::hold_directory(path, true, held_err);
                if (existing.valid() && !inside(existing.final_path())) {
                    warn("skipped " + path + ": it leads outside the folder restored into");
                    return;
                }
                if (!existing.valid() && !os::is_not_supported(held_err)) {
                    warn("skipped " + path + ": " + (held_err == kStoppedOnLink
                                                          ? std::string("a link or junction is in the folder's place")
                                                          : os::error_message(held_err)));
                    return;
                }
            }
            restore_children(node.subtree, path);
            os::set_metadata(path, info_of(node));  // after the children, so times and permissions stick
            if (made.valid() && options_.permissions && node.security) {
                try {
                    const os::ErrorCode e = made.set_security(repo_.load_blob(BlobType::Data, *node.security));
                    if (e != 0) warn("cannot restore the permissions of " + path + ": " + os::error_message(e));
                } catch (const ArchivedError&) {
                    throw;
                } catch (const Error& e) {
                    warn("cannot restore the permissions of " + path + ": " + e.what());
                }
            }
            ++result_.dirs;
            break;
        }
        case NodeKind::File:
            restore_file(node, path);
            break;
        case NodeKind::Symlink:
            restore_symlink(node, path);
            break;
        case NodeKind::Reparse:
            restore_link(node, path);
            break;
    }
}

RestoreResult RestoreEngine::run(const Snapshot& snapshot) {
    os::enable_restore_privileges();
    Node start;
    if (!resolve_snapshot_path(repo_, snapshot, options_.subpath, start))
        throw Error("path not found in snapshot: " + options_.subpath);

    std::string root = os::absolute_path(options_.target);
    ensure_dir_chain(root);
    // Work under the target's real path, with any link or junction on the way resolved now, and
    // hold every folder of it open from the top down, so none can be renamed, replaced by a link
    // or repointed during the restore. Every one of them must be a real folder.
    if (const std::string resolved = os::final_path(root); !resolved.empty()) {
        // With administrator rights, a link or junction where the user typed a folder could send
        // the restore anywhere: only the folder itself is accepted then.
        const auto same = [](const std::string& a, const std::string& b) {
            const auto trim = [](std::string t) {
                while (t.size() > 3 && (t.back() == '\\' || t.back() == '/')) t.pop_back();
                return t;
            };
            const std::string x = trim(a);
            const std::string y = trim(b);
            return x.size() == y.size() && std::equal(x.begin(), x.end(), y.begin(), [](char p, char q) {
                       return std::tolower(static_cast<unsigned char>(p)) == std::tolower(static_cast<unsigned char>(q));
                   });
        };
        if (!same(resolved, root) && os::process_elevated())
            throw Error("cannot restore into " + root + " with administrator rights: it leads to " + resolved +
                        " through a link or junction; choose " + resolved + " itself");
        root = resolved;
        root_ = resolved;
    }
    std::vector<os::File> held;
    {
        std::vector<std::string> chain;
        for (std::string p = root; !p.empty(); p = os::parent_of(p)) {
            chain.push_back(p);
            if (os::parent_of(p) == p) break;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            os::ErrorCode err = 0;
            const bool top = os::parent_of(*it) == *it || os::parent_of(*it).empty();  // a drive or share root
            os::File f = os::File::hold_directory(*it, !top, err);
            if (os::is_not_supported(err)) break;
            if (!f.valid() && !top)
                throw Error("cannot restore into " + root + ": " +
                            (err == kStoppedOnLink ? *it + " is a link or junction, not a folder"
                                                   : os::error_message(err)));
            if (f.valid()) held.push_back(std::move(f));
        }
    }

    if (options_.subpath.empty() || (options_.contents_only && start.kind == NodeKind::Dir)) {
        // The snapshot root is virtual, and with contents_only the folder's own place is `root`.
        restore_children(start.subtree, root);
    } else {
        restore_node(start, root);
    }
    report();
    if (kept_permissions_ > 0 && options_.on_warning)
        options_.on_warning(std::to_string(kept_permissions_) +
                            " folder(s) existed already and kept their own permissions (only folders the "
                            "restore creates get the stored ones)");
    return result_;
}

}  // namespace

bool resolve_snapshot_path(Repository& repo, const Snapshot& snapshot, const std::string& path, Node& out) {
    Node cur;
    cur.kind = NodeKind::Dir;
    cur.subtree = snapshot.tree;
    for (const std::string& part : split_snapshot_path(path)) {
        if (cur.kind != NodeKind::Dir) return false;
        const Tree tree = parse_tree(repo.load_blob(BlobType::Tree, cur.subtree));
        const Node* n = tree.find(part);
        // Drive letters are stored upper case; accept "c" for "C".
        if (n == nullptr && part.size() == 1)
            n = tree.find(std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])))));
        if (n == nullptr) return false;
        cur = *n;
    }
    out = std::move(cur);
    return true;
}

std::string restored_path(const std::string& target, const std::string& snapshot_path) {
    std::string out = os::absolute_path(target);
    for (const std::string& c : os::path_components(os::absolute_path(snapshot_path))) out = os::join(out, c);
    return out;
}

RestoreResult run_restore(Repository& repo, const Snapshot& snapshot, const RestoreOptions& options) {
    RestoreEngine engine(repo, options);
    return engine.run(snapshot);
}

}  // namespace vaultik
