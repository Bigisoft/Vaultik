#include "vaultik/ops/verify.hpp"

#include <map>
#include <unordered_set>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"  // Cancelled

namespace vaultik {

namespace {

class VerifyEngine {
public:
    VerifyEngine(Repository& repo, const VerifyOptions& options) : repo_(repo), options_(options) {}

    VerifyResult run();

private:
    void error(const std::string& message) {
        ++result_.errors;
        if (options_.on_error) options_.on_error(message);
    }
    void progress(const std::string& message) {
        if (options_.on_progress) options_.on_progress(message);
    }

    // Whether a pack belongs to this check's sample: a mix of its id and the seed, compared with
    // the share to read.
    // One pack (picked by the seed) is always in it, so a small repository is never left unread.
    bool in_sample(const Id& pack_id, std::size_t index, std::size_t count) const {
        if (options_.read_share >= 1.0) return true;
        if (count > 0 && index == options_.sample_seed % count) return true;
        std::uint64_t x = options_.sample_seed * 0x9E3779B97F4A7C15ull;
        for (int i = 0; i < 8; ++i) x = (x ^ pack_id[static_cast<std::size_t>(i)]) * 0x100000001B3ull;
        x ^= x >> 33;
        return static_cast<double>(x % 1000000) < options_.read_share * 1000000.0;
    }

    void check_packs();
    void check_snapshots();
    void walk_tree(const Id& id, const std::string& path);

    Repository& repo_;
    const VerifyOptions& options_;
    VerifyResult result_;
    std::unordered_set<Id, IdHash> trees_seen_;
};

void VerifyEngine::check_packs() {
    const Index& index = repo_.index();

    // Expected blobs per pack, from the index.
    std::vector<std::map<Id, BlobLocation>> expected(index.packs().size());
    index.for_each([&](const Id& id, const BlobLocation& loc) { expected[loc.pack].emplace(id, loc); });
    const std::vector<bool> archived = repo_.archived_packs();

    for (std::size_t p = 0; p < index.packs().size(); ++p) {
        const Id& pack_id = index.packs()[p];
        const std::string name = Repository::pack_name(pack_id);
        progress("pack " + std::to_string(p + 1) + "/" + std::to_string(index.packs().size()));
        if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
        ++result_.packs;
        try {
            if (!repo_.backend().exists(name)) {
                error("missing pack " + name);
                continue;
            }
            if (archived[p]) {
                // Reading it needs a thaw; its listings and versions are checked below.
                ++result_.packs_archived;
                result_.blobs += expected[p].size();
                continue;
            }
            std::vector<BlobEntry> entries;
            Bytes pack;
            const bool read = options_.read_data && in_sample(pack_id, p, index.packs().size());
            if (read) {
                pack = repo_.backend().get(name);
                result_.bytes_read += pack.size();
                ++result_.packs_read;
                if (blake3(pack) != pack_id) {
                    Bytes copy = pack;
                    if (repo_.rebuild_pack(pack_id, copy)) {
                        ++result_.packs_repairable;
                        error("pack content does not match its id: " + name +
                              " (its repair data can rebuild it: run a repair)");
                    } else {
                        error("pack content does not match its id: " + name);
                    }
                    continue;
                }
                if (options_.add_parity > 0) {
                    try {
                        if (!repo_.backend().exists(Repository::parity_name(pack_id))) {
                            repo_.write_parity(pack_id, pack, options_.add_parity);
                            ++result_.parity_added;
                        }
                    } catch (const std::exception&) {
                        // not this time; the pack itself is fine
                    }
                }
                entries = parse_pack_header(pack, repo_.aead());
            } else {
                entries = read_pack_header(repo_.backend(), name, repo_.aead());
            }

            for (const BlobEntry& e : entries) {
                auto it = expected[p].find(e.id);
                // A blob indexed in another pack too is fine; only blobs indexed here must match.
                if (it != expected[p].end() &&
                    (it->second.offset != e.offset || it->second.length != e.length || it->second.type != e.type)) {
                    error("index entry disagrees with pack header for blob " + id_hex(e.id) + " in " + name);
                }
                if (read) {
                    try {
                        const Bytes plain = repo_.decode_blob(e.type, e.id, ByteSpan(pack).subspan(e.offset, e.length));
                        if (plain.size() != e.raw_len) error("blob has an unexpected size: " + id_hex(e.id));
                    } catch (const Error& ex) {
                        error("damaged blob " + id_hex(e.id) + " in " + name + ": " + ex.what());
                    }
                }
                ++result_.blobs;
            }
            std::unordered_set<Id, IdHash> in_pack;
            for (const BlobEntry& e : entries) in_pack.insert(e.id);
            for (const auto& [id, loc] : expected[p]) {
                if (!in_pack.contains(id)) error("indexed blob " + id_hex(id) + " is not in pack " + name);
            }
        } catch (const ArchivedError&) {
            ++result_.packs_archived;  // archived without the job saying so: not damage
        } catch (const Error& ex) {
            error("cannot check pack " + name + ": " + ex.what());
        }
    }
}

void VerifyEngine::walk_tree(const Id& id, const std::string& path) {
    if (!trees_seen_.insert(id).second) return;
    ++result_.trees;
    Tree tree;
    try {
        tree = parse_tree(repo_.load_blob(BlobType::Tree, id));
    } catch (const Error& ex) {
        error("cannot read folder " + (path.empty() ? std::string("/") : path) + ": " + ex.what());
        return;
    }
    for (const Node& n : tree.nodes) {
        const std::string child = path + "/" + n.name;
        if (n.kind == NodeKind::Dir) walk_tree(n.subtree, child);
        // Its content, streams and security descriptor.
        bool missing = false;
        for_each_data_blob(n, [&](const Id& chunk) {
            const BlobLocation* loc = repo_.index().find(chunk);
            if (!missing && (loc == nullptr || loc->type != BlobType::Data)) {
                error(child + " references missing data " + id_hex(chunk));
                missing = true;
            }
        });
    }
}

void VerifyEngine::check_snapshots() {
    for (const Id& id : repo_.list_snapshot_ids()) {
        ++result_.snapshots;
        progress("snapshot " + id_hex(id).substr(0, 8));
        try {
            const Snapshot s = repo_.load_snapshot(id);
            walk_tree(s.tree, "");
            if (s.apps) {
                const BlobLocation* loc = repo_.index().find(*s.apps);
                if (loc == nullptr || loc->type != BlobType::Data)
                    error("snapshot " + id_hex(id) + " references a missing program list " + id_hex(*s.apps));
            }
        } catch (const Error& ex) {
            error("cannot read snapshot " + id_hex(id) + ": " + ex.what());
        }
    }
}

VerifyResult VerifyEngine::run() {
    check_packs();
    check_snapshots();
    return result_;
}

}  // namespace

VerifyResult run_verify(Repository& repo, const VerifyOptions& options) {
    VerifyEngine engine(repo, options);
    return engine.run();
}

}  // namespace vaultik
