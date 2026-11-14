#include "vaultik/ops/backup.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "vaultik/chunker/fastcdc.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

bool wildcard_match(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

class Excluder {
public:
    explicit Excluder(const std::vector<std::string>& patterns) {
        for (std::string p : patterns) {
#ifdef _WIN32
            std::replace(p.begin(), p.end(), '/', '\\');
#endif
            patterns_.push_back(os::fold_case(p));
        }
    }

    bool excluded(std::string_view name, const std::string& full) const {
        if (patterns_.empty()) return false;
        const std::string lname = os::fold_case(name);
        std::string lfull;
        for (const std::string& p : patterns_) {
            if (p.find(os::kSeparator) != std::string::npos) {
                if (lfull.empty()) lfull = os::fold_case(full);
                if (wildcard_match(p, lfull)) return true;
            } else if (wildcard_match(p, lname)) {
                return true;
            }
        }
        return false;
    }

private:
    std::vector<std::string> patterns_;
};

Node node_from_info(const os::FileInfo& info, std::string name) {
    Node n;
    n.name = std::move(name);
    n.attributes = info.attributes;
    n.mode = info.mode;
    n.uid = info.uid;
    n.gid = info.gid;
    n.ctime = info.ctime;
    n.mtime = info.mtime;
    n.size = info.size;
    return n;
}

// Virtual directories above the backed-up sources (drive letter and parent folders).
struct VirtualDir {
    std::map<std::string, VirtualDir> dirs;
    std::vector<Node> leaves;
};

std::string paths_key(const std::vector<std::string>& paths) {
    std::vector<std::string> folded;
    folded.reserve(paths.size());
    for (const auto& p : paths) folded.push_back(os::fold_case(p));
    std::sort(folded.begin(), folded.end());
    std::string key;
    for (const auto& p : folded) key += p + "\n";
    return key;
}

// Reads, chunks and hashes several files at once (BackupOptions::read_threads). The repository
// is not thread-safe, so the readers only use what never changes in it (the chunker and the
// keyed hasher) and hand each chunk with its id to the engine's thread through a small queue
// per file; the engine takes the files in the order they were submitted and stores everything
// there. Readers start a file only while it is among the next 2 x threads ones the engine has
// not finished, so memory stays bounded however many files are submitted. Because readers take
// files strictly in order, the file the engine waits for (the oldest one) was always taken
// before any file a reader is blocked on, so there is no deadlock.
class ReaderPool {
public:
    struct Piece {
        Bytes data;
        Id id{};
    };

    // One file to read. Everything but io_path is guarded by the pool's mutex until `done`.
    struct FileJob {
        std::string io_path;
        bool opened = false;  // opening was tried (open_error says how it went)
        std::uint32_t names = 1;  // the file's number of names, once opened
        os::ErrorCode open_error = 0;
        std::deque<Piece> pieces;
        std::size_t queued = 0;      // bytes in `pieces`
        bool random_start = false;   // the first chunk looks like encrypted data
        bool done = false;           // no more pieces will come
        std::string read_error;      // reading failed part way (an os::OsError)
        std::exception_ptr failure;  // anything else, thrown again on the engine's thread
        bool abandoned = false;      // the engine no longer wants it (an error, or a cancel)
    };

    ReaderPool(const Repository& repo, unsigned threads) : repo_(repo), window_(2 * std::size_t{threads}) {
        try {
            for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this] { run(); });
        } catch (...) {
            shutdown();
            throw;
        }
    }
    ~ReaderPool() { shutdown(); }
    ReaderPool(const ReaderPool&) = delete;
    ReaderPool& operator=(const ReaderPool&) = delete;

    // Queues files to read after those submitted before; the engine takes them in this order.
    void submit(std::vector<std::shared_ptr<FileJob>> jobs) {
        if (jobs.empty()) return;
        {
            std::lock_guard g(mu_);
            for (auto& job : jobs) queue_.push_back(std::move(job));
        }
        to_readers_.notify_all();
    }

    // Drops every file the engine has not finished (it is unwinding, or will not take them).
    void abandon_all() {
        {
            std::lock_guard g(mu_);
            abandon_locked();
        }
        to_readers_.notify_all();
    }

    // Files submitted that the engine has not finished yet.
    std::size_t pending() {
        std::lock_guard g(mu_);
        return queue_.size();
    }

    // Whether `job` is the oldest unfinished file, the only one the engine may take next.
    bool is_next(const FileJob* job) {
        std::lock_guard g(mu_);
        return !queue_.empty() && queue_.front().get() == job;
    }

    // Waits until the file was opened and returns the error (0 when it opened). `idle` runs
    // every 100 ms while waiting, so a cancel is seen even while a read hangs.
    os::ErrorCode wait_open(FileJob& job, const std::function<void()>& idle) {
        std::unique_lock lk(mu_);
        wait(lk, idle, [&] { return job.opened; });
        return job.open_error;
    }

    // The file's next chunk; false at its end (then see read_error and failure).
    bool next(FileJob& job, Piece& out, const std::function<void()>& idle) {
        std::unique_lock lk(mu_);
        wait(lk, idle, [&] { return !job.pieces.empty() || job.done; });
        if (job.pieces.empty()) return false;
        out = std::move(job.pieces.front());
        job.pieces.pop_front();
        job.queued -= out.data.size();
        lk.unlock();
        to_readers_.notify_all();
        return true;
    }

    // The engine is done with the oldest file: one more may be started.
    void finish() {
        {
            std::lock_guard g(mu_);
            queue_.pop_front();
            --taken_;  // a file the engine finished was always taken (opened) by a reader
        }
        to_readers_.notify_all();
    }

