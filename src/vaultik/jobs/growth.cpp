#include "vaultik/jobs/growth.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/job.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/restore_test.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::jobs {

namespace {

// Folders whose content is rarely worth keeping: downloads (usually still on the internet),
// temporary files, caches and build output.
constexpr const char* kWellKnown[] = {"downloads", "temp", "tmp",     "cache",  "caches", ".cache",
                                      "obj",       "build", "target", ".gradle", ".npm",  ".nuget",
                                      ".venv",     "venv",  ".tox",   "node_modules"};

std::string lower_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string last_component(const std::string& path) {
    const std::size_t at = path.find_last_of("\\/");
    return at == std::string::npos ? path : path.substr(at + 1);
}

// Whether the job uses a preset: it has all of its patterns.
bool uses(const std::vector<std::string>& excludes, const ExclusionPreset& p) {
    return std::all_of(p.patterns.begin(), p.patterns.end(), [&](const std::string& x) {
        return std::find(excludes.begin(), excludes.end(), x) != excludes.end();
    });
}

std::uint64_t key_of(const Id& id) {
    std::uint64_t k;
    std::memcpy(&k, id.data(), sizeof k);
    return k;
}

struct Folder {
    std::map<std::string, std::unique_ptr<Folder>> children;
    std::uint64_t own_stored = 0, own_raw = 0, own_files = 0;  // files directly in it
    std::uint64_t stored = 0, raw = 0, files = 0;              // with everything below
};

// Whether a snapshot path is strictly inside one of `roots` (snapshot paths).
const std::string* root_of(const std::vector<std::string>& roots, const std::string& path) {
    for (const std::string& r : roots)
        if (path.size() > r.size() + 1 && path.starts_with(r) && path[r.size()] == '/') return &r;
    return nullptr;
}

class Walker {
public:
    // `roots`: the backed-up folders (snapshot paths). A ready-made exclusion that would leave out
    // a whole backed-up folder says nothing about what is in it.
    Walker(Repository& repo, const GrowthOptions& options, const std::vector<std::string>& roots)
        : repo_(repo), options_(options), roots_(roots) {
        for (const ExclusionPreset& p : exclusion_presets())
            if (!uses(options.excludes, p)) presets_.push_back(&p);
        preset_totals_.resize(presets_.size());
        for (const std::string& r : roots_) {
            std::vector<bool> m(presets_.size(), false);
            const std::string where = local(r);
            if (!where.empty())
                for (std::size_t i = 0; i < presets_.size(); ++i) m[i] = path_excluded(presets_[i]->patterns, where);
            root_presets_.emplace(r, std::move(m));
        }
    }

    // Marks what the version holds as stored; with `count`, new data goes to its folders.
    void walk(const Id& tree_id, const std::string& prefix, bool count) {
        if (!trees_.insert(tree_id).second) return;  // everything below was seen already
        if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
        const Tree tree = parse_tree(repo_.load_blob(BlobType::Tree, tree_id));
        for (const Node& n : tree.nodes) {
            const std::string path = prefix.empty() ? n.name : prefix + "/" + n.name;
            if (n.kind == NodeKind::Dir) {
                walk(n.subtree, path, count);
                continue;
            }
            std::uint64_t added = 0, added_raw = 0;
            const auto take = [&](const Id& id) {
                if (!chunks_.insert(key_of(id)).second) return;
                const BlobLocation* loc = repo_.index().find(id);
                if (loc == nullptr) return;  // removed since: it takes no space now
                added += loc->length;
                added_raw += loc->raw_len;
            };
            for (const Id& id : n.content) take(id);
            for (const DataStream& s : n.streams)
                for (const Id& id : s.content) take(id);
            if (count && added > 0) add(prefix, path, added, added_raw);
        }
    }

    Folder& root() { return root_; }
    std::uint64_t stored = 0, raw = 0, files = 0;
    std::vector<PresetGrowth> preset_growth() const {
        std::vector<PresetGrowth> out;
        for (std::size_t i = 0; i < presets_.size(); ++i)
            if (preset_totals_[i].stored > 0) {
                PresetGrowth g = preset_totals_[i];
                g.id = presets_[i]->id;
                g.patterns = presets_[i]->patterns;
                out.push_back(std::move(g));
            }
        std::sort(out.begin(), out.end(), [](const PresetGrowth& a, const PresetGrowth& b) { return a.stored > b.stored; });
        return out;
    }

private:
    std::string local(const std::string& snapshot_path) const {
        return options_.local_path ? options_.local_path(snapshot_path) : original_location(snapshot_path);
    }

