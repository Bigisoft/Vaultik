#include "vaultik/ops/prune.hpp"

#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "vaultik/model/tree.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long long days_from_civil(int y, int m, int d) {
    y -= m <= 2 ? 1 : 0;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// Period keys in local time, from "YYYY-MM-DD HH:MM:SS".
struct Periods {
    std::string hour, day, week, month, year;
};

Periods periods_of(std::uint64_t time) {
    const std::string t = os::format_time_local(time);
    Periods p;
    if (t.size() < 13) return p;  // no valid time: every rule sees one shared bucket
    p.year = t.substr(0, 4);
    p.month = t.substr(0, 7);
    p.day = t.substr(0, 10);
    p.hour = t.substr(0, 13);
    const long long days = days_from_civil(std::stoi(t.substr(0, 4)), std::stoi(t.substr(5, 2)), std::stoi(t.substr(8, 2)));
    // 1970-01-01 was a Thursday; +3 makes weeks start on Monday.
    const long long week = (days + 3 >= 0) ? (days + 3) / 7 : (days + 3 - 6) / 7;
    p.week = std::to_string(week);
    return p;
}

std::string group_key(const Snapshot& s) {
    for (const std::string& tag : s.tags)
        if (tag.starts_with("job:")) return tag;
    std::vector<std::string> paths;
    for (const std::string& p : s.paths) paths.push_back(os::fold_case(p));
    std::sort(paths.begin(), paths.end());
    std::string key = "host:" + s.hostname;
    for (const std::string& p : paths) key += "\n" + p;
    return key;
}

std::string short_hex(const Id& id) {
    return id_hex(id).substr(0, 8);
}

}  // namespace

std::string describe(const RetentionPolicy& p) {
    std::string s;
    auto add = [&](int n, const char* what) {
        if (n <= 0) return;
        s += (s.empty() ? "" : ", ") + std::to_string(n) + " " + what;
    };
    if (p.last > 0) s = "last " + std::to_string(p.last);
    add(p.hourly, "hourly");
    add(p.daily, "daily");
    add(p.weekly, "weekly");
    add(p.monthly, "monthly");
    add(p.yearly, "yearly");
    return s.empty() ? "keep all" : s;
}

std::vector<ForgetDecision> plan_forget(std::vector<SnapshotRecord> snapshots, const RetentionPolicy& policy) {
    std::map<std::string, std::vector<SnapshotRecord>> groups;
    for (SnapshotRecord& e : snapshots) groups[group_key(e.snapshot)].push_back(std::move(e));

    struct Rule {
        int count;
        const char* name;
        std::string Periods::*key;  // nullptr: every snapshot is its own bucket ("last")
    };
    const Rule rules[] = {
        {policy.last, "last", nullptr},          {policy.hourly, "hourly", &Periods::hour},
        {policy.daily, "daily", &Periods::day},  {policy.weekly, "weekly", &Periods::week},
        {policy.monthly, "monthly", &Periods::month}, {policy.yearly, "yearly", &Periods::year},
    };

    std::vector<ForgetDecision> out;
    for (auto& [key, group] : groups) {
        std::sort(group.begin(), group.end(), [](const SnapshotRecord& a, const SnapshotRecord& b) {
            return a.snapshot.time > b.snapshot.time;
        });
        std::vector<Periods> periods;
        periods.reserve(group.size());
        for (const SnapshotRecord& e : group) periods.push_back(periods_of(e.snapshot.time));

        std::vector<ForgetDecision> decisions(group.size());
        for (const Rule& rule : rules) {
            if (rule.count <= 0) continue;
            int kept = 0;
            std::string last_bucket;
            bool first = true;
            for (std::size_t i = 0; i < group.size() && kept < rule.count; ++i) {
                const std::string bucket = rule.key ? periods[i].*rule.key : std::to_string(i);
                if (!first && bucket == last_bucket) continue;
                first = false;
                last_bucket = bucket;
                ++kept;
                decisions[i].keep = true;
                decisions[i].reason += (decisions[i].reason.empty() ? "" : ", ") + std::string(rule.name);
            }
        }
        for (std::size_t i = 0; i < group.size(); ++i) {
            decisions[i].entry = std::move(group[i]);
            out.push_back(std::move(decisions[i]));
        }
    }
    return out;
}