private:
    // Chunks one file may have waiting (a single larger chunk is always let through).
    static constexpr std::size_t kQueueBytes = 4u << 20;

    template <class Ready>
    void wait(std::unique_lock<std::mutex>& lk, const std::function<void()>& idle, Ready ready) {
        while (!ready()) {
            if (to_engine_.wait_for(lk, std::chrono::milliseconds(100)) == std::cv_status::timeout && !ready()) {
                lk.unlock();
                idle();  // may throw (Cancelled); the lock is not held then
                lk.lock();
            }
        }
    }

    void abandon_locked() {
        for (const auto& job : queue_) job->abandoned = true;
        queue_.clear();
        taken_ = 0;
    }

    void shutdown() {
        {
            std::lock_guard g(mu_);
            stop_ = true;
            abandon_locked();
        }
        to_readers_.notify_all();
        for (std::thread& t : threads_)
            if (t.joinable()) t.join();
    }

    void run() {
        Bytes buffer;  // each reader chunks with its own buffer
        for (;;) {
            std::shared_ptr<FileJob> job;
            {
                std::unique_lock lk(mu_);
                to_readers_.wait(lk, [&] { return stop_ || (taken_ < queue_.size() && taken_ < window_); });
                if (stop_) return;
                job = queue_[taken_++];
            }
            read(*job, buffer);
        }
    }

    void read(FileJob& job, Bytes& buffer) {
        try {
            os::ErrorCode err = 0;
            os::File file = os::File::open_source(job.io_path, err);
            {
                std::lock_guard g(mu_);
                job.opened = true;
                job.open_error = file.valid() ? 0 : err;
                if (file.valid()) job.names = file.names();
                job.done = !file.valid();
            }
            to_engine_.notify_all();
            if (!file.valid()) return;
            ChunkReader reader(
                repo_.chunker(), [&file](std::uint8_t* dst, std::size_t max) { return file.read(dst, max); },
                buffer);
            bool first = true;
            ByteSpan chunk;
            try {
                while (reader.next(chunk)) {
                    Piece p;
                    p.id = repo_.blob_id(BlobType::Data, chunk);
                    p.data.assign(chunk.begin(), chunk.end());
                    const bool random =
                        first && byte_entropy(chunk.first(std::min<std::size_t>(chunk.size(), 65536))) > 7.9;
                    if (!push(job, std::move(p), first, random)) return;
                    first = false;
                }
            } catch (const os::OsError& e) {
                complete(job, e.what(), nullptr);
                return;
            }
            complete(job, {}, nullptr);
        } catch (...) {
            complete(job, {}, std::current_exception());
        }
    }

    // Waits for room in the file's queue; false when the file is no longer wanted.
    bool push(FileJob& job, Piece p, bool first, bool random) {
        std::unique_lock lk(mu_);
        to_readers_.wait(lk, [&] {
            return stop_ || job.abandoned || job.queued == 0 || job.queued + p.data.size() <= kQueueBytes;
        });
        if (stop_ || job.abandoned) return false;
        if (first) job.random_start = random;
        job.queued += p.data.size();
        job.pieces.push_back(std::move(p));
        lk.unlock();
        to_engine_.notify_all();
        return true;
    }

    void complete(FileJob& job, std::string read_error, std::exception_ptr failure) {
        {
            std::lock_guard g(mu_);
            job.opened = true;
            job.done = true;
            job.read_error = std::move(read_error);
            job.failure = std::move(failure);
        }
        to_engine_.notify_all();
    }

    const Repository& repo_;
    const std::size_t window_;
    std::mutex mu_;
    std::condition_variable to_readers_;  // work, or room in a queue
    std::condition_variable to_engine_;   // a file opened, a chunk, or a file's end
    std::deque<std::shared_ptr<FileJob>> queue_;  // submitted, not finished by the engine, oldest first
    std::size_t taken_ = 0;                       // how many of them readers have taken
    bool stop_ = false;
    std::vector<std::thread> threads_;
};

// A folder listed ahead (with several readers), its files to read already submitted to them.
struct PreparedDir {
    struct Entry {
        std::string path;
        os::FileInfo info;
        Node node;
        const Node* pnode = nullptr;  // the entry in the parent snapshot
        std::shared_ptr<ReaderPool::FileJob> job;
    };
    std::string dir;
    std::uint64_t device = 0;
    std::optional<Tree> own_parent;  // the parent snapshot's listing, when loaded ahead
    const Tree* parent = nullptr;
    bool unlisted = false;  // could not be listed at all
    os::ErrorCode list_error = 0;
    bool stopped_early = false;  // the listing broke off
    os::ErrorCode stop_error = 0;
    std::vector<Entry> entries;
};

class BackupEngine {
public:
    BackupEngine(Repository& repo, const BackupOptions& options)
        : repo_(repo), options_(options), excluder_(options.excludes) {}

    BackupResult run();

private:
    struct Source {
        std::string full;                // absolute
        std::vector<std::string> comps;  // tree components
    };

