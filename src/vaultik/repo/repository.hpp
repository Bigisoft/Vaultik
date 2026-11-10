#pragma once

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

private:
    Repository(std::unique_ptr<Backend> backend, RepoConfig config, const MasterKey& key);

    void load_indexes();
    void finish_pack();
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
    std::unordered_set<Id, IdHash> pack_pending_;  // blobs in the pack being built
    IndexFileContent index_pending_;               // packs written but not yet in an index file
    WriteStats write_stats_;
};

}  // namespace vaultik
