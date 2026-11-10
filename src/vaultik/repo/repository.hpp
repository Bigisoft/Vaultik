#pragma once

#include <atomic>
#include <unordered_map>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "vaultik/chunker/fastcdc.hpp"
#include "vaultik/compress/zstd.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/crypto/hash.hpp"
#include "vaultik/model/snapshot.hpp"
#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/config.hpp"
#include "vaultik/repo/index.hpp"
#include "vaultik/repo/pack.hpp"

namespace vaultik {

struct InitOptions {
    std::uint32_t kdf_iterations = kDefaultKdfIterations;
    ChunkerParams chunker{};
    int zstd_level = kDefaultZstdLevel;
};

struct WriteStats {
    std::uint64_t blobs_added = 0;
    std::uint64_t raw_bytes_added = 0;     // plaintext bytes of new blobs
    std::uint64_t stored_bytes_added = 0;  // bytes written to the backend (packs, indexes, snapshots)
    std::uint64_t packs_written = 0;
};

constexpr std::size_t kPackTargetSize = 32u << 20;
constexpr std::size_t kPacksPerIndexFile = 64;

// An open repository: owns the backend, keys and in-memory index. Not thread-safe.
class Repository {
public:
    // Creates a new repository in an empty backend. Throws if one already exists there.
    static void init(Backend& backend, std::string_view password, const InitOptions& options = {});

    // Opens an existing repository and loads its index. Throws AuthError on a wrong password.
    // Without `load_index` the index files are not read at all (repair, when one is damaged).
    static std::unique_ptr<Repository> open(std::unique_ptr<Backend> backend, std::string_view password,
                                            bool load_index = true);

    ~Repository();
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    const RepoConfig& config() const { return config_; }
    const FastCdc& chunker() const { return chunker_; }
    Backend& backend() { return *backend_; }
    const Index& index() const { return index_; }
    const WriteStats& write_stats() const { return write_stats_; }
    const AesGcm& aead() const { return *aead_; }

    // BLAKE3-keyed(id_key, type || plain): the same bytes stored as data and as a tree get
    // different ids, so deduplication can never return a blob of the wrong type.
    Id blob_id(BlobType type, ByteSpan plain) const;
    bool has_blob(const Id& id) const;

    // Stores a blob unless an identical one already exists. Returns true when it was new.
    bool save_blob(BlobType type, ByteSpan plain, Id& id_out);

    // Loads and authenticates a blob, and checks that its content matches its id.
    Bytes load_blob(BlobType type, const Id& id);

    // Decodes a sealed blob (as stored in a pack) and checks its content against `id`.
    Bytes decode_blob(BlobType type, const Id& id, ByteSpan sealed);

    // Writes the current pack and any pending index entries. Call before saving a snapshot.
    void flush();

    // Speeds up writing: new blobs are compressed and encrypted on `threads` worker threads (0:
    // on the calling thread; their order in the packs stays the same), and up to `uploads` packs
    // are written in the background while the next one is built (0: each pack is written before
    // the backup goes on). Each upload in flight holds a pack (about 32 MiB) in memory. A failed
    // background step is final: its blobs count as stored in memory only, so every later
    // save_blob, load_blob and flush throws that error again, and no snapshot can be saved that
    // would point at them.
    void set_parallelism(unsigned threads, unsigned uploads);

    Id save_snapshot(const Snapshot& snapshot);
    // Rewrites an existing snapshot (its pin or note). Hold a shared lock, so a forget cannot
    // remove the snapshot meanwhile; throws Error when it no longer exists.
    void update_snapshot(const Id& id, const Snapshot& snapshot);
    Snapshot load_snapshot(const Id& id);
    std::vector<Id> list_snapshot_ids();

    // Adds an already sealed blob to the pack being built (prune repacking), even when the index
    // has it elsewhere; the new location replaces the old one once the pack is written.
    void copy_blob(BlobType type, const Id& id, ByteSpan sealed, std::uint32_t raw_len);