    std::vector<Source> resolve_sources();
    std::optional<Id> find_parent(const std::vector<std::string>& paths);
    std::optional<Tree> load_tree(const Id& id);
    std::optional<Node> lookup(const Tree& root, const std::vector<std::string>& comps);
    Tree scan_children(const std::string& dir, std::uint64_t device, const Tree* parent);
    Id scan_dir(const std::string& dir, std::uint64_t device, const Tree* parent);
    // A folder the change journal says is unchanged: its parent listing, with each subfolder
    // asked again. nullopt (nothing counted yet) when the parent's data is not all there.
    std::optional<Id> reuse_dir(const std::string& dir, std::uint64_t device, const Tree& parent);
    // Whether the change journal lets folder `dir` be taken from the parent version. Not inside
    // a folder that was itself renamed, moved or created since then: the parent version's
    // folder of the same name may be another folder, so the names of its subfolders say nothing.
    bool journal_unchanged(const std::string& dir);
    bool use_journal_ = false;
    std::vector<std::string> moved_dirs_;  // folders the journal has records of themselves
    std::uint64_t dirs_reused_ = 0;
    // `job`: a file the readers are reading (read_threads > 1), taken from them instead of
    // opened here.
    bool scan_entry(const std::string& path, const os::FileInfo& info, std::uint64_t parent_device, Node& node,
                    const Node* pnode, ReaderPool::FileJob* job = nullptr);
    bool scan_file(const std::string& path, Node& node, const Node* parent, ReaderPool::FileJob* job);
    // Whether a file is unchanged since the parent snapshot, so its chunk list can be reused.
    bool reusable(const Node& node, const Node* parent) const;
    // A listed file that could be reused: on Windows the listing shows a hard-linked file's size
    // and times as of the last change through that name, so the file itself is asked when it
    // had several names, when `all` (the change journal says a file with another name in this
    // folder changed) or when the job asks for every file. Leaves `info` as listed when that fails.
    void refresh_info(const std::string& path, os::FileInfo& info, const Node* pnode, bool all) const;
    // Whether every file listed in `dir` is to be asked (see BackupOptions::refresh_dir).
    bool refresh_all_in(const std::string& dir) const {
        return use_journal_ && options_.refresh_dir && options_.refresh_dir(dir);
    }
    bool take_file(const std::string& path, Node& node, const Node* parent, ReaderPool::FileJob& job);
    bool file_read(const std::string& path, Node& node, const Node* parent, std::uint64_t total, bool random_start);
    // With several readers, folders are listed ahead of the engine (see look_ahead) and their
    // files submitted to the readers in exactly the order the engine will take them: a folder's
    // files in listing order, then its subfolders one by one, depth first.
    std::unique_ptr<ReaderPool> pool_;
    std::shared_ptr<PreparedDir> prepare(const std::string& dir, std::uint64_t device, const Tree* parent,
                                         std::optional<Tree> own_parent);
    // The folder the engine enters now: the one listed ahead, or listed here.
    std::shared_ptr<PreparedDir> take_prepared(const std::string& dir, std::uint64_t device, const Tree* parent);
    // The next folder listed ahead, when it is `dir`.
    const PreparedDir* upcoming(const std::string& dir) const {
        return !ahead_.empty() && ahead_.front()->dir == dir ? ahead_.front().get() : nullptr;
    }
    // Lists folders ahead of the engine, depth first, while few files wait for the readers. It
    // stops at a folder the engine may not read (one the change journal may let it reuse, or
    // another file system), since what the engine does there cannot be told in advance.
    void look_ahead();
    void drop_lookahead();
    void scan_listing(PreparedDir& dir, Tree& tree);
    struct WalkFrame {
        std::shared_ptr<PreparedDir> dir;
        std::size_t next = 0;  // the next entry to look at for a subfolder
    };
    std::deque<std::shared_ptr<PreparedDir>> ahead_;  // listed, not yet entered by the engine
    std::vector<WalkFrame> walk_;
    // Windows extras: the security descriptor (stored once per distinct one), the alternate
    // data streams of a file, and the target of a symlink or junction.
    void scan_security(const std::string& path, Node& node);
    void scan_streams(const std::string& path, Node& node);
    bool store_stream(os::File& file, std::vector<Id>& content, std::uint64_t& total);
    std::map<Bytes, Id> security_ids_;
    // Whether anything differs from the parent snapshot (for skip_unchanged): an item added, or
    // one whose content or metadata changed. A folder's own times are left out: NTFS updates
    // them lazily, so they drift without any change, and an added or removed entry counts anyway.
    bool changed_ = false;
    void note_change(const Node& n, const Node* old) {
        if (old == nullptr) {
            changed_ = true;
            return;
        }
        const bool same = n.kind == old->kind && n.attributes == old->attributes && n.mode == old->mode &&
                          n.uid == old->uid && n.gid == old->gid && n.size == old->size && n.content == old->content &&
                          n.link_target == old->link_target && n.security == old->security &&
                          n.streams == old->streams && n.link == old->link &&
                          (n.kind == NodeKind::Dir || (n.mtime == old->mtime && n.ctime == old->ctime));
        if (!same) changed_ = true;
    }
    bool all_present(const std::vector<Id>& ids) const;
    Id save_virtual(const VirtualDir& dir);
    // An ExtraSource: its node, named after its place. False (with a warning) when it was skipped.
    bool scan_extra(const ExtraSource& x, Node& node, const Node* pnode, std::uint64_t time);
    bool scan_stream(const ExtraSource& x, Node& node, std::uint64_t time);
    void warn(const std::string& message);
    void report(bool force = false);
    void check_cancel();
    // The path to do I/O on for a real path (a shadow copy, or the path itself).
    std::string io(const std::string& path) const { return options_.read_path ? options_.read_path(path) : path; }

    Repository& repo_;
    const BackupOptions& options_;
    Excluder excluder_;
    Bytes buffer_;
    SnapshotStats stats_;
    ChangeAlarm alarm_;
    std::vector<std::string> missing_sources_;
    BackupProgress progress_;

    void suspicious(const std::string& path) {
        if (alarm_.examples.size() < 5) alarm_.examples.push_back(path);
    }
    // Whether the stored chunks of a file's earlier content compressed well (index sizes).
    bool was_compressible(const std::vector<Id>& content) const;
    std::chrono::steady_clock::time_point last_report_{};
};

bool BackupEngine::was_compressible(const std::vector<Id>& content) const {
    std::uint64_t raw = 0, stored = 0;
    for (std::size_t i = 0; i < content.size() && i < 4; ++i) {
        const BlobLocation* loc = repo_.index().find(content[i]);
        if (loc == nullptr) return false;
        raw += loc->raw_len;
        stored += loc->length;
    }
    return raw >= 16384 && stored * 10 < raw * 9;  // saved more than 10%
}

void BackupEngine::warn(const std::string& message) {
    ++stats_.errors;
    if (options_.on_warning) options_.on_warning(message);
}

void BackupEngine::report(bool force) {
    if (!options_.on_progress) return;
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last_report_ < std::chrono::milliseconds(200)) return;
    last_report_ = now;
    progress_.files = stats_.files;
    progress_.dirs = stats_.dirs;
    progress_.bytes = stats_.bytes;
    progress_.new_bytes = stats_.new_bytes;
    options_.on_progress(progress_);
}

void BackupEngine::check_cancel() {
    if (options_.should_cancel && options_.should_cancel()) throw Cancelled();
}

std::vector<BackupEngine::Source> BackupEngine::resolve_sources() {
    if (options_.paths.empty() && options_.extras.empty()) throw Error("nothing to back up: no paths given");
    std::vector<Source> sources;
    for (const std::string& p : options_.paths) {
        Source s;
        s.full = os::absolute_path(p);
        s.comps = os::path_components(s.full);
        sources.push_back(std::move(s));
    }
    // Reject duplicates and nesting (C:\a together with C:\a\b): one would hide the other.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        for (std::size_t j = 0; j < sources.size(); ++j) {
            if (i == j) continue;
            const auto& a = sources[i].comps;
            const auto& b = sources[j].comps;
            if (a.size() > b.size()) continue;
            bool prefix = true;
            for (std::size_t k = 0; k < a.size() && prefix; ++k) prefix = os::fold_case(a[k]) == os::fold_case(b[k]);
            if (prefix) throw Error("overlapping backup paths: " + sources[i].full + " and " + sources[j].full);
        }
    }
    return sources;
}

std::optional<Id> BackupEngine::find_parent(const std::vector<std::string>& paths) {
    if (options_.parent) return options_.parent;
    if (!options_.use_parent) return std::nullopt;
    const std::string key = paths_key(paths);
    const std::string host = os::hostname();
    std::optional<Id> best;
    std::uint64_t best_time = 0;
    for (const Id& id : repo_.list_snapshot_ids()) {
        Snapshot s;
        try {
            s = repo_.load_snapshot(id);
        } catch (const Error&) {
            continue;  // unreadable snapshots are reported by verify, not here
        }
        if (s.hostname == host && paths_key(s.paths) == key && s.time > best_time) {
            best = id;
            best_time = s.time;
        }
    }
    return best;
}

std::optional<Tree> BackupEngine::load_tree(const Id& id) {
    try {
        return parse_tree(repo_.load_blob(BlobType::Tree, id));
    } catch (const Error& e) {
        // Not fatal: the files are simply read again instead of being reused.
        if (options_.on_warning) options_.on_warning(std::string("parent snapshot tree unreadable: ") + e.what());
        return std::nullopt;
    }
}