ForgetResult run_forget(Repository& repo, const ForgetOptions& options) {
    if (options.policy.empty())
        throw Error("no retention rule given (for example --keep-daily 7); refusing to remove every snapshot");
    std::vector<SnapshotRecord> candidates;
    for (const Id& id : repo.list_snapshot_ids()) {
        SnapshotRecord e;
        e.id = id;
        try {
            e.snapshot = repo.load_snapshot(id);
        } catch (const Error&) {
            continue;  // never remove what cannot be read; verify reports it
        }
        const bool tagged = std::all_of(options.tags.begin(), options.tags.end(), [&](const std::string& t) {
            return std::find(e.snapshot.tags.begin(), e.snapshot.tags.end(), t) != e.snapshot.tags.end();
        });
        if (tagged) candidates.push_back(std::move(e));
    }

    ForgetResult result;
    result.decisions = plan_forget(std::move(candidates), options.policy);
    for (const ForgetDecision& d : result.decisions) {
        if (d.keep) continue;
        if (!options.dry_run) repo.backend().remove(Repository::snapshot_name(d.entry.id));
        ++result.removed;
    }
    return result;
}

PruneResult run_prune(Repository& repo, const PruneOptions& o) {
    auto progress = [&](const std::string& m) {
        if (o.on_progress) o.on_progress(m);
    };
    auto warn = [&](const std::string& m) {
        if (o.on_warning) o.on_warning(m);
    };
    Backend& backend = repo.backend();
    PruneResult r;

    // 1. Every pack with its blobs, from the index files (a pack can appear in more than one).
    progress("reading the index");
    repo.reload_index();
    const std::vector<std::string> old_index_files = repo.list_index_files();
    std::map<Id, std::vector<BlobEntry>> packs;
    for (const std::string& name : old_index_files) {
        for (auto& [pack_id, entries] : repo.read_index_file(name)) {
            std::vector<BlobEntry>& list = packs[pack_id];
            for (BlobEntry& e : entries) {
                const bool seen = std::any_of(list.begin(), list.end(), [&](const BlobEntry& x) { return x.id == e.id; });
                if (!seen) list.push_back(e);
            }
        }
    }
    std::unordered_set<std::string> stored;
    for (const std::string& name : backend.list("data")) stored.insert(name);

    // Packs no index lists: normally left by cancelled or failed backups (no run holds a lock
    // now), but an index file may also have been lost. Each pack describes itself, so read its
    // header and make its blobs loadable; step 3 decides whether it is still needed.
    std::vector<std::pair<std::string, std::vector<BlobEntry>>> unindexed;
    std::unordered_set<std::string> indexed;
    for (const auto& entry : packs) indexed.insert(Repository::pack_name(entry.first));
    for (const std::string& name : stored) {
        if (indexed.contains(name)) continue;
        std::vector<BlobEntry> entries;
        try {
            entries = read_pack_header(backend, name, repo.aead());
        } catch (const Error& e) {
            warn("cannot read unindexed pack " + name + ", left in place: " + e.what());
            continue;
        }
        const std::string hex = name.substr(name.find_last_of('/') + 1);
        repo.index_pack(id_from_hex(hex), entries);
        unindexed.emplace_back(name, std::move(entries));
    }

    // 2. Everything any snapshot still needs. Shared folders are walked once.
    progress("finding the data still in use");
    std::unordered_set<Id, IdHash> used;
    std::vector<Id> pending;
    for (const Id& sid : repo.list_snapshot_ids()) {
        Snapshot s;
        try {
            s = repo.load_snapshot(sid);
        } catch (const Error& e) {
            throw Error("cannot read snapshot " + short_hex(sid) + ": " + e.what() +
                        "; nothing was deleted (run vaultik verify)");
        }
        if (used.insert(s.tree).second) pending.push_back(s.tree);
    }
    while (!pending.empty()) {
        const Id tree_id = pending.back();
        pending.pop_back();
        Tree tree;
        try {
            tree = parse_tree(repo.load_blob(BlobType::Tree, tree_id));
        } catch (const Error& e) {
            throw Error("cannot read folder listing " + short_hex(tree_id) + ": " + e.what() +
                        "; nothing was deleted (run vaultik verify)");
        }
        for (const Node& n : tree.nodes) {
            if (n.kind == NodeKind::Dir) {
                if (used.insert(n.subtree).second) pending.push_back(n.subtree);
            } else if (n.kind == NodeKind::File) {
                used.insert(n.content.begin(), n.content.end());
            }
        }
    }

    // 3. Keep, rewrite or delete each pack. A blob stored twice is kept in the first pack only.
    std::unordered_set<Id, IdHash> claimed;
    std::vector<Id> to_delete;
    std::vector<std::pair<Id, std::vector<BlobEntry>>> to_repack;
    IndexFileContent kept;
    for (auto& [pack_id, entries] : packs) {
        const std::string name = Repository::pack_name(pack_id);
        if (!stored.contains(name)) {
            warn("pack " + short_hex(pack_id) + " is listed in the index but missing; it is dropped from the index");
            continue;
        }
        ++r.packs;
        const std::uint64_t size = backend.size(name);
        r.bytes_before += size;
        std::vector<BlobEntry> live;
        std::uint64_t dead_bytes = 0;  // blobs nothing uses; the pack header is not counted
        for (const BlobEntry& e : entries) {
            if (used.contains(e.id) && claimed.insert(e.id).second) {
                live.push_back(e);
            } else {
                dead_bytes += e.length;
            }
        }
        r.blobs_used += live.size();
        r.blobs_unused += entries.size() - live.size();
        if (live.empty()) {
            to_delete.push_back(pack_id);
            r.bytes_deleted += size;
        } else if (static_cast<double>(dead_bytes) > o.repack_unused * static_cast<double>(size)) {
            to_repack.emplace_back(pack_id, std::move(live));
            r.bytes_deleted += size;
        } else {
            kept.emplace_back(pack_id, std::move(entries));
        }
    }
    std::vector<std::string> orphans;
    for (auto& [name, entries] : unindexed) {
        const std::uint64_t size = backend.size(name);
        r.bytes_before += size;
        ++r.packs;
        const bool needed = std::any_of(entries.begin(), entries.end(),
                                        [&](const BlobEntry& e) { return used.contains(e.id) && !claimed.contains(e.id); });
        if (needed) {
            const std::string hex = name.substr(name.find_last_of('/') + 1);
            for (const BlobEntry& e : entries) claimed.insert(e.id);
            warn("pack " + hex.substr(0, 8) + " holds data in use but was missing from the index; indexed again");
            kept.emplace_back(id_from_hex(hex), std::move(entries));
            continue;
        }
        orphans.push_back(name);
        r.bytes_deleted += size;
    }
    r.packs_deleted = to_delete.size() + orphans.size();
    r.packs_repacked = to_repack.size();
    if (o.dry_run) return r;

    // 4a. Rewrite partly used packs; every blob is authenticated before it is copied.
    const WriteStats before = repo.write_stats();
    std::size_t done = 0;
    for (const auto& [pack_id, live] : to_repack) {
        progress("rewriting pack " + std::to_string(++done) + " of " + std::to_string(to_repack.size()));
        const Bytes data = backend.get(Repository::pack_name(pack_id));
        for (const BlobEntry& e : live) {
            if (e.offset > data.size() || e.length > data.size() - e.offset)
                throw Error("pack " + short_hex(pack_id) + " is shorter than its index says; nothing was deleted");
            const ByteSpan sealed(data.data() + e.offset, e.length);
            try {
                repo.decode_blob(e.type, e.id, sealed);
            } catch (const Error& ex) {
                throw Error("damaged data in pack " + short_hex(pack_id) + ": " + ex.what() +
                            "; nothing was deleted (run vaultik verify --read-data)");
            }
            repo.copy_blob(e.type, e.id, sealed, e.raw_len);
        }
    }
    repo.flush();  // the new packs and an index file for them
    r.packs_written = repo.write_stats().packs_written - before.packs_written;

    // 4b. A fresh index for the packs that stay, then 4c the old index files, then 4d the packs.
    progress("writing the new index");
    for (std::size_t i = 0; i < kept.size(); i += kPacksPerIndexFile) {
        const auto first = kept.begin() + static_cast<std::ptrdiff_t>(i);
        const auto last = kept.begin() + static_cast<std::ptrdiff_t>(std::min(kept.size(), i + kPacksPerIndexFile));
        repo.write_index_file(IndexFileContent(first, last));
    }
    for (const std::string& name : old_index_files) backend.remove(name);
    progress("deleting unused data");
    for (const Id& id : to_delete) backend.remove(Repository::pack_name(id));
    for (const auto& [id, live] : to_repack) backend.remove(Repository::pack_name(id));
    for (const std::string& name : orphans) backend.remove(name);
    r.bytes_written = repo.write_stats().stored_bytes_added - before.stored_bytes_added;
    return r;
}

}  // namespace vaultik