    // Writes pending data, then reads the index files again: other runs may have added data since
    // the repository was opened (prune does this once it holds its exclusive lock).
    void reload_index();
    // Adds a pack that no index file lists to the in-memory index (prune, after reading the
    // pack's own header), so its blobs can be loaded.
    void index_pack(const Id& pack_id, const std::vector<BlobEntry>& entries);
    // Empties the in-memory index (repair builds it again from the packs).
    void clear_index();

    // Index files, for prune: their names ("index/<hex>"), one file's content, and a new file.
    std::vector<std::string> list_index_files();
    IndexFileContent read_index_file(const std::string& name);
    void write_index_file(const IndexFileContent& content);

    // A second cipher with the same key, for lock files: a lock is refreshed from its own thread
    // while this object is busy, and AesGcm objects are not shared between threads.
    const AesGcm& lock_aead() const { return *lock_aead_; }

    static std::string pack_name(const Id& pack_id);
    static std::string snapshot_name(const Id& id);

    // Repair data (docs/FORMAT.md, "Repair data"): while it is on, every pack written gets
    // parity/<xx>/<pack-id> next to it, `percent` of its size as Reed-Solomon parity, so a pack
    // damaged by bad sectors can be rebuilt (repair). The setting lives in the repository
    // ("parity/percent"), so every program writing to it (backup, prune, repair, a copy) adds it.
    // 0 when off. Writing repair data never fails a backup: a pack without it is only less safe.
    unsigned parity_percent() const { return parity_percent_.load(); }
    // Turns it on (1..50) or off (0) for packs written from now on; existing repair data stays.
    void set_parity_percent(unsigned percent);
    static std::string parity_name(const Id& pack_id);
    // Writes the repair data of an intact pack (`bytes` hash to `pack_id`). Throws Error.
    void write_parity(const Id& pack_id, ByteSpan bytes, unsigned percent);
    // Rebuilds a damaged pack (read as `pack`: wrong content, cut short or longer) from its repair
    // data. True when `pack` now hashes to `pack_id`; false (and `pack` unchanged) when the pack
    // has no repair data, it cannot be read, or too much is damaged.
    bool rebuild_pack(const Id& pack_id, Bytes& pack);
    // Reads a pack that cannot be read whole (bad sectors): what can be read, in small pieces,
    // with zeros where reading failed, for rebuild_pack. Throws when even its size is unknown.
    Bytes read_pack_salvaging(const Id& pack_id);
    // Blobs of this pack are read from `bytes` instead of the backend from now on (a repair's dry
    // run, which rebuilds a pack in memory without writing it).
    void use_pack_bytes(const Id& pack_id, Bytes bytes);

private:
    Repository(std::unique_ptr<Backend> backend, RepoConfig config, const MasterKey& key);

    void load_indexes();
    void finish_pack();
    struct Pipeline;
    // Moves blobs the workers have sealed into the pack, in order (`all`: waits for every one).
    void drain(bool all);
    // Takes finished uploads into the index files to write (`all`: waits for every one).
    void collect_uploads(bool all);
    // Throws the first failure of a background step, if there was one.
    void check_pipeline();
    std::unique_ptr<Pipeline> pipe_;
    Key32 enc_key_{};  // for the workers' own ciphers
    std::string write_failed_;  // a pack written in line failed: nothing more may count as stored
    void write_index();
    Bytes seal_object(ByteSpan plain, ByteSpan aad, bool compress);
    Bytes open_object(ByteSpan sealed, ByteSpan aad);

    std::unique_ptr<Backend> backend_;
    RepoConfig config_;
    FastCdc chunker_;
    std::unique_ptr<AesGcm> aead_;
    std::unique_ptr<AesGcm> lock_aead_;
    KeyedHasher id_hasher_;
    ZstdCompressor compressor_;
    ZstdDecompressor decompressor_;

    Index index_;
    PackWriter pack_;
    std::atomic<unsigned> parity_percent_{0};
    std::unordered_map<Id, Bytes, IdHash> pack_bytes_;  // use_pack_bytes
    std::unordered_set<Id, IdHash> pack_pending_;  // blobs in the pack being built
    IndexFileContent index_pending_;               // packs written but not yet in an index file
    WriteStats write_stats_;
};

}  // namespace vaultik