std::optional<Node> BackupEngine::lookup(const Tree& root, const std::vector<std::string>& comps) {
    const Tree* tree = &root;
    std::optional<Tree> holder;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        const Node* n = tree->find(comps[i]);
        if (n == nullptr) return std::nullopt;
        if (i + 1 == comps.size()) return *n;
        if (n->kind != NodeKind::Dir) return std::nullopt;
        std::optional<Tree> next = load_tree(n->subtree);
        if (!next) return std::nullopt;
        holder = std::move(next);
        tree = &*holder;
    }
    return std::nullopt;
}

bool BackupEngine::all_present(const std::vector<Id>& ids) const {
    return std::all_of(ids.begin(), ids.end(), [&](const Id& id) { return repo_.has_blob(id); });
}

void BackupEngine::scan_security(const std::string& path, Node& node) {
    const Bytes sd = os::read_security(io(path));
    if (sd.empty()) return;
    if (const auto it = security_ids_.find(sd); it != security_ids_.end()) {
        node.security = it->second;
        return;
    }
    Id id;
    if (repo_.save_blob(BlobType::Data, sd, id)) stats_.new_bytes += sd.size();
    if (security_ids_.size() < 10000) security_ids_.emplace(sd, id);
    node.security = id;
}

bool BackupEngine::store_stream(os::File& file, std::vector<Id>& content, std::uint64_t& total) {
    ChunkReader reader(
        repo_.chunker(), [&file](std::uint8_t* dst, std::size_t max) { return file.read(dst, max); }, buffer_);
    ByteSpan chunk;
    while (reader.next(chunk)) {
        check_cancel();
        Id id;
        if (repo_.save_blob(BlobType::Data, chunk, id)) stats_.new_bytes += chunk.size();
        content.push_back(id);
        total += chunk.size();
    }
    return true;
}

void BackupEngine::scan_streams(const std::string& path, Node& node) {
    for (const os::StreamEntry& s : os::list_streams(io(path))) {
        os::ErrorCode err = 0;
        os::File f = os::File::open_source(os::stream_path(io(path), s.name), err);
        if (!f.valid()) {
            warn("skipped the stream " + s.name + " of " + path + ": " + os::error_message(err));
            continue;
        }
        DataStream ds;
        ds.name = s.name;
        try {
            store_stream(f, ds.content, ds.size);
        } catch (const os::OsError& e) {
            warn("skipped the stream " + s.name + " of " + path + ": " + e.what());
            continue;
        }
        node.streams.push_back(std::move(ds));
    }
}

void BackupEngine::refresh_info(const std::string& path, os::FileInfo& info, const Node* pnode, bool all) const {
#ifdef _WIN32
    if (info.type != os::EntryType::File || pnode == nullptr || pnode->kind != NodeKind::File) return;
    if (pnode->names <= 1 && !all && !options_.check_every_file) return;
    os::FileInfo fresh;
    os::ErrorCode err = 0;
    if (os::stat(io(path), fresh, err) && fresh.type == os::EntryType::File) info = fresh;
#else
    // lstat, which the listing uses, reads the file's own record.
    (void)path;
    (void)info;
    (void)pnode;
    (void)all;
#endif
}

bool BackupEngine::reusable(const Node& node, const Node* parent) const {
    // Unchanged since the parent snapshot: reuse its chunk list without reading the file. Its
    // streams too: writing a stream changes the file's modification time.
    const auto streams_present = [&](const Node& p) {
        return std::all_of(p.streams.begin(), p.streams.end(),
                           [&](const DataStream& s) { return all_present(s.content); });
    };
    return parent != nullptr && parent->kind == NodeKind::File && parent->size == node.size &&
           parent->mtime == node.mtime && parent->ctime == node.ctime && all_present(parent->content) &&
           streams_present(*parent);
}

bool BackupEngine::take_file(const std::string& path, Node& node, const Node* parent, ReaderPool::FileJob& job) {
    assert(pool_->is_next(&job));
    const std::function<void()> idle = [this] { check_cancel(); };
    const os::ErrorCode err = pool_->wait_open(job, idle);
    if (err != 0) {
        pool_->finish();
        warn("skipped " + path + ": " + os::error_message(err));
        return false;
    }
    node.names = job.names > 1 ? job.names : 0;
    progress_.current = path;
    report();
    std::uint64_t total = 0;
    ReaderPool::Piece piece;
    while (pool_->next(job, piece, idle)) {
        check_cancel();  // also inside a large file: a pause or stop does not wait for its end
        if (repo_.save_blob_with_id(BlobType::Data, piece.data, piece.id)) stats_.new_bytes += piece.data.size();
        node.content.push_back(piece.id);
        total += piece.data.size();
        stats_.bytes += piece.data.size();
        report();
    }
    pool_->finish();
    // The reader is done with the job: what it left there is no longer written to.
    if (job.failure) std::rethrow_exception(job.failure);
    if (!job.read_error.empty()) {
        warn("skipped " + path + ": " + job.read_error);
        return false;
    }
    return file_read(path, node, parent, total, job.random_start);
}

bool BackupEngine::file_read(const std::string& path, Node& node, const Node* parent, std::uint64_t total,
                             bool random_start) {
    const bool is_new = parent == nullptr || parent->kind != NodeKind::File;
    node.size = total;  // the file may have changed size since it was listed
    scan_streams(path, node);
    if (is_new) {
        ++stats_.files_new;
    } else if (node.content != parent->content) {  // a touched but identical file is not a change
        ++stats_.files_changed;
        if (random_start && total >= 16384 && was_compressible(parent->content)) {
            ++alarm_.became_random;
            suspicious(path);
        }
    }
    return true;
}

bool BackupEngine::scan_file(const std::string& path, Node& node, const Node* parent, ReaderPool::FileJob* job) {
    // The readers took this file because it could not be reused when the folder was listed.
    if (job != nullptr) return take_file(path, node, parent, *job);
    if (reusable(node, parent)) {
        node.content = parent->content;
        node.names = parent->names;
        // A version made before streams were kept has no extras for any file: look once more.
        // (One made since carries the file's security descriptor, so an empty list is real.)
        if (parent->streams.empty() && !parent->security) scan_streams(path, node);
        else node.streams = parent->streams;
        stats_.bytes += node.size;
        report();
        return true;
    }

    os::ErrorCode err = 0;
    os::File file = os::File::open_source(io(path), err);
    if (!file.valid()) {
        warn("skipped " + path + ": " + os::error_message(err));
        return false;
    }
    const std::uint32_t names = file.names();
    node.names = names > 1 ? names : 0;

    progress_.current = path;
    report();
    ChunkReader reader(
        repo_.chunker(), [&file](std::uint8_t* dst, std::size_t max) { return file.read(dst, max); }, buffer_);

    std::uint64_t total = 0;
    bool random_start = false;  // the file's first chunk looks like encrypted data
    try {
        ByteSpan chunk;
        while (reader.next(chunk)) {
            if (total == 0) random_start = byte_entropy(chunk.first(std::min<std::size_t>(chunk.size(), 65536))) > 7.9;
            check_cancel();  // also inside a large file: a pause or stop does not wait for its end
            Id id;
            if (repo_.save_blob(BlobType::Data, chunk, id)) stats_.new_bytes += chunk.size();
            node.content.push_back(id);
            total += chunk.size();
            stats_.bytes += chunk.size();
            report();
        }
    } catch (const os::OsError& e) {
        warn("skipped " + path + ": " + e.what());
        return false;
    }
    return file_read(path, node, parent, total, random_start);
}