    void add(const std::string& folder, const std::string& file, std::uint64_t s, std::uint64_t r) {
        stored += s;
        raw += r;
        ++files;
        Folder* f = &root_;
        std::size_t at = 0;
        while (at < folder.size()) {
            std::size_t next = folder.find('/', at);
            if (next == std::string::npos) next = folder.size();
            auto& child = f->children[folder.substr(at, next - at)];
            if (!child) child = std::make_unique<Folder>();
            f = child.get();
            at = next + 1;
        }
        f->own_stored += s;
        f->own_raw += r;
        ++f->own_files;

        // The ready-made exclusions that would have left the file out.
        if (presets_.empty()) return;
        const std::string* root = root_of(roots_, file);
        if (root == nullptr) return;
        const std::vector<bool>& whole = root_presets_.at(*root);
        auto it = folder_presets_.find(folder);
        if (it == folder_presets_.end()) {
            std::vector<bool> m(presets_.size(), false);
            const std::string where = local(folder);
            if (!where.empty())
                for (std::size_t i = 0; i < presets_.size(); ++i) m[i] = path_excluded(presets_[i]->patterns, where);
            it = folder_presets_.emplace(folder, std::move(m)).first;
        }
        const std::string where = local(file);
        for (std::size_t i = 0; i < presets_.size(); ++i) {
            if (whole[i]) continue;
            if (!it->second[i] && (where.empty() || !path_excluded(presets_[i]->patterns, where))) continue;
            preset_totals_[i].stored += s;
            ++preset_totals_[i].files;
        }
    }

    Repository& repo_;
    const GrowthOptions& options_;
    const std::vector<std::string>& roots_;
    std::unordered_map<std::string, std::vector<bool>> root_presets_;  // per root: the presets that leave it out
    std::unordered_set<Id, IdHash> trees_;
    std::unordered_set<std::uint64_t> chunks_;  // the first 8 bytes of a chunk's id are enough
    Folder root_;
    std::vector<const ExclusionPreset*> presets_;
    std::vector<PresetGrowth> preset_totals_;
    std::unordered_map<std::string, std::vector<bool>> folder_presets_;
};

void sum_up(Folder& f) {
    f.stored = f.own_stored;
    f.raw = f.own_raw;
    f.files = f.own_files;
    for (auto& [name, c] : f.children) {
        sum_up(*c);
        f.stored += c->stored;
        f.raw += c->raw;
        f.files += c->files;
    }
}

class Picker {
public:
    Picker(const GrowthOptions& options, std::uint64_t threshold, const std::vector<std::string>& roots)
        : options_(options), threshold_(threshold), roots_(roots) {}

    // The most specific folders that hold a share of the growth: a folder whose parts each hold
    // less is reported whole; otherwise its larger parts are looked at in turn, and what is left
    // of it is reported as its rest.
    void pick(const Folder& f, const std::string& path) {
        const std::string where = local(path);
        // Only folders inside what is backed up: leaving out a folder above it would leave out all.
        std::optional<GrowthSuggestion> s =
            where.empty() || !inside_roots(path) ? std::nullopt : suggest_exclusion(where, options_.excludes);
        std::vector<std::pair<const std::string*, const Folder*>> big;
        for (const auto& [name, c] : f.children)
            if (c->stored >= threshold_) big.emplace_back(&name, c.get());
        if (s || big.empty()) {
            emit(path, where, false, f.stored, f.raw, f.files, std::move(s));
            return;
        }
        std::uint64_t stored = f.stored, raw = f.raw, files = f.files;
        for (const auto& [name, c] : big) {
            pick(*c, path + "/" + *name);
            stored -= c->stored;
            raw -= c->raw;
            files -= c->files;
        }
        if (stored >= threshold_) emit(path, where, true, stored, raw, files, std::nullopt);
    }

    std::vector<FolderGrowth> folders;

private:
    std::string local(const std::string& snapshot_path) const {
        return options_.local_path ? options_.local_path(snapshot_path) : original_location(snapshot_path);
    }

    bool inside_roots(const std::string& path) const { return root_of(roots_, path) != nullptr; }

    void emit(const std::string& path, const std::string& where, bool rest, std::uint64_t stored,
              std::uint64_t raw, std::uint64_t files, std::optional<GrowthSuggestion> s) {
        FolderGrowth g;
        g.path = where.empty() ? path : where;
        g.snapshot_path = path;
        g.rest = rest;
        g.stored = stored;
        g.raw = raw;
        g.files = files;
        g.suggestion = std::move(s);
        folders.push_back(std::move(g));
    }

    const GrowthOptions& options_;
    std::uint64_t threshold_;
    const std::vector<std::string>& roots_;  // the backed-up folders, as snapshot paths
};

}  // namespace

