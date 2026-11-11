#include "vaultik/repo/repository.hpp"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/repo/recovery.hpp"

namespace vaultik {

namespace {

constexpr std::uint8_t kCompNone = 0;
constexpr std::uint8_t kCompZstd = 1;
constexpr char kIndexAadPrefix[] = "VKINDEX1";
constexpr char kSnapshotAadPrefix[] = "VKSNAP01";
constexpr char kConfigName[] = "config";
constexpr char kParityMarker[] = "parity/percent";

// Best effort: a pack is complete without its repair data, so a failure here never fails the
// write of the pack (a later health check adds what is missing).
// `data`: a pack of file data only, whose repair data goes to the backend's data class too.
void put_parity(Backend& backend, const Id& pack_id, ByteSpan bytes, unsigned percent, bool data) {
    if (percent == 0) return;
    try {
        const Bytes parity = repo::make_recovery(bytes, percent);
        if (data) backend.put_data(Repository::parity_name(pack_id), parity);
        else backend.put(Repository::parity_name(pack_id), parity);
    } catch (const std::exception&) {
    }
}
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

std::unique_ptr<Repository> Repository::open(std::unique_ptr<Backend> backend, std::string_view password,
                                             bool load_index) {
    if (!backend->exists(kConfigName)) throw Error("no Vaultik repository at " + backend->location());
    RepoConfig config = parse_config(backend->get(kConfigName));
    const MasterKey key = unwrap_master_key(config, password);
    std::unique_ptr<Repository> repo(new Repository(std::move(backend), std::move(config), key));
    if (load_index) repo->load_indexes();
    try {
        if (repo->backend_->exists(kParityMarker)) {
            const Bytes b = repo->backend_->get(kParityMarker);
            const int v = std::atoi(std::string(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(b.size(), 8))).c_str());
            if (v >= 1 && v <= 50) repo->parity_percent_ = static_cast<unsigned>(v);
        }
    } catch (const std::exception&) {
        // unreadable: packs are written without repair data, as before
    }
    return repo;
}

void Repository::set_parity_percent(unsigned percent) {
    if (percent > 50) throw Error("repair data can be at most 50% of the data");
    if (percent == parity_percent_.load()) return;
    if (percent == 0) {
        backend_->remove(kParityMarker);
    } else {
        const std::string text = std::to_string(percent) + "\n";
        backend_->put(kParityMarker, ByteSpan(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }
    parity_percent_ = percent;
}

std::string Repository::parity_name(const Id& pack_id) {
    const std::string hex = id_hex(pack_id);
    return "parity/" + hex.substr(0, 2) + "/" + hex;
}

namespace {

// Whether a pack (its bytes) holds file data only; false when its header cannot be read.
bool data_only(ByteSpan pack, const AesGcm& aead) {
    try {
        const std::vector<BlobEntry> entries = parse_pack_header(pack, aead);
        return std::none_of(entries.begin(), entries.end(), [](const BlobEntry& e) { return e.type == BlobType::Tree; });
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace

void Repository::write_parity(const Id& pack_id, ByteSpan bytes, unsigned percent) {
    const Bytes parity = repo::make_recovery(bytes, percent);
    if (split_trees_ && data_only(bytes, *aead_)) backend_->put_data(parity_name(pack_id), parity);
    else backend_->put(parity_name(pack_id), parity);
}

void Repository::write_pack(const Id& pack_id, ByteSpan bytes) {
    if (split_trees_ && data_only(bytes, *aead_)) backend_->put_data(pack_name(pack_id), bytes);
    else backend_->put(pack_name(pack_id), bytes);
}

std::vector<bool> Repository::archived_packs() const {
    if (!backend_->data_needs_thaw()) return std::vector<bool>(index_.packs().size(), false);
    std::vector<bool> out(index_.packs().size(), true);
    index_.for_each([&out](const Id&, const BlobLocation& loc) {
        if (loc.type == BlobType::Tree && loc.pack < out.size()) out[loc.pack] = false;
    });
    return out;
}

bool Repository::rebuild_pack(const Id& pack_id, Bytes& pack) {
    Bytes parity;
    try {
        const std::string name = parity_name(pack_id);
        if (!backend_->exists(name)) return false;
        parity = backend_->get(name);
    } catch (const std::exception&) {
        return false;
    }
    try {
        Bytes work = pack;
        const repo::RecoveryResult r = repo::repair_with_recovery(work, parity);
        if (!(r.repaired || r.intact) || blake3(work) != pack_id) return false;
        pack = std::move(work);
        return true;
    } catch (const std::exception&) {
        return false;  // the repair data itself is damaged beyond use
    }
}

void Repository::use_pack_bytes(const Id& pack_id, Bytes bytes) {
    pack_bytes_[pack_id] = std::move(bytes);
}

Bytes Repository::read_pack_salvaging(const Id& pack_id) {
    const std::string name = pack_name(pack_id);
    const std::uint64_t size = backend_->size(name);
    if (size > (std::uint64_t{1} << 30)) throw Error("pack " + name + " is too large to be a pack");
    Bytes out(static_cast<std::size_t>(size));
    constexpr std::size_t kBig = 1u << 20;
    constexpr std::size_t kSmall = 64u << 10;
    for (std::size_t at = 0; at < out.size(); at += kBig) {
        const std::size_t len = std::min(kBig, out.size() - at);
        try {
            const Bytes part = backend_->get_range(name, at, len);
            std::memcpy(out.data() + at, part.data(), std::min(part.size(), len));
            continue;
        } catch (const std::exception&) {
        }
        for (std::size_t s = at; s < at + len; s += kSmall) {
            const std::size_t n = std::min(kSmall, at + len - s);
            try {
                const Bytes part = backend_->get_range(name, s, n);
                std::memcpy(out.data() + s, part.data(), std::min(part.size(), n));
            } catch (const std::exception&) {
                // left as zeros: a damaged part for the repair data to rebuild
            }
        }
    }
    return out;
}

Repository::Repository(std::unique_ptr<Backend> backend, RepoConfig config, const MasterKey& key)
    : backend_(std::move(backend)),
      config_(std::move(config)),
      chunker_(config_.chunker),
      aead_(std::make_unique<AesGcm>(key.enc)),
      lock_aead_(std::make_unique<AesGcm>(key.enc)),
      id_hasher_(key.id),
      compressor_(config_.zstd_level),
      split_trees_(!backend_->data_class().empty()),
      enc_key_(key.enc) {}

// Worker threads that seal blobs and upload packs; see set_parallelism.
struct Repository::Pipeline {
    struct Job {
        BlobType type = BlobType::Data;
        Id id{};
        Bytes plain;
        Bytes sealed;
        std::uint32_t raw_len = 0;
        bool done = false;
        std::string error;  // empty: sealed
    };
    struct Upload {
        Id pack_id{};
        Bytes bytes;
        std::vector<BlobEntry> entries;
        bool data = false;  // file data only: to the backend's data class
    };

    std::mutex mu;
    std::condition_variable work;  // a job or an upload is waiting, or stop
    std::condition_variable done;  // a job or an upload finished
    std::deque<std::shared_ptr<Job>> todo;   // waiting for a worker
    std::deque<std::shared_ptr<Job>> order;  // every job not yet in the pack, in submission order
    std::deque<Upload> uploads;              // waiting for an uploader
    std::size_t uploading = 0;
    std::vector<std::pair<Id, std::vector<BlobEntry>>> uploaded;  // written, not yet indexed
    std::uint64_t uploaded_bytes = 0;
    std::set<Id> in_flight;  // packs queued or being written
    // The first failure of a worker, as text (an exception object is not passed between threads);
    // never cleared.
    std::string failed;
    bool stop = false;
    unsigned threads = 0;
    unsigned max_uploads = 0;
    std::vector<std::thread> workers;

    ~Pipeline() {
        {
            std::lock_guard g(mu);
            stop = true;
        }
        work.notify_all();
        for (std::thread& t : workers) t.join();
    }
};

namespace {

// Compresses (when it helps) and encrypts an object; shared by the calling thread and workers.
Bytes seal_with(ZstdCompressor& compressor, const AesGcm& aead, ByteSpan plain, ByteSpan aad, bool compress) {
    if (plain.size() > std::numeric_limits<std::uint32_t>::max()) throw Error("object too large");
    ByteWriter header;
    Bytes payload;
    payload.reserve(plain.size() + 16);
    bool compressed = false;
    if (compress && plain.size() >= 64) {
        Bytes candidate;
        compressor.compress_append(plain, candidate);
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
    Bytes sealed = aead.seal(payload, aad);
    secure_wipe(payload.data(), payload.size());
    return sealed;
}

}  // namespace

Repository::~Repository() {
    pipe_.reset();  // waits for the upload in progress; queued packs are dropped (never indexed)
    secure_wipe(enc_key_.data(), enc_key_.size());
}

void Repository::set_parallelism(unsigned threads, unsigned uploads) {
    flush();
    pipe_.reset();
    if (threads == 0 && uploads == 0) return;
    pipe_ = std::make_unique<Pipeline>();
    Pipeline* p = pipe_.get();
    p->threads = threads;
    p->max_uploads = uploads;
    const int level = config_.zstd_level;
    for (unsigned i = 0; i < threads; ++i) {
        // Each worker its own cipher, made here: no copy of the key is left in a worker (or in
        // the lambda copies std::thread makes), where nothing would wipe it.
        std::shared_ptr<const AesGcm> cipher = std::make_shared<const AesGcm>(enc_key_);
        p->workers.emplace_back([p, level, cipher] {
            ZstdCompressor compressor(level);
            const AesGcm& aead = *cipher;
            std::unique_lock lk(p->mu);
            for (;;) {
                p->work.wait(lk, [p] { return p->stop || !p->todo.empty(); });
                if (p->stop) return;
                const std::shared_ptr<Pipeline::Job> job = p->todo.front();
                p->todo.pop_front();
                lk.unlock();
                try {
                    job->sealed = seal_with(compressor, aead, job->plain, blob_aad(job->type, job->id), true);
                } catch (const std::exception& e) {
                    job->error = std::string("cannot encrypt data: ") + e.what();
                } catch (...) {
                    job->error = "cannot encrypt data";
                }
                secure_wipe(job->plain.data(), job->plain.size());
                job->plain = Bytes();
                lk.lock();
                if (!job->error.empty() && p->failed.empty()) p->failed = job->error;
                job->done = true;
                p->done.notify_all();
            }
        });
    }
    Backend* backend = backend_.get();
    const std::atomic<unsigned>* parity = &parity_percent_;
    for (unsigned i = 0; i < uploads; ++i) {
        p->workers.emplace_back([p, backend, parity] {
            std::unique_lock lk(p->mu);
            for (;;) {
                p->work.wait(lk, [p] { return p->stop || !p->uploads.empty(); });
                if (p->stop) return;
                Pipeline::Upload u = std::move(p->uploads.front());
                p->uploads.pop_front();
                ++p->uploading;
                lk.unlock();
                std::string error;
                try {
                    if (u.data) backend->put_data(pack_name(u.pack_id), u.bytes);
                    else backend->put(pack_name(u.pack_id), u.bytes);
                    put_parity(*backend, u.pack_id, u.bytes, parity->load(), u.data);
                } catch (const std::exception& e) {
                    error = std::string("cannot write a pack: ") + e.what();
                } catch (...) {
                    error = "cannot write a pack";
                }
                lk.lock();
                --p->uploading;
                if (!error.empty()) {
                    if (p->failed.empty()) p->failed = error;
                    p->in_flight.erase(u.pack_id);
                } else {
                    p->uploaded_bytes += u.bytes.size();
                    p->uploaded.emplace_back(u.pack_id, std::move(u.entries));
                }
                p->done.notify_all();
            }
        });
    }
}

void Repository::check_pipeline() {
    if (!write_failed_.empty()) throw Error(write_failed_);
    if (!pipe_) return;
    std::string failed;
    {
        std::lock_guard g(pipe_->mu);
        failed = pipe_->failed;
    }
    if (!failed.empty()) throw Error(failed);
}

void Repository::drain(bool all) {
    if (!pipe_) return;
    check_pipeline();
    Pipeline& p = *pipe_;
    for (;;) {
        std::shared_ptr<Pipeline::Job> job;
        {
            std::unique_lock lk(p.mu);
            if (p.order.empty()) return;
            if (!p.order.front()->done) {
                if (!all) return;
                p.done.wait(lk, [&p] { return p.order.front()->done; });
            }
            job = p.order.front();
            p.order.pop_front();
        }
        if (!job->error.empty()) throw Error(job->error);
        PackWriter& writer = writer_for(job->type);
        writer.add(job->type, job->id, job->sealed, job->raw_len);
        if (writer.size() >= kPackTargetSize) finish_pack(writer);
    }
}

void Repository::collect_uploads(bool all) {
    if (!pipe_) return;
    Pipeline& p = *pipe_;
    std::vector<std::pair<Id, std::vector<BlobEntry>>> ready;
    std::uint64_t bytes = 0;
    {
        std::unique_lock lk(p.mu);
        if (all) p.done.wait(lk, [&p] { return p.uploads.empty() && p.uploading == 0; });
        if (!p.failed.empty()) throw Error(p.failed);
        ready.swap(p.uploaded);
        bytes = p.uploaded_bytes;
        p.uploaded_bytes = 0;
        for (const auto& r : ready) p.in_flight.erase(r.first);
    }
    write_stats_.stored_bytes_added += bytes;
    write_stats_.packs_written += ready.size();
    for (auto& r : ready) index_pending_.push_back(std::move(r));
    if (index_pending_.size() >= kPacksPerIndexFile) write_index();
}

std::string Repository::pack_name(const Id& pack_id) {
    const std::string hex = id_hex(pack_id);
    return "data/" + hex.substr(0, 2) + "/" + hex;
}

std::string Repository::snapshot_name(const Id& id) {
    return std::string(kSnapshotDir) + "/" + id_hex(id);
}

Bytes Repository::seal_object(ByteSpan plain, ByteSpan aad, bool compress) {
    return seal_with(compressor_, *aead_, plain, aad, compress);
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
    return save_blob_with_id(type, plain, id_out);
}

bool Repository::save_blob_with_id(BlobType type, ByteSpan plain, const Id& id) {
    // A wrong id would store the content under another blob's name; the backup's readers compute
    // it with the same hasher, so this only checks the caller in debug builds.
    assert(id == blob_id(type, plain));
    check_pipeline();  // after a failed background write nothing more may count as stored
    const Id& id_out = id;
    if (has_blob(id_out)) return false;

    pack_pending_.insert(id_out);
    ++write_stats_.blobs_added;
    write_stats_.raw_bytes_added += plain.size();
    if (pipe_ && pipe_->threads > 0) {
        // A worker compresses and encrypts it; drain() puts it into the pack in this order.
        auto job = std::make_shared<Pipeline::Job>();
        job->type = type;
        job->id = id_out;
        job->plain.assign(plain.begin(), plain.end());
        job->raw_len = static_cast<std::uint32_t>(plain.size());
        Pipeline& p = *pipe_;
        const std::size_t limit = 2 * static_cast<std::size_t>(p.threads) + 2;  // chunks held in memory
        {
            std::lock_guard g(p.mu);
            p.order.push_back(job);
            p.todo.push_back(std::move(job));
        }
        p.work.notify_all();
        drain(false);
        for (;;) {
            {
                std::unique_lock lk(p.mu);
                if (p.order.size() <= limit) break;
                p.done.wait(lk, [&p] { return p.order.front()->done; });
            }
            drain(false);
        }
        return true;
    }
    const Bytes sealed = seal_object(plain, blob_aad(type, id_out), true);
    PackWriter& writer = writer_for(type);
    writer.add(type, id_out, sealed, static_cast<std::uint32_t>(plain.size()));
    if (writer.size() >= kPackTargetSize) finish_pack(writer);
    return true;
}

Bytes Repository::decode_blob(BlobType type, const Id& id, ByteSpan sealed) {
    Bytes plain = open_object(sealed, blob_aad(type, id));
    if (blob_id(type, plain) != id) throw AuthError("blob content does not match its id: " + id_hex(id));
    return plain;
}

Bytes Repository::load_blob(BlobType type, const Id& id) {
    check_pipeline();
    if (pack_pending_.contains(id)) flush();  // the blob is still in memory; write it out first
    const BlobLocation* loc = index_.find(id);
    if (loc == nullptr) throw Error(std::string(blob_type_name(type)) + " blob not found: " + id_hex(id));
    if (loc->type != type) throw Error("blob has an unexpected type: " + id_hex(id));
    if (pipe_ && pipe_->max_uploads > 0) {
        bool flying = false;
        {
            std::lock_guard g(pipe_->mu);
            flying = pipe_->in_flight.contains(index_.packs()[loc->pack]);
        }
        if (flying) collect_uploads(true);  // its pack is still being written
    }
    const Id& pack_id = index_.packs()[loc->pack];
    Bytes sealed;
    if (const auto held = pack_bytes_.find(pack_id); held != pack_bytes_.end()) {
        if (loc->offset + loc->length > held->second.size()) throw Error("malformed pack: a blob lies outside it");
        sealed.assign(held->second.begin() + static_cast<std::ptrdiff_t>(loc->offset),
                      held->second.begin() + static_cast<std::ptrdiff_t>(loc->offset + loc->length));
    } else {
        sealed = backend_->get_range(pack_name(pack_id), loc->offset, loc->length);
    }
    return decode_blob(type, id, sealed);
}

void Repository::finish_pack() {
    finish_pack(pack_);
    finish_pack(tree_pack_);
}

void Repository::finish_pack(PackWriter& writer) {
    if (writer.empty()) return;
    const bool data = split_trees_ && &writer == &pack_;  // file data only (see splits_trees)
    std::vector<BlobEntry> entries;
    const Bytes bytes = writer.finish(*aead_, entries);
    const Id pack_id = blake3(bytes);
    const bool background = pipe_ && pipe_->max_uploads > 0;
    if (!background) {
        // Written before its blobs count as stored; a failed write is final, like a failed
        // background one, since those blobs are in no pack now.
        try {
            if (data) backend_->put_data(pack_name(pack_id), bytes);
            else backend_->put(pack_name(pack_id), bytes);
        } catch (const std::exception& e) {
            write_failed_ = std::string("cannot write a pack: ") + e.what();
            throw;
        }
        put_parity(*backend_, pack_id, bytes, parity_percent_.load(), data);
    }
    // Known at once (deduplication); listed in an index file once the pack is written.
    const std::uint32_t pos = index_.add_pack(pack_id);
    for (const BlobEntry& e : entries) {
        index_.add(e, pos);
        pack_pending_.erase(e.id);  // (others may still be with the workers)
    }
    if (background) {
        Pipeline& p = *pipe_;
        {
            std::unique_lock lk(p.mu);
            p.done.wait(lk, [&p] { return p.uploads.size() + p.uploading < p.max_uploads || !p.failed.empty(); });
            if (!p.failed.empty()) throw Error(p.failed);
            p.in_flight.insert(pack_id);
            p.uploads.push_back({pack_id, std::move(bytes), std::move(entries), data});
        }
        p.work.notify_all();
        collect_uploads(false);
        return;
    }
    write_stats_.stored_bytes_added += bytes.size();
    ++write_stats_.packs_written;
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
    drain(true);  // keep the order of blobs already handed to the workers
    if (pack_pending_.contains(id)) return;
    PackWriter& writer = writer_for(type);
    writer.add(type, id, sealed, raw_len);
    pack_pending_.insert(id);
    if (writer.size() >= kPackTargetSize) finish_pack(writer);
}

void Repository::flush() {
    check_pipeline();
    drain(true);
    finish_pack();
    collect_uploads(true);
    write_index();
}

void Repository::clear_index() {
    index_ = Index();
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