bool BackupEngine::scan_entry(const std::string& path, const os::FileInfo& info, std::uint64_t parent_device,
                              Node& node, const Node* pnode, ReaderPool::FileJob* job) {
    scan_security(path, node);
    switch (info.type) {
        case os::EntryType::Other: {
            // A cloud file only online: downloaded and backed up when the job asks for it (reading
            // it makes its sync program download it), otherwise counted on its own.
            if (os::is_cloud_only(info)) {
                if (options_.download_cloud_files)
                    return scan_entry(path, os::downloaded_form(info), parent_device, node, pnode, job);
                node.kind = NodeKind::Reparse;
                node.size = 0;
                ++stats_.cloud_only;
                stats_.cloud_only_bytes += info.size;
                return true;
            }
            // Stored as metadata only; following links could loop or leave the backup set. A
            // symlink's or junction's target is kept, so a restore can bring the link back.
            node.kind = NodeKind::Reparse;
            node.size = 0;
            os::ReparseLink link;
            if (os::read_link(io(path), link)) node.link = LinkInfo{link.tag, link.target, link.relative};
            ++stats_.skipped;
            return true;
        }
        case os::EntryType::Symlink: {
            node.kind = NodeKind::Symlink;
            node.size = 0;
            const os::ErrorCode err = os::read_symlink(io(path), node.link_target);
            if (err != 0) {
                warn("cannot read link " + path + ": " + os::error_message(err));
                return false;
            }
            ++stats_.files;
            return true;
        }
        case os::EntryType::Dir: {
            node.kind = NodeKind::Dir;
            node.size = 0;
            // Another file system mounted here: skip kernel pseudo file systems such as /proc.
            if (info.device != parent_device && os::is_pseudo_filesystem(io(path))) {
                ++stats_.skipped;
                node.subtree = save_virtual(VirtualDir{});
                return true;
            }
            std::optional<Tree> ptree;
            const Tree* pt = nullptr;
            if (const PreparedDir* pre = upcoming(path)) {
                // Listed ahead, together with its parent listing; never one the journal may reuse.
                pt = pre->parent;
            } else {
                if (pnode != nullptr && pnode->kind == NodeKind::Dir) ptree = load_tree(pnode->subtree);
                pt = ptree ? &*ptree : nullptr;
                if (pt != nullptr && journal_unchanged(path)) {
                    if (const std::optional<Id> id = reuse_dir(path, info.device, *pt)) {
                        node.subtree = *id;
                        ++stats_.dirs;
                        return true;
                    }
                }
            }
            node.subtree = scan_dir(path, info.device, pt);
            ++stats_.dirs;
            return true;
        }
        case os::EntryType::File:
            node.kind = NodeKind::File;
            if (!scan_file(path, node, pnode, job)) return false;
            ++stats_.files;
            return true;
    }
    return false;
}

Tree BackupEngine::scan_children(const std::string& dir, std::uint64_t device, const Tree* parent) {
    Tree tree;
    // Listed ahead, it also owns `parent` (the parent snapshot's listing), used below.
    std::shared_ptr<PreparedDir> listed;
    if (pool_) {
        listed = take_prepared(dir, device, parent);
        if (listed->unlisted) {
            warn("cannot list " + dir + ": " + os::error_message(listed->list_error));
            return tree;
        }
        scan_listing(*listed, tree);
        if (listed->stopped_early) warn("listing " + dir + " stopped early: " + os::error_message(listed->stop_error));
    } else {
        os::DirReader reader(io(dir));
        if (!reader.ok()) {
            warn("cannot list " + dir + ": " + os::error_message(reader.error()));
            return tree;
        }
        std::string name;
        os::FileInfo info;
        const bool refresh_all = refresh_all_in(dir);
        while (reader.next(name, info)) {
            check_cancel();
            const std::string child = os::join(dir, name);
            if (excluder_.excluded(name, child)) continue;
            const Node* pnode = parent != nullptr ? parent->find(name) : nullptr;
            refresh_info(child, info, pnode, refresh_all);
            Node node = node_from_info(info, name);
            if (scan_entry(child, info, device, node, pnode)) {
                note_change(node, pnode);
                tree.nodes.push_back(std::move(node));
            }
        }
        if (!reader.ok()) warn("listing " + dir + " stopped early: " + os::error_message(reader.error()));
    }
    tree.sort();
    if (parent != nullptr) {
        for (const Node& old : parent->nodes)
            if (tree.find(old.name) == nullptr) ++stats_.items_removed;
        // A file gone and a new one with its name plus an extension: "a.docx" -> "a.docx.locked".
        for (const Node& n : tree.nodes) {
            if (n.kind != NodeKind::File || parent->find(n.name) != nullptr) continue;
            const std::size_t dot = n.name.rfind('.');
            if (dot == std::string::npos || dot == 0) continue;
            const std::string before = n.name.substr(0, dot);
            const Node* old = parent->find(before);
            if (old != nullptr && old->kind == NodeKind::File && tree.find(before) == nullptr &&
                before.find('.') != std::string::npos) {
                ++alarm_.renamed;
                suspicious(os::join(dir, n.name));
            }
        }
    }
    return tree;
}