std::optional<GrowthSuggestion> suggest_exclusion(const std::string& path, const std::vector<std::string>& excludes) {
    if (path.empty() || path_excluded(excludes, path)) return std::nullopt;
    for (const ExclusionPreset& p : exclusion_presets()) {
        if (uses(excludes, p) || !path_excluded(p.patterns, path)) continue;
        return GrowthSuggestion{p.id, p.patterns};
    }
    const std::string name = lower_ascii(last_component(path));
    if (path.find_first_of("*?") != std::string::npos) return std::nullopt;  // would read as a wildcard
    for (const char* w : kWellKnown)
        if (name == w) return GrowthSuggestion{{}, {path}};
    return std::nullopt;
}

GrowthReport what_grew(Repository& repo, const std::vector<std::pair<Id, Snapshot>>& snapshots,
                       const GrowthOptions& options) {
    GrowthReport report;
    report.since = options.since;
    report.until = options.until;
    std::vector<const std::pair<Id, Snapshot>*> sorted;
    for (const auto& s : snapshots) sorted.push_back(&s);
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->second.time < b->second.time; });

    // Before the period: the newest version of each computer and set of folders.
    std::map<std::string, const Snapshot*> before;
    for (const auto* s : sorted) {
        if (s->second.time >= options.since) break;
        std::vector<std::string> paths = s->second.paths;
        std::sort(paths.begin(), paths.end());
        std::string key = s->second.hostname;
        for (const std::string& p : paths) key += '\n' + p;
        before[key] = &s->second;
    }
    // The period, and the folders the job's versions in it back up.
    std::vector<const Snapshot*> period;
    std::vector<bool> mine;
    std::vector<std::string> roots;
    for (const auto* s : sorted) {
        if (s->second.time < options.since || (options.until != 0 && s->second.time >= options.until)) continue;
        period.push_back(&s->second);
        mine.push_back(!options.select || options.select(s->second));
        if (!mine.back()) continue;
        ++report.versions;
        for (const std::string& p : s->second.paths) {
            std::string r;
            try {
                for (const std::string& c : os::path_components(p)) r += (r.empty() ? "" : "/") + c;
            } catch (const std::exception&) {
                continue;  // not a path of this system
            }
            if (std::find(roots.begin(), roots.end(), r) == roots.end()) roots.push_back(std::move(r));
        }
    }

    Walker walker(repo, options, roots);
    std::size_t done = 0;
    for (const auto& [key, s] : before) {
        if (options.on_progress)
            options.on_progress("reading what was stored before (" + std::to_string(++done) + "/" +
                                std::to_string(before.size()) + ")");
        walker.walk(s->tree, "", false);
    }

    // The period, oldest first: new data of the job's versions goes to their folders.
    done = 0;
    for (std::size_t i = 0; i < period.size(); ++i) {
        if (options.on_progress)
            options.on_progress("reading version " + std::to_string(++done) + " of " + std::to_string(period.size()));
        walker.walk(period[i]->tree, "", mine[i]);
    }
    report.stored = walker.stored;
    report.raw = walker.raw;
    report.files = walker.files;
    report.presets = walker.preset_growth();

    Folder& root = walker.root();
    sum_up(root);
    const auto threshold = std::max<std::uint64_t>(
        1, static_cast<std::uint64_t>(static_cast<double>(root.stored) * std::max(0.0, options.min_share)));
    Picker picker(options, threshold, roots);
    for (const auto& [name, top] : root.children) {
        if (top->stored > 0) picker.pick(*top, name);
    }
    std::vector<FolderGrowth> folders = std::move(picker.folders);
    std::stable_sort(folders.begin(), folders.end(), [](const FolderGrowth& a, const FolderGrowth& b) { return a.stored > b.stored; });
    if (folders.size() > options.top) folders.resize(options.top);
    report.folders = std::move(folders);
    return report;
}

std::string to_json(const GrowthReport& r) {
    nlohmann::json j;
    j["since"] = r.since;
    j["until"] = r.until;
    j["versions"] = r.versions;
    j["stored"] = r.stored;
    j["raw"] = r.raw;
    j["files"] = r.files;
    j["folders"] = nlohmann::json::array();
    for (const FolderGrowth& f : r.folders) {
        nlohmann::json o{{"path", f.path},   {"snapshot_path", f.snapshot_path}, {"rest", f.rest},
                         {"stored", f.stored}, {"raw", f.raw},                     {"files", f.files}};
        if (f.suggestion) o["suggestion"] = {{"preset", f.suggestion->preset}, {"patterns", f.suggestion->patterns}};
        j["folders"].push_back(std::move(o));
    }
    j["presets"] = nlohmann::json::array();
    for (const PresetGrowth& p : r.presets)
        j["presets"].push_back({{"id", p.id}, {"patterns", p.patterns}, {"stored", p.stored}, {"files", p.files}});
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace vaultik::jobs
