#include "vaultik/repo/repository.hpp"

#include <cstring>
#include <limits>

#include "vaultik/crypto/hash.hpp"

namespace vaultik {

namespace {

constexpr std::uint8_t kCompNone = 0;
constexpr std::uint8_t kCompZstd = 1;
constexpr char kIndexAadPrefix[] = "VKINDEX1";
constexpr char kSnapshotAadPrefix[] = "VKSNAP01";
constexpr char kConfigName[] = "config";
constexpr char kIndexDir[] = "index";
constexpr char kSnapshotDir[] = "snapshots";

Bytes blob_aad(BlobType type, const Id& id) {
    Bytes aad;
    aad.reserve(1 + id.size());
    aad.push_back(static_cast<std::uint8_t>(type));
    aad.insert(aad.end(), id.begin(), id.end());
    return aad;
}

Bytes named_aad(const char* prefix, const Id& id) {
    Bytes aad(prefix, prefix + std::strlen(prefix));
    aad.insert(aad.end(), id.begin(), id.end());
    return aad;
}

// "dir/<64 hex>" -> id, or false when the name does not have that shape.
bool parse_object_name(const std::string& name, const std::string& dir, Id& id) {
    if (!name.starts_with(dir + "/")) return false;
    const std::string_view rest = std::string_view(name).substr(dir.size() + 1);
    if (rest.size() != 64) return false;
    try {
        id = id_from_hex(rest);
    } catch (const Error&) {
        return false;
    }
    return true;
}

}  // namespace

void Repository::init(Backend& backend, std::string_view password, const InitOptions& options) {
    if (backend.exists(kConfigName)) throw Error("a repository already exists at " + backend.location());
    // Data without a config is a damaged or unreachable repository, not an empty place: a new
    // config (with a new key) would make that data unreadable for good.
    for (const char* dir : {"data", "index", "snapshots", "keys"}) {
        if (!backend.list(dir).empty())
            throw Error(backend.location() + " already holds Vaultik data (" + dir +
                        ") but no readable config; refusing to create a new repository over it");
    }
    if (password.empty()) throw Error("the password must not be empty");
    options.chunker.validate();

    RepoConfig config;
    random_bytes(config.repo_id);
    random_bytes(config.salt);
    config.kdf_iterations = options.kdf_iterations;
    config.chunker = options.chunker;
    config.zstd_level = options.zstd_level;

    MasterKey key;
    random_bytes(key.enc);
    random_bytes(key.id);
    wrap_master_key(config, key, password);
    backend.put(kConfigName, serialize_config(config));
}

std::unique_ptr<Repository> Repository::open(std::unique_ptr<Backend> backend, std::string_view password) {
    if (!backend->exists(kConfigName)) throw Error("no Vaultik repository at " + backend->location());
    RepoConfig config = parse_config(backend->get(kConfigName));
    const MasterKey key = unwrap_master_key(config, password);
    std::unique_ptr<Repository> repo(new Repository(std::move(backend), std::move(config), key));
    repo->load_indexes();
    return repo;
}

Repository::Repository(std::unique_ptr<Backend> backend, RepoConfig config, const MasterKey& key)
    : backend_(std::move(backend)),
      config_(std::move(config)),
      chunker_(config_.chunker),
      aead_(std::make_unique<AesGcm>(key.enc)),
      lock_aead_(std::make_unique<AesGcm>(key.enc)),
      id_hasher_(key.id),
      compressor_(config_.zstd_level) {}

Repository::~Repository() = default;

std::string Repository::pack_name(const Id& pack_id) {
    const std::string hex = id_hex(pack_id);
    return "data/" + hex.substr(0, 2) + "/" + hex;
}

std::string Repository::snapshot_name(const Id& id) {
    return std::string(kSnapshotDir) + "/" + id_hex(id);
}

Bytes Repository::seal_object(ByteSpan plain, ByteSpan aad, bool compress) {
    if (plain.size() > std::numeric_limits<std::uint32_t>::max()) throw Error("object too large");
    ByteWriter header;
    Bytes payload;
    payload.reserve(plain.size() + 16);
    bool compressed = false;
    if (compress && plain.size() >= 64) {
        Bytes candidate;
        compressor_.compress_append(plain, candidate);
        if (candidate.size() < plain.size()) {
            header.u8(kCompZstd);
            header.varint(plain.size());
            payload = header.take();
            payload.insert(payload.end(), candidate.begin(), candidate.end());
            compressed = true;
        }
    }
    if (!compressed) {
        header.u8(kCompNone);
        header.varint(plain.size());
        payload = header.take();
        payload.insert(payload.end(), plain.begin(), plain.end());
    }
    Bytes sealed = aead_->seal(payload, aad);
    secure_wipe(payload.data(), payload.size());
    return sealed;
}

Bytes Repository::open_object(ByteSpan sealed, ByteSpan aad) {
    const Bytes payload = aead_->open(sealed, aad);
    ByteReader r(payload);
    const std::uint8_t comp = r.u8();
    const std::uint64_t raw_len = r.varint();
    if (raw_len > (1ULL << 31)) throw Error("malformed object: size out of range");
    const ByteSpan body = r.raw(r.remaining());
    if (comp == kCompNone) {
        if (body.size() != raw_len) throw Error("malformed object: length mismatch");
        return Bytes(body.begin(), body.end());
    }
    if (comp == kCompZstd) return decompressor_.decompress(body, static_cast<std::size_t>(raw_len));
    throw Error("malformed object: unknown compression");
}

Id Repository::blob_id(BlobType type, ByteSpan plain) const {
    return id_hasher_.hash(static_cast<std::uint8_t>(type), plain);
}

bool Repository::has_blob(const Id& id) const {
    return index_.contains(id) || pack_pending_.contains(id);
}

bool Repository::save_blob(BlobType type, ByteSpan plain, Id& id_out) {
    id_out = blob_id(type, plain);
    if (has_blob(id_out)) return false;

    const Bytes sealed = seal_object(plain, blob_aad(type, id_out), true);
    pack_.add(type, id_out, sealed, static_cast<std::uint32_t>(plain.size()));
    pack_pending_.insert(id_out);
    ++write_stats_.blobs_added;
    write_stats_.raw_bytes_added += plain.size();
    if (pack_.size() >= kPackTargetSize) finish_pack();
    return true;
}

Bytes Repository::decode_blob(BlobType type, const Id& id, ByteSpan sealed) {
    Bytes plain = open_object(sealed, blob_aad(type, id));
    if (blob_id(type, plain) != id) throw AuthError("blob content does not match its id: " + id_hex(id));
    return plain;
}

Bytes Repository::load_blob(BlobType type, const Id& id) {
    if (pack_pending_.contains(id)) flush();  // the blob is still in memory; write it out first
    const BlobLocation* loc = index_.find(id);
    if (loc == nullptr) throw Error(std::string(blob_type_name(type)) + " blob not found: " + id_hex(id));
    if (loc->type != type) throw Error("blob has an unexpected type: " + id_hex(id));
    const Bytes sealed = backend_->get_range(pack_name(index_.packs()[loc->pack]), loc->offset, loc->length);
    return decode_blob(type, id, sealed);
}

void Repository::finish_pack() {
    if (pack_.empty()) return;
    std::vector<BlobEntry> entries;
    const Bytes bytes = pack_.finish(*aead_, entries);
    const Id pack_id = blake3(bytes);
    backend_->put(pack_name(pack_id), bytes);
    write_stats_.stored_bytes_added += bytes.size();
    ++write_stats_.packs_written;

    const std::uint32_t pos = index_.add_pack(pack_id);
    for (const BlobEntry& e : entries) index_.add(e, pos);
    pack_pending_.clear();
    index_pending_.emplace_back(pack_id, std::move(entries));
    if (index_pending_.size() >= kPacksPerIndexFile) write_index();
}

void Repository::write_index() {
    if (index_pending_.empty()) return;
    write_index_file(index_pending_);
    index_pending_.clear();
}

void Repository::write_index_file(const IndexFileContent& content) {
    const Id index_id = random_id();
    const Bytes plain = serialize_index_file(content);
    const Bytes sealed = seal_object(plain, named_aad(kIndexAadPrefix, index_id), true);
    backend_->put(std::string(kIndexDir) + "/" + id_hex(index_id), sealed);
    write_stats_.stored_bytes_added += sealed.size();
}

void Repository::reload_index() {
    flush();
    index_ = Index();
    load_indexes();
}

void Repository::index_pack(const Id& pack_id, const std::vector<BlobEntry>& entries) {
    const std::uint32_t pos = index_.add_pack(pack_id);
    for (const BlobEntry& e : entries)
        if (!index_.contains(e.id)) index_.add(e, pos);
}

std::vector<std::string> Repository::list_index_files() {
    std::vector<std::string> names;
    for (const std::string& name : backend_->list(kIndexDir)) {
        Id id;
        if (parse_object_name(name, kIndexDir, id)) names.push_back(name);
    }
    return names;
}

IndexFileContent Repository::read_index_file(const std::string& name) {
    Id index_id;
    if (!parse_object_name(name, kIndexDir, index_id)) throw Error("not an index file name: " + name);
    return parse_index_file(open_object(backend_->get(name), named_aad(kIndexAadPrefix, index_id)));
}

void Repository::copy_blob(BlobType type, const Id& id, ByteSpan sealed, std::uint32_t raw_len) {
    if (pack_pending_.contains(id)) return;
    pack_.add(type, id, sealed, raw_len);
    pack_pending_.insert(id);
    if (pack_.size() >= kPackTargetSize) finish_pack();
}

void Repository::flush() {
    finish_pack();
    write_index();
}

void Repository::load_indexes() {
    for (const std::string& name : backend_->list(kIndexDir)) {
        Id index_id;
        if (!parse_object_name(name, kIndexDir, index_id)) continue;
        const Bytes plain = open_object(backend_->get(name), named_aad(kIndexAadPrefix, index_id));
        for (const auto& [pack_id, entries] : parse_index_file(plain)) {
            const std::uint32_t pos = index_.add_pack(pack_id);
            for (const BlobEntry& e : entries) index_.add(e, pos);
        }
    }
}

Id Repository::save_snapshot(const Snapshot& snapshot) {
    flush();
    const Id id = random_id();
    const Bytes sealed = seal_object(serialize_snapshot(snapshot), named_aad(kSnapshotAadPrefix, id), true);
    backend_->put(snapshot_name(id), sealed);
    write_stats_.stored_bytes_added += sealed.size();
    return id;
}

void Repository::update_snapshot(const Id& id, const Snapshot& snapshot) {
    // Never bring back a snapshot that was removed: its data may be pruned already.
    if (!backend_->exists(snapshot_name(id))) throw Error("this version no longer exists");
    const Bytes sealed = seal_object(serialize_snapshot(snapshot), named_aad(kSnapshotAadPrefix, id), true);
    backend_->put(snapshot_name(id), sealed);
}

Snapshot Repository::load_snapshot(const Id& id) {
    const Bytes plain = open_object(backend_->get(snapshot_name(id)), named_aad(kSnapshotAadPrefix, id));
    return parse_snapshot(plain);
}

std::vector<Id> Repository::list_snapshot_ids() {
    std::vector<Id> ids;
    for (const std::string& name : backend_->list(kSnapshotDir)) {
        Id id;
        if (parse_object_name(name, kSnapshotDir, id)) ids.push_back(id);
    }
    return ids;
}

}  // namespace vaultik