std::shared_ptr<PreparedDir> BackupEngine::prepare(const std::string& dir, std::uint64_t device,
                                                   const Tree* parent, std::optional<Tree> own_parent) {
    auto listed = std::make_shared<PreparedDir>();
    listed->dir = dir;
    listed->device = device;
    listed->own_parent = std::move(own_parent);
    listed->parent = listed->own_parent ? &*listed->own_parent : parent;
    os::DirReader reader(io(dir));
    if (!reader.ok()) {
        listed->unlisted = true;
        listed->list_error = reader.error();
        return listed;
    }
    std::string name;
    os::FileInfo info;
    const bool refresh_all = refresh_all_in(dir);
    while (reader.next(name, info)) {
        check_cancel();
        std::string child = os::join(dir, name);
        if (excluder_.excluded(name, child)) continue;
        PreparedDir::Entry e;
        e.path = std::move(child);
        e.pnode = listed->parent != nullptr ? listed->parent->find(name) : nullptr;
        refresh_info(e.path, info, e.pnode, refresh_all);
        e.info = info;
        e.node = node_from_info(info, name);
        listed->entries.push_back(std::move(e));
    }
    if (!reader.ok()) {
        listed->stopped_early = true;
        listed->stop_error = reader.error();
    }
    // Files that cannot be reused go to the readers now, in listing order.
    std::vector<std::shared_ptr<ReaderPool::FileJob>> jobs;
    for (PreparedDir::Entry& e : listed->entries) {
        if (e.info.type != os::EntryType::File || reusable(e.node, e.pnode)) continue;
        e.job = std::make_shared<ReaderPool::FileJob>();
        e.job->io_path = io(e.path);
        jobs.push_back(e.job);
    }
    pool_->submit(std::move(jobs));
    return listed;
}

std::shared_ptr<PreparedDir> BackupEngine::take_prepared(const std::string& dir, std::uint64_t device,
                                                         const Tree* parent) {
    if (!ahead_.empty() && ahead_.front()->dir == dir) {
        std::shared_ptr<PreparedDir> listed = std::move(ahead_.front());
        ahead_.pop_front();
        return listed;
    }
    // Nothing listed ahead for this folder (a backup path, a folder inside one the journal let
    // the engine reuse, or where the lookahead stopped): it starts again from here.
    drop_lookahead();
    std::shared_ptr<PreparedDir> listed = prepare(dir, device, parent, std::nullopt);
    walk_.push_back(WalkFrame{listed, 0});
    return listed;
}

void BackupEngine::look_ahead() {
    // Enough to keep every reader busy across folder ends, without listing far ahead.
    constexpr std::size_t kMaxFoldersAhead = 64;
    const std::size_t max_files = 4 * std::size_t{options_.read_threads};
    while (!walk_.empty() && ahead_.size() < kMaxFoldersAhead && pool_->pending() < max_files) {
        WalkFrame& frame = walk_.back();
        if (frame.next >= frame.dir->entries.size()) {
            walk_.pop_back();
            continue;
        }
        const PreparedDir::Entry& e = frame.dir->entries[frame.next];
        if (e.info.type != os::EntryType::Dir) {
            ++frame.next;
            continue;
        }
        // What the engine does with this folder depends on more than its listing: stop here.
        if (e.info.device != frame.dir->device || journal_unchanged(e.path)) {
            walk_.clear();
            return;
        }
        ++frame.next;
        std::optional<Tree> ptree;
        if (e.pnode != nullptr && e.pnode->kind == NodeKind::Dir) ptree = load_tree(e.pnode->subtree);
        std::shared_ptr<PreparedDir> listed = prepare(e.path, e.info.device, nullptr, std::move(ptree));
        ahead_.push_back(listed);
        walk_.push_back(WalkFrame{std::move(listed), 0});  // `frame` is not used after this
    }
}

void BackupEngine::drop_lookahead() {
    ahead_.clear();
    walk_.clear();
    pool_->abandon_all();
}

void BackupEngine::scan_listing(PreparedDir& dir, Tree& tree) {
    // Left by an exception (Cancelled, a write error): the readers stop on what was listed ahead.
    struct DropOnError {
        BackupEngine& engine;
        int exceptions = std::uncaught_exceptions();
        ~DropOnError() {
            if (std::uncaught_exceptions() > exceptions) engine.drop_lookahead();
        }
    } drop_on_error{*this};

    const auto scan = [&](PreparedDir::Entry& e) {
        look_ahead();
        check_cancel();
        if (scan_entry(e.path, e.info, dir.device, e.node, e.pnode, e.job.get())) {
            note_change(e.node, e.pnode);
            tree.nodes.push_back(std::move(e.node));
        }
        e.job.reset();
    };
    // Its files first, then its subfolders: no reader then waits on a file the engine reaches
    // only after deeper folders.
    for (PreparedDir::Entry& e : dir.entries)
        if (e.info.type != os::EntryType::Dir) scan(e);
    for (PreparedDir::Entry& e : dir.entries)
        if (e.info.type == os::EntryType::Dir) scan(e);
}

Id BackupEngine::scan_dir(const std::string& dir, std::uint64_t device, const Tree* parent) {
    const Tree tree = scan_children(dir, device, parent);
    Id id;
    repo_.save_blob(BlobType::Tree, serialize_tree(tree), id);
    return id;
}

bool BackupEngine::journal_unchanged(const std::string& dir) {
    if (!use_journal_) return false;
    for (const std::string& m : moved_dirs_)
        if (dir.starts_with(m) && (dir.size() == m.size() || dir[m.size()] == '\\' || dir[m.size()] == '/'))
            return false;
    if (options_.dir_unchanged(dir)) return true;
    // Asked only of the few folders the journal does not call unchanged. Folders are met before
    // what is in them, so the folder is known here before any of its subfolders is asked about.
    if (options_.dir_moved && options_.dir_moved(dir)) moved_dirs_.push_back(dir);
    return false;
}

std::optional<Id> BackupEngine::reuse_dir(const std::string& dir, std::uint64_t device, const Tree& parent) {
    // Everything the listing points at must still be stored (a prune may have removed data no
    // version uses); otherwise the folder is read as usual.
    for (const Node& n : parent.nodes) {
        if (n.security && !repo_.has_blob(*n.security)) return std::nullopt;
        if (n.kind == NodeKind::File && !all_present(n.content)) return std::nullopt;
        for (const DataStream& s : n.streams)
            if (!all_present(s.content)) return std::nullopt;
    }
    ++dirs_reused_;
    Tree tree;
    for (const Node& old : parent.nodes) {
        check_cancel();
        const std::string child = os::join(dir, old.name);
        if (excluder_.excluded(old.name, child)) continue;
        Node node = old;
        switch (old.kind) {
            case NodeKind::File:
                ++stats_.files;
                stats_.bytes += old.size;
                break;
            case NodeKind::Symlink:
                ++stats_.files;
                break;
            case NodeKind::Reparse:
                if (os::is_cloud_only(os::FileInfo{os::EntryType::Other, old.attributes})) ++stats_.cloud_only;
                else ++stats_.skipped;
                break;
            case NodeKind::Dir: {
                // The folder's own entry is as before; what is in it is asked about in turn.
                std::optional<Tree> sub = load_tree(old.subtree);
                std::optional<Id> id;
                if (sub && journal_unchanged(child)) id = reuse_dir(child, device, *sub);
                if (id) {
                    node.subtree = *id;
                    ++stats_.dirs;
                    break;
                }
                // Read it: listed afresh, with its own metadata.
                os::FileInfo info;
                os::ErrorCode err = 0;
                if (!os::stat(io(child), info, err)) {
                    warn("cannot back up " + child + ": " + os::error_message(err));
                    continue;
                }
                Node fresh = node_from_info(info, old.name);
                if (!scan_entry(child, info, device, fresh, &old)) continue;
                note_change(fresh, &old);
                node = std::move(fresh);
                break;
            }
        }
        tree.nodes.push_back(std::move(node));
        report();
    }
    tree.sort();
    Id id;
    repo_.save_blob(BlobType::Tree, serialize_tree(tree), id);
    return id;
}

