#include "vaultik/ops/restore_test.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <random>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

struct Candidate {
    std::string path;  // snapshot path, '/' separated
    Node node;
};

// Picks up to `want` files at random from the whole version, in one pass (reservoir sampling),
// so memory stays small however many files there are.
class Picker {
public:
    Picker(Repository& repo, std::size_t want, std::uint64_t max_size, std::uint64_t seed,
           const std::function<bool()>& cancel)
        : repo_(repo), want_(want), max_size_(max_size), rng_(seed), cancel_(cancel) {}

    void walk(const Id& tree_id, const std::string& prefix) {
        if (cancel_ && cancel_()) throw Cancelled();
        const Tree tree = parse_tree(repo_.load_blob(BlobType::Tree, tree_id));
        for (const Node& n : tree.nodes) {
            const std::string path = prefix.empty() ? n.name : prefix + "/" + n.name;
            if (n.kind == NodeKind::Dir) {
                walk(n.subtree, path);
            } else if (n.kind == NodeKind::File && n.size > 0 && n.size <= max_size_) {
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
    }

    std::uint64_t seen() const { return seen_; }
    std::vector<Candidate>& picked() { return picked_; }

private:
    Repository& repo_;
    std::size_t want_;
    std::uint64_t max_size_;
    std::mt19937_64 rng_;
    const std::function<bool()>& cancel_;
    std::uint64_t seen_ = 0;
    std::vector<Candidate> picked_;
};

std::filesystem::path fs_path(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Whether two files hold the same bytes: "" when they do, what differs when not, nothing when the
// file on this PC could not be read (in use, or gone).
std::optional<std::string> compare_files(const std::string& a, const std::string& b) {
    os::ErrorCode ea = 0, eb = 0;
    os::File fa = os::File::open_read(a, ea);
    if (!fa.valid()) return "the restored file could not be opened";
    os::File fb = os::File::open_read(b, eb);
    if (!fb.valid()) return std::nullopt;
    std::vector<std::uint8_t> ba(1 << 20), bb(1 << 20);
    std::uint64_t offset = 0;
    for (;;) {
        const std::size_t na = fa.read(ba.data(), ba.size());
        std::size_t nb = 0;
        while (nb < na) {
            const std::size_t got = fb.read(bb.data() + nb, na - nb);
            if (got == 0) break;
            nb += got;
        }
        if (na != nb) return "it is shorter or longer than the file on this PC";
        if (na == 0) return std::string();
        for (std::size_t i = 0; i < na; ++i)
            if (ba[i] != bb[i]) return "its content differs from the unchanged file on this PC at byte " +
                                       std::to_string(offset + i);
        offset += na;
    }
}

}  // namespace

std::string original_location(const std::string& snapshot_path) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= snapshot_path.size()) {
        std::size_t end = snapshot_path.find('/', pos);
        if (end == std::string::npos) end = snapshot_path.size();
        if (end > pos) parts.push_back(snapshot_path.substr(pos, end - pos));
        pos = end + 1;
    }
    if (parts.empty()) return {};
#ifdef _WIN32
    std::string out;
    std::size_t first = 1;
    if (parts[0] == "UNC") {
        if (parts.size() < 4) return {};
        out = "\\\\" + parts[1] + "\\" + parts[2];
        first = 3;
    } else {
        if (parts[0].size() != 1 || parts.size() < 2) return {};
        out = parts[0] + ":";
    }
    for (std::size_t i = first; i < parts.size(); ++i) out += "\\" + parts[i];
    return out;
#else
    std::string out;
    for (const std::string& p : parts) out += "/" + p;
    return out;
#endif
}

RestoreTestResult test_restore(Repository& repo, const Snapshot& snapshot, const RestoreTestOptions& options) {
    RestoreTestResult result;
    if (options.work_dir.empty()) throw Error("no folder for the test restore");
    const std::uint64_t per_file_limit = options.max_bytes;
    Picker picker(repo, options.max_files, per_file_limit, options.seed, options.should_cancel);
    if (options.on_progress) options.on_progress("picking files");
    picker.walk(snapshot.tree, "");
    result.candidates = picker.seen();

    // Within the size budget: the picked files in random order, skipping those that no longer fit.
    std::vector<Candidate> chosen;
    std::uint64_t total = 0;
    for (Candidate& c : picker.picked()) {
        if (total + c.node.size > options.max_bytes) continue;
        total += c.node.size;
        chosen.push_back(std::move(c));
    }

    // Folders left by a test that was cut short (the computer went off) go first, once a day old,
    // so a test running at the same time in another process keeps its own.
    {
        std::error_code ec;
        const auto day_ago = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        for (std::filesystem::directory_iterator it(fs_path(options.work_dir), ec), end; !ec && it != end;
             it.increment(ec)) {
            std::error_code e2;
            if (!it->path().filename().native().starts_with(fs_path("restore-test-").native()) || !it->is_directory(e2) ||
                it->is_symlink(e2))
                continue;
            const auto when = it->last_write_time(e2);
            if (!e2 && when < day_ago) std::filesystem::remove_all(it->path(), e2);
        }
    }
    std::uint8_t rnd[8];
    random_bytes(rnd);
    const std::string work = os::join(options.work_dir, "restore-test-" + to_hex(ByteSpan(rnd, sizeof rnd)));
    os::make_dirs(work);
    struct Cleanup {
        std::string dir;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(fs_path(dir), ec);
        }
    } cleanup{work};

    for (std::size_t i = 0; i < chosen.size(); ++i) {
        if (options.should_cancel && options.should_cancel()) throw Cancelled();
        const Candidate& c = chosen[i];
        if (options.on_progress)
            options.on_progress(std::to_string(i + 1) + "/" + std::to_string(chosen.size()) + " " + c.path);
        const std::string target = os::join(work, std::to_string(i));
        std::string warning;
        RestoreResult r;
        try {
            RestoreOptions ro;
            ro.target = target;
            ro.subpath = c.path;
            ro.should_cancel = options.should_cancel;
            ro.on_warning = [&warning](const std::string& w) {
                if (warning.empty()) warning = w;
            };
            r = run_restore(repo, snapshot, ro);
        } catch (const Cancelled&) {
            throw;
        } catch (const std::exception& e) {
            result.problems.push_back(c.path + ": could not be restored: " + e.what());
            continue;
        }
        if (r.errors > 0 || r.files != 1) {
            result.problems.push_back(c.path + ": could not be restored" + (warning.empty() ? "" : ": " + warning));
            continue;
        }
        const std::string restored = os::join(target, c.node.name);
        os::FileInfo info;
        os::ErrorCode err = 0;
        if (!os::stat(restored, info, err)) {
            result.problems.push_back(c.path + ": the restored file is missing");
            continue;
        }
        if (info.size != c.node.size) {
            result.problems.push_back(c.path + ": restored " + std::to_string(info.size) + " bytes instead of " +
                                      std::to_string(c.node.size));
            continue;
        }
        ++result.files;
        result.bytes += info.size;

        // The file on this PC, when it is still as it was backed up.
        const std::string original = original_location(c.path);
        os::FileInfo now;
        if (!original.empty() && os::stat(original, now, err) && now.type == os::EntryType::File &&
            now.size == c.node.size && now.mtime == c.node.mtime) {
            const std::optional<std::string> diff = compare_files(restored, original);
            if (diff && !diff->empty()) {
                result.problems.push_back(c.path + ": " + *diff);
                continue;
            }
            if (diff) ++result.compared;
        }
        std::error_code ec;
        std::filesystem::remove_all(fs_path(target), ec);
    }
    return result;
}

}  // namespace vaultik
