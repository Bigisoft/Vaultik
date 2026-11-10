#include "vaultik/ops/change_journal.hpp"

#include <algorithm>
#include <map>

#include <nlohmann/json.hpp>

#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/platform/usn_journal.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

constexpr std::uint64_t kWeek = 7ull * 24 * 3600 * 10'000'000;  // FILETIME ticks

struct Volume {
    std::string root;  // "C:\", as volume_of names it
    std::uint64_t journal_id = 0;
    std::int64_t usn = 0;
};

ByteSpan bytes_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace

struct ChangeJournal::State {
    std::string file;  // where the position is kept
    std::string key;   // the job's folders and exclusions, hashed
    std::vector<std::string> sources;
    std::uint64_t full_scan = 0;  // when every folder was last read
    std::vector<Volume> now;      // the journals' positions before this backup
    // Usable only when every source's volume answered.
    std::optional<Id> parent;
    std::map<std::string, std::shared_ptr<os::UsnChanges>> changes;  // volume root -> records since
    std::vector<std::pair<std::string, std::string>> source_roots;  // folded source path -> volume root
};

ChangeJournal::ChangeJournal(std::string store_dir, std::string job_id, std::vector<std::string> sources,
                             const std::vector<std::string>& excludes)
    : s_(std::make_unique<State>()) {
    try {
        if (!os::process_elevated()) return;
        for (std::string& p : sources) p = os::absolute_path(p);
        std::string text;
        for (const std::string& p : sources) text += os::fold_case(p) + '\n';
        text += '\n';
        for (const std::string& e : excludes) text += e + '\n';
        s_->key = id_hex(blake3(bytes_of(text)));
        s_->sources = sources;
        s_->file = os::join(os::join(store_dir, "journal"), job_id + ".json");

        // The journals now, before anything is read: what changes during the backup is in the
        // next one's records.
        for (const std::string& p : sources) {
            const os::VolumeLocation loc = os::volume_of(p);
            if (loc.root.empty()) return;
            s_->source_roots.emplace_back(os::fold_case(p), loc.root);
            const bool known = std::any_of(s_->now.begin(), s_->now.end(), [&](const Volume& v) { return v.root == loc.root; });
            if (known) continue;
            const std::optional<os::UsnJournalState> j = os::usn_journal_state(loc.root);
            if (!j) return;  // no journal on that volume
            s_->now.push_back({loc.root, j->journal_id, j->next_usn});
        }

        // The position after the last backup.
        if (!os::exists(s_->file)) return;
        const Bytes b = os::read_file(s_->file, 1 << 20);
        const nlohmann::json j = nlohmann::json::parse(std::string(b.begin(), b.end()));
        if (j.value("key", std::string()) != s_->key) return;
        s_->full_scan = j.value("full_scan", std::uint64_t{0});
        const std::uint64_t now = os::now();
        if (s_->full_scan > now || now - s_->full_scan > kWeek) return;  // read everything once a week
        std::map<std::string, std::shared_ptr<os::UsnChanges>> changes;
        for (const Volume& v : s_->now) {
            const nlohmann::json* saved = nullptr;
            for (const nlohmann::json& e : j.at("volumes"))
                if (e.value("root", std::string()) == v.root) saved = &e;
            if (saved == nullptr || saved->value("journal", std::uint64_t{0}) != v.journal_id) return;
            std::optional<os::UsnChanges> c =
                os::usn_changes_since(v.root, v.journal_id, saved->value("usn", std::int64_t{0}), v.usn);
            if (!c) return;  // the journal no longer reaches back that far
            changes[v.root] = std::make_shared<os::UsnChanges>(std::move(*c));
        }
        s_->changes = std::move(changes);
        s_->parent = id_from_hex(j.value("snapshot", std::string()));
    } catch (const std::exception&) {
        s_->parent.reset();
        s_->changes.clear();
    }
}

ChangeJournal::~ChangeJournal() = default;

bool ChangeJournal::usable() const {
    return s_->parent.has_value() && !s_->changes.empty();
}

void ChangeJournal::apply(BackupOptions& options) const {
    if (!usable()) return;
    options.journal_parent = s_->parent;
    // The folder's volume is its source's (links and mount points inside are not followed).
    auto roots = s_->source_roots;
    auto changes = s_->changes;
    options.dir_unchanged = [roots, changes](const std::string& dir) {
        const std::string folded = os::fold_case(dir);
        for (const auto& [source, root] : roots) {
            if (!folded.starts_with(source)) continue;
            if (folded.size() > source.size() && folded[source.size()] != '\\' && folded[source.size()] != '/' &&
                !source.ends_with('\\'))
                continue;  // "C:\a\bc" is not inside "C:\a\b"
            const auto it = changes.find(root);
            if (it == changes.end()) return false;
            try {
                const os::FileRef ref = os::file_ref(dir);
                return !it->second->parents.contains(ref) && !it->second->changed.contains(ref);
            } catch (const std::exception&) {
                return false;
            }
        }
        return false;
    };
}

void ChangeJournal::save(const Id& snapshot) noexcept {
    try {
        if (s_->now.empty() || s_->file.empty()) return;
        nlohmann::json volumes = nlohmann::json::array();
        for (const Volume& v : s_->now) volumes.push_back({{"root", v.root}, {"journal", v.journal_id}, {"usn", v.usn}});
        // A backup that used the journal keeps the time of the last full read; one that read
        // everything starts the week again.
        const nlohmann::json j = {{"snapshot", id_hex(snapshot)},
                                  {"key", s_->key},
                                  {"full_scan", usable() ? s_->full_scan : os::now()},
                                  {"volumes", volumes}};
        os::make_dirs(os::parent_of(s_->file));
        os::write_file_atomic(s_->file, bytes_of(j.dump(2) + "\n"));
    } catch (const std::exception&) {
        // the next backup reads every folder
    }
}

}  // namespace vaultik