Id BackupEngine::save_virtual(const VirtualDir& dir) {
    Tree tree;
    for (const auto& [name, child] : dir.dirs) {
        Node n;
        n.name = name;
        n.kind = NodeKind::Dir;
        n.subtree = save_virtual(child);
        tree.nodes.push_back(std::move(n));
    }
    for (const Node& leaf : dir.leaves) tree.nodes.push_back(leaf);
    tree.sort();
    Id id;
    repo_.save_blob(BlobType::Tree, serialize_tree(tree), id);
    return id;
}

bool BackupEngine::scan_stream(const ExtraSource& x, Node& node, std::uint64_t time) {
    std::string shown;
    for (const std::string& c : x.place) shown += (shown.empty() ? "" : "/") + c;
    progress_.current = shown;
    report(true);
    node.kind = NodeKind::File;
    node.attributes = 0x20;  // FILE_ATTRIBUTE_ARCHIVE, as a new file has
    node.mode = 0100644;
    node.ctime = node.mtime = time;
    node.size = 0;
    try {
        if (!x.open) throw Error("nothing to read");
        std::unique_ptr<StreamSource> source = x.open();
        ChunkReader reader(
            repo_.chunker(), [&source](std::uint8_t* dst, std::size_t max) { return source->read(dst, max); },
            buffer_);
        ByteSpan chunk;
        while (reader.next(chunk)) {
            check_cancel();
            Id id;
            if (repo_.save_blob(BlobType::Data, chunk, id)) stats_.new_bytes += chunk.size();
            node.content.push_back(id);
            node.size += chunk.size();
            stats_.bytes += chunk.size();
            report();
        }
        source->finish();
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& e) {
        warn("skipped " + shown + ": " + e.what());
        return false;
    }
    ++stats_.files;
    return true;
}

bool BackupEngine::scan_extra(const ExtraSource& x, Node& node, const Node* pnode, std::uint64_t time) {
    node.name = x.place.back();
    if (x.folder.empty()) {
        if (!scan_stream(x, node, time)) return false;
        if (pnode == nullptr || pnode->kind != NodeKind::File) ++stats_.files_new;
        else if (pnode->content != node.content) ++stats_.files_changed;
        return true;
    }
    os::FileInfo info;
    os::ErrorCode err = 0;
    if (!os::stat(io(x.folder), info, err)) {
        warn("cannot back up " + x.folder + ": " + os::error_message(err));
        return false;
    }
    const std::string name = node.name;
    node = node_from_info(info, name);
    // The change journal only knows the job's own folders. The exclusions are for those folders
    // too: the folder is stored whole (it is an export, kept out of the folders by excluding the
    // staging area, which would otherwise also empty it here).
    const bool journal = use_journal_;
    use_journal_ = false;
    Excluder excluder({});
    std::swap(excluder, excluder_);
    bool ok = false;
    try {
        ok = scan_entry(x.folder, info, info.device, node, pnode);
    } catch (...) {
        std::swap(excluder, excluder_);
        use_journal_ = journal;
        throw;
    }
    std::swap(excluder, excluder_);
    use_journal_ = journal;
    return ok;
}

