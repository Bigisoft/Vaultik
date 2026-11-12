#include "vaultik/ops/rot_check.hpp"

#include <algorithm>
#include <chrono>
#include <random>

#include "vaultik/chunker/fastcdc.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/restore_test.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

struct Candidate {
    std::string path;  // snapshot path
    Node node;
};

// Picks up to `want` files at random in one pass over the version (reservoir sampling).
class Picker {
public:
    Picker(Repository& repo, std::size_t want, std::uint64_t written_before, std::uint64_t seed,
           const std::function<bool()>& cancel)
        : repo_(repo), want_(want), written_before_(written_before), rng_(seed), cancel_(cancel) {}

    void walk(const Id& tree_id, const std::string& prefix) {
        if (cancel_ && cancel_()) throw Cancelled();
        const Tree tree = parse_tree(repo_.load_blob(BlobType::Tree, tree_id));
        for (const Node& n : tree.nodes) {
            const std::string path = prefix.empty() ? n.name : prefix + "/" + n.name;
            if (n.kind == NodeKind::Dir) {
                walk(n.subtree, path);
                continue;
            }
            if (n.kind != NodeKind::File || n.size == 0 || n.content.empty() || n.mtime > written_before_) continue;
            ++seen_;
            if (picked_.size() < want_) {
                picked_.push_back({path, n});
            } else {
                std::uniform_int_distribution<std::uint64_t> d(0, seen_ - 1);
                const std::uint64_t j = d(rng_);
                if (j < want_) picked_[static_cast<std::size_t>(j)] = {path, n};
            }
        }
    }

    std::uint64_t seen() const { return seen_; }
    std::vector<Candidate>& picked() { return picked_; }
    std::mt19937_64& rng() { return rng_; }

private:
    Repository& repo_;
    std::size_t want_;
    std::uint64_t written_before_;
    std::mt19937_64 rng_;
    const std::function<bool()>& cancel_;
    std::uint64_t seen_ = 0;
    std::vector<Candidate> picked_;
};

bool same_metadata(const os::FileInfo& info, const Node& node, bool compare_ctime) {
    return info.type == os::EntryType::File && info.size == node.size && info.mtime == node.mtime &&
           (!compare_ctime || info.ctime == node.ctime);
}

enum class Outcome { Same, Differs, Unreadable, OutOfTime };

}  // namespace

RotCheckResult check_unchanged_files(Repository& repo, const Snapshot& snapshot, const RotCheckOptions& options) {
    RotCheckResult result;
    if (options.max_files == 0 || snapshot.image) return result;
    const auto start = std::chrono::steady_clock::now();
    const auto out_of_time = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > options.max_seconds;
    };
    const auto cancelled = [&] { return options.should_cancel && options.should_cancel(); };
    const auto local_of = [&](const std::string& p) {
        return options.local_path ? options.local_path(p) : original_location(p);
    };

    // Several times as many as are read: many files have changed since the version.
    const std::size_t want = std::max<std::size_t>(50, options.max_files * 10);
    const std::uint64_t written_before = snapshot.time > options.min_age ? snapshot.time - options.min_age : 0;
    Picker picker(repo, want, written_before, options.seed, options.should_cancel);
    if (options.on_progress) options.on_progress("picking files");
    picker.walk(snapshot.tree, "");
    result.candidates = picker.seen();
    std::vector<Candidate>& picked = picker.picked();
    std::shuffle(picked.begin(), picked.end(), picker.rng());

    Bytes buffer;
    for (const Candidate& c : picked) {
        if (result.checked >= options.max_files) break;
        if (cancelled()) throw Cancelled();
        if (out_of_time()) {
            result.budget_reached = true;
            break;
        }
        if (result.bytes + c.node.size > options.max_bytes) continue;
        const std::string local = local_of(c.path);
        if (local.empty()) continue;
        os::FileInfo before;
        os::ErrorCode err = 0;
        if (!os::stat(local, before, err) || !same_metadata(before, c.node, options.compare_ctime)) {
            ++result.changed;
            continue;
        }
        if (options.on_progress) options.on_progress(local);

        // Read it the way the backup does and compare chunk by chunk; stop at the first difference.
        Outcome outcome = Outcome::Same;
        std::uint64_t offset = 0;
        std::uint64_t read = 0;
        {
            os::File file = os::File::open_source(local, err);
            if (!file.valid()) {
                ++result.unreadable;
                continue;
            }
            ChunkReader reader(
                repo.chunker(), [&file](std::uint8_t* dst, std::size_t max) { return file.read(dst, max); }, buffer);
            try {
                std::size_t i = 0;
                ByteSpan chunk;
                while (reader.next(chunk)) {
                    if (cancelled()) throw Cancelled();
                    if (out_of_time()) {
                        outcome = Outcome::OutOfTime;
                        break;
                    }
                    read += chunk.size();
                    if (i >= c.node.content.size() || repo.blob_id(BlobType::Data, chunk) != c.node.content[i]) {
                        outcome = Outcome::Differs;
                        break;
                    }
                    offset += chunk.size();
                    ++i;
                }
                if (outcome == Outcome::Same && i != c.node.content.size()) outcome = Outcome::Differs;
            } catch (const os::OsError&) {
                outcome = Outcome::Unreadable;
            }
        }
        result.bytes += read;
        if (outcome == Outcome::OutOfTime) {
            result.budget_reached = true;
            break;
        }
        if (outcome == Outcome::Unreadable) {
            ++result.unreadable;
            continue;
        }
        // Written to while it was read: not a finding.
        os::FileInfo after;
        if (!os::stat(local, after, err) || !same_metadata(after, c.node, options.compare_ctime)) {
            ++result.changed;
            continue;
        }
        ++result.checked;
        if (outcome == Outcome::Differs) result.damaged.push_back({local, c.path, c.node.size, c.node.mtime, offset});
    }
    return result;
}

}  // namespace vaultik