BackupResult BackupEngine::run() {
    os::enable_backup_privileges();  // lets an elevated run read files regardless of their ACLs
    const std::vector<Source> sources = resolve_sources();

    Snapshot snap;
    snap.time = os::now();
    snap.hostname = os::hostname();
    snap.username = os::username();
    for (const Source& s : sources) snap.paths.push_back(s.full);
    snap.tags = options_.tags;

    std::optional<Tree> parent_root;
    std::optional<Id> parent_tree;  // the parent's root tree, to see whether anything changed
    std::optional<Id> parent_apps;  // the parent's list of installed programs
    bool parent_is_own = false;
    snap.parent = find_parent(snap.paths);
    if (snap.parent) {
        try {
            const Snapshot ps = repo_.load_snapshot(*snap.parent);
            parent_tree = ps.tree;
            parent_apps = ps.apps;
            // Only a version of this same job may stand for an unchanged run (one that carries
            // every tag this run would get), so its own retention keeps it.
            parent_is_own = std::all_of(options_.tags.begin(), options_.tags.end(), [&](const std::string& t) {
                return std::find(ps.tags.begin(), ps.tags.end(), t) != ps.tags.end();
            });
            alarm_.parent_files = ps.stats.files;
            parent_root = load_tree(ps.tree);
        } catch (const Error& e) {
            if (options_.on_warning) options_.on_warning(std::string("parent snapshot unreadable: ") + e.what());
            snap.parent.reset();
        }
    }

    if (options_.read_threads > 1) pool_ = std::make_unique<ReaderPool>(repo_, options_.read_threads);

    // The journal answers only for changes since the snapshot it was read against.
    use_journal_ = options_.dir_unchanged && options_.journal_parent && snap.parent &&
                   *snap.parent == *options_.journal_parent && parent_root.has_value();

    const WriteStats before = repo_.write_stats();
    VirtualDir root;
    for (const Source& s : sources) {
        os::FileInfo info;
        os::ErrorCode err = 0;
        if (!os::stat(io(s.full), info, err)) {
            warn("cannot back up " + s.full + ": " + os::error_message(err));
            missing_sources_.push_back(s.full);
            continue;
        }
        // A source named explicitly is followed even if it is a link to a folder.
        if (info.type != os::EntryType::Dir && info.type != os::EntryType::File && os::is_directory(io(s.full)))
            info.type = os::EntryType::Dir;

        if (s.comps.empty()) {
            // The file system root ("/"): its entries go directly under the snapshot root.
            if (info.type != os::EntryType::Dir) continue;
            Tree tree = scan_children(s.full, info.device, parent_root ? &*parent_root : nullptr);
            for (Node& n : tree.nodes) root.leaves.push_back(std::move(n));
            ++stats_.dirs;
            continue;
        }

        Node node = node_from_info(info, s.comps.back());
        std::optional<Node> pnode;
        if (parent_root) pnode = lookup(*parent_root, s.comps);
        if (!scan_entry(s.full, info, info.device, node, pnode ? &*pnode : nullptr)) continue;
        note_change(node, pnode ? &*pnode : nullptr);

        VirtualDir* cur = &root;
        for (std::size_t i = 0; i + 1 < s.comps.size(); ++i) cur = &cur->dirs[s.comps[i]];
        cur->leaves.push_back(std::move(node));
    }
    for (const ExtraSource& x : options_.extras) {
        check_cancel();
        if (x.place.empty()) continue;
        // (The folders above it are made only once it is stored: no empty "WSL" for a skipped one.)
        const VirtualDir* look = &root;
        for (std::size_t i = 0; look != nullptr && i + 1 < x.place.size(); ++i) {
            const auto it = look->dirs.find(x.place[i]);
            look = it == look->dirs.end() ? nullptr : &it->second;
        }
        const bool taken = look != nullptr && (look->dirs.contains(x.place.back()) ||
                                               std::any_of(look->leaves.begin(), look->leaves.end(),
                                                           [&](const Node& n) { return n.name == x.place.back(); }));
        if (taken) {
            warn("skipped " + x.place.back() + ": two items to back up have the same name");
            continue;
        }
        std::optional<Node> pnode;
        if (parent_root) pnode = lookup(*parent_root, x.place);
        Node node;
        if (!scan_extra(x, node, pnode ? &*pnode : nullptr, snap.time)) {
            // Like a source that could not be read: old versions, which still have it, stay.
            std::string shown;
            for (const std::string& c : x.place) shown += (shown.empty() ? "" : "/") + c;
            missing_sources_.push_back(shown);
            continue;
        }
        if (node.kind == NodeKind::File && x.folder.empty()) {
            // A stream's times are those of the run: only its content tells whether it changed.
            if (!pnode || pnode->kind != NodeKind::File || pnode->content != node.content) changed_ = true;
        } else {
            note_change(node, pnode ? &*pnode : nullptr);
        }
        VirtualDir* cur = &root;
        for (std::size_t i = 0; i + 1 < x.place.size(); ++i) cur = &cur->dirs[x.place[i]];
        cur->leaves.push_back(std::move(node));
    }
    pool_.reset();  // every file is read: the readers and their buffers go

    snap.tree = save_virtual(root);
    if (!options_.app_list && options_.reuse_app_list && parent_apps) {
        snap.apps = parent_apps;
    } else if (options_.app_list) {
        try {
            if (const std::optional<std::string> list = options_.app_list()) {
                Id id{};
                repo_.save_blob(BlobType::Data,
                                ByteSpan(reinterpret_cast<const std::uint8_t*>(list->data()), list->size()), id);
                snap.apps = id;
            }
        } catch (const Cancelled&) {
            throw;
        } catch (const std::exception& e) {
            if (options_.on_warning) options_.on_warning(std::string("cannot list the installed programs: ") + e.what());
        }
    }
    repo_.flush();
    if (options_.before_save) options_.before_save();
    stats_.stored_bytes = repo_.write_stats().stored_bytes_added - before.stored_bytes_added;
    snap.stats = stats_;
    snap.stats.changes_known = true;
    report(true);

    BackupResult result;
    if (options_.skip_unchanged && parent_tree && parent_is_own && !options_.parent && !changed_ &&
        stats_.items_removed == 0 && missing_sources_.empty()) {
        // Exactly what the last version holds: nothing to save.
        result.unchanged = true;
        result.dirs_reused = dirs_reused_;
        result.snapshot = std::move(snap);
        result.alarm = std::move(alarm_);
        return result;
    }
    result.snapshot_id = repo_.save_snapshot(snap);
    result.snapshot = std::move(snap);
    result.alarm = std::move(alarm_);
    result.missing_sources = std::move(missing_sources_);
    result.dirs_reused = dirs_reused_;
    return result;
}

}  // namespace

SourceEstimate estimate_sources(const std::vector<std::string>& paths, const std::vector<std::string>& excludes,
                                const std::function<bool()>& cancelled,
                                const std::function<void(const SourceEstimate&)>& progress) {
    const Excluder excluder(excludes);
    SourceEstimate e;
    auto last = std::chrono::steady_clock::now();
    const auto tick = [&] {
        if (cancelled && cancelled()) throw Cancelled();
        const auto now = std::chrono::steady_clock::now();
        if (progress && now - last > std::chrono::milliseconds(200)) {
            last = now;
            progress(e);
        }
    };
    std::vector<std::string> pending;
    for (const std::string& p : paths) {
        const std::string full = os::absolute_path(p);
        os::FileInfo info;
        os::ErrorCode err = 0;
        if (!os::stat(full, info, err)) {
            ++e.unreadable;
            continue;
        }
        if (info.type == os::EntryType::File) {
            ++e.files;
            e.bytes += info.size;
        } else if (info.type == os::EntryType::Dir || os::is_directory(full)) {
            ++e.dirs;
            pending.push_back(full);
        }
    }
    while (!pending.empty()) {
        const std::string dir = std::move(pending.back());
        pending.pop_back();
        os::DirReader reader(dir);
        std::string name;
        os::FileInfo info;
        while (reader.next(name, info)) {
            tick();
            const std::string child = os::join(dir, name);
            if (excluder.excluded(name, child)) continue;
            if (info.type == os::EntryType::Dir) {
                ++e.dirs;
                pending.push_back(child);
            } else if (info.type == os::EntryType::File) {
                ++e.files;
                e.bytes += info.size;
            }
        }
        if (!reader.ok()) ++e.unreadable;
    }
    return e;
}

double byte_entropy(ByteSpan data) {
    if (data.empty()) return 0;
    std::array<std::uint32_t, 256> counts{};
    for (const std::uint8_t b : data) ++counts[b];
    double h = 0;
    const double n = static_cast<double>(data.size());
    for (const std::uint32_t c : counts) {
        if (c == 0) continue;
        const double p = c / n;
        h -= p * std::log2(p);
    }
    return h;
}

BackupResult run_backup(Repository& repo, const BackupOptions& options) {
    BackupEngine engine(repo, options);
    return engine.run();
}

bool path_excluded(const std::vector<std::string>& excludes, const std::string& path) {
    if (excludes.empty()) return false;
    const Excluder excluder(excludes);
    std::size_t at = 0;
    while (at < path.size()) {
        std::size_t next = path.find_first_of("\\/", at);
        if (next == std::string::npos) next = path.size();
        if (next > at && excluder.excluded(std::string_view(path).substr(at, next - at), path.substr(0, next)))
            return true;
        at = next + 1;
    }
    return false;
}

}  // namespace vaultik
