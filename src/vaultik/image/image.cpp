#include "vaultik/image/image.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <map>

#include "vaultik/model/tree.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik::image {

namespace {

constexpr std::size_t kReadBlock = 4u << 20;

std::string segment_name(std::uint64_t s) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "s%06llu", static_cast<unsigned long long>(s));
    return buf;
}

std::string group_name(std::uint64_t g) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "g%04llu", static_cast<unsigned long long>(g));
    return buf;
}

Node dir_node(std::string name, const Id& subtree) {
    Node n;
    n.name = std::move(name);
    n.kind = NodeKind::Dir;
    n.subtree = subtree;
    return n;
}

Node file_node(std::string name, std::uint64_t size, std::vector<Id> content) {
    Node n;
    n.name = std::move(name);
    n.kind = NodeKind::File;
    n.size = size;
    n.content = std::move(content);
    return n;
}

Id save_tree(Repository& repo, Tree& tree) {
    tree.sort();
    Id id{};
    repo.save_blob(BlobType::Tree, serialize_tree(tree), id);
    return id;
}

// Hands out the bytes of consecutive Data and Zero extents in order, reading the next block on
// another thread while the current one is chunked.
class StreamReader {
public:
    StreamReader(DiskSource& source, std::size_t partition, std::vector<Extent> extents)
        : source_(source), partition_(partition), extents_(std::move(extents)) {}
    ~StreamReader() {
        if (next_.valid()) next_.wait();
    }

    std::size_t read(std::uint8_t* dst, std::size_t max) {
        std::size_t done = 0;
        while (done < max) {
            if (pos_ == cur_.size()) {
                if (!next_.valid()) {
                    if (finished()) break;
                    next_ = std::async(std::launch::async, [this] { return produce(); });
                }
                cur_ = next_.get();
                pos_ = 0;
                if (cur_.empty()) break;
                if (!finished()) next_ = std::async(std::launch::async, [this] { return produce(); });
            }
            const std::size_t n = std::min(max - done, cur_.size() - pos_);
            std::memcpy(dst + done, cur_.data() + pos_, n);
            pos_ += n;
            done += n;
        }
        return done;
    }

    std::uint64_t bytes_read() const { return read_.load(); }

private:
    bool finished() const { return index_ >= extents_.size(); }

    // The next block of at most kReadBlock bytes (empty at the end). Runs on one thread at a
    // time: read() waits for it before starting the next.
    Bytes produce() {
        Bytes out;
        while (index_ < extents_.size() && out.size() < kReadBlock) {
            const Extent& e = extents_[index_];
            const std::size_t n =
                static_cast<std::size_t>(std::min<std::uint64_t>(kReadBlock - out.size(), e.length - within_));
            const std::size_t at = out.size();
            out.resize(at + n, 0);
            if (e.kind == Extent::Kind::Data) {
                source_.read(partition_, e.offset + within_, out.data() + at, n);
                read_ += n;
            }
            within_ += n;
            if (within_ == e.length) {
                ++index_;
                within_ = 0;
            }
        }
        return out;
    }

    DiskSource& source_;
    std::size_t partition_;
    std::vector<Extent> extents_;
    std::size_t index_ = 0;
    std::uint64_t within_ = 0;
    std::atomic<std::uint64_t> read_{0};  // also read while the next block is being read
    Bytes cur_;
    std::size_t pos_ = 0;
    std::future<Bytes> next_;
};

class Backup {
public:
    Backup(Repository& repo, const ImageBackupOptions& o) : repo_(repo), o_(o) {}

    ImageBackupResult run() {
        const auto start_stats = repo_.write_stats();
        start_raw_ = start_stats.raw_bytes_added;
        // Totals first, for the progress bar.
        for (DiskSource* d : o_.disks) {
            DiskLayout& l = d->layout();
            if (l.regions.empty()) l.regions = gap_regions(l);
            for (const Partition& p : l.partitions) progress_.total += p.length;
            for (const Region& r : l.regions) progress_.total += r.length;
        }

        Tree root;
        Snapshot snap;
        for (std::size_t k = 0; k < o_.disks.size(); ++k) {
            DiskSource& d = *o_.disks[k];
            root.nodes.push_back(dir_node("disk" + std::to_string(k), store_disk(d)));
            snap.paths.push_back(image_path(d.layout()));
            snap.stats.files += d.layout().partitions.size();
        }
        snap.tree = save_tree(repo_, root);
        repo_.flush();
        if (o_.before_save) o_.before_save();

        snap.time = os::now();
        snap.hostname = os::hostname();
        snap.username = os::username();
        snap.parent = o_.parent;
        snap.tags = o_.tags;
        snap.image = true;
        snap.stats.dirs = o_.disks.size();
        snap.stats.bytes = progress_.total;
        snap.stats.new_bytes = repo_.write_stats().raw_bytes_added - start_raw_;
        snap.stats.stored_bytes = repo_.write_stats().stored_bytes_added - start_stats.stored_bytes_added;
        const Id id = repo_.save_snapshot(snap);
        report(true);
        return {id, snap};
    }

private:
    void check_cancel() {
        if (o_.should_cancel && o_.should_cancel()) throw Cancelled();
    }

    void report(bool force) {
        if (!o_.on_progress) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last_report_ < std::chrono::milliseconds(200)) return;
        last_report_ = now;
        progress_.new_bytes = repo_.write_stats().raw_bytes_added - start_raw_;
        o_.on_progress(progress_);
    }

    Id zero_chunk(std::uint64_t size) {
        auto it = zero_ids_.find(size);
        if (it != zero_ids_.end()) return it->second;
        const Bytes zeros(static_cast<std::size_t>(size), 0);
        Id id{};
        repo_.save_blob(BlobType::Data, zeros, id);
        zero_ids_.emplace(size, id);
        return id;
    }

    // Small content (layout, regions) as one file node's chunks.
    std::vector<Id> store_bytes(ByteSpan data) {
        std::vector<Id> ids;
        std::size_t pos = 0;
        const std::size_t max = repo_.chunker().params().max_size;
        while (pos < data.size()) {
            const std::size_t n = std::min(max, data.size() - pos);
            Id id{};
            repo_.save_blob(BlobType::Data, data.subspan(pos, n), id);
            ids.push_back(id);
            pos += n;
        }
        return ids;
    }

    Id store_disk(DiskSource& d) {
        DiskLayout& l = d.layout();
        Tree tree;
        // Regions outside the partitions.
        for (std::size_t i = 0; i < l.regions.size(); ++i) {
            const Region& r = l.regions[i];
            Bytes buf(static_cast<std::size_t>(r.length));
            d.read_disk(r.offset, buf.data(), buf.size());
            tree.nodes.push_back(file_node("r" + std::to_string(i), r.length, store_bytes(buf)));
            progress_.done += r.length;
            progress_.read += r.length;
        }
        // Partitions.
        for (std::size_t i = 0; i < l.partitions.size(); ++i) {
            Partition& p = l.partitions[i];
            if (l.style == PartitionStyle::Mbr && is_container(p.mbr_type)) {
                progress_.done += p.length;
                continue;
            }
            progress_.current = describe(p);
            report(true);
            const std::vector<Extent> plan = d.plan(i);
            p.used_bytes = data_bytes(plan);
            p.holes = p.method == ReadMethod::Used ? holes_of(plan) : Ranges{};
            tree.nodes.push_back(dir_node("p" + std::to_string(i), store_partition(d, i, plan)));
        }
        const std::string json = to_json(l);
        tree.nodes.push_back(file_node("layout.json", json.size(), store_bytes(as_bytes(json))));
        return save_tree(repo_, tree);
    }

    Id store_partition(DiskSource& d, std::size_t index, const std::vector<Extent>& plan) {
        const Partition& p = d.layout().partitions[index];
        const std::uint64_t segments = (p.length + kSegmentSize - 1) / kSegmentSize;
        Tree part;
        Tree group;
        for (std::uint64_t s = 0; s < segments; ++s) {
            const std::uint64_t from = s * kSegmentSize;
            const std::uint64_t to = std::min(p.length, from + kSegmentSize);
            group.nodes.push_back(file_node(segment_name(s), to - from, store_segment(d, index, clip(plan, from, to))));
            if (group.nodes.size() == kSegmentsPerGroup || s + 1 == segments) {
                part.nodes.push_back(dir_node(group_name(s / kSegmentsPerGroup), save_tree(repo_, group)));
                group = Tree{};
            }
        }
        return save_tree(repo_, part);
    }

    std::vector<Id> store_segment(DiskSource& d, std::size_t index, const std::vector<Extent>& extents) {
        std::vector<Id> ids;
        const std::uint64_t max = repo_.chunker().params().max_size;
        std::size_t i = 0;
        while (i < extents.size()) {
            check_cancel();
            if (extents[i].kind == Extent::Kind::Hole) {
                for (const std::uint64_t n : zero_chunk_sizes(extents[i].length, max)) {
                    ids.push_back(zero_chunk(n));
                    progress_.done += n;
                }
                report(false);
                ++i;
                continue;
            }
            // Data with the short unused runs in it, chunked as one stream.
            std::size_t j = i;
            while (j < extents.size() && extents[j].kind != Extent::Kind::Hole) ++j;
            StreamReader reader(d, index, std::vector<Extent>(extents.begin() + static_cast<std::ptrdiff_t>(i),
                                                              extents.begin() + static_cast<std::ptrdiff_t>(j)));
            ChunkReader chunks(
                repo_.chunker(), [&reader](std::uint8_t* dst, std::size_t n) { return reader.read(dst, n); }, buffer_);
            ByteSpan chunk;
            std::uint64_t before = 0;
            while (chunks.next(chunk)) {
                Id id{};
                repo_.save_blob(BlobType::Data, chunk, id);
                ids.push_back(id);
                progress_.done += chunk.size();
                progress_.read += reader.bytes_read() - before;
                before = reader.bytes_read();
                report(false);
                check_cancel();
            }
            i = j;
        }
        return ids;
    }

    Repository& repo_;
    const ImageBackupOptions& o_;
    ImageProgress progress_;
    std::uint64_t start_raw_ = 0;
    std::chrono::steady_clock::time_point last_report_{};
    std::map<std::uint64_t, Id> zero_ids_;
    Bytes buffer_;
};

// Collects writes into large aligned pieces for a disk that needs sector-aligned writes.
class AlignedWriter {
public:
    explicit AlignedWriter(DiskTarget& target) : target_(target), align_(std::max(1u, target.alignment())) {}

    void write(std::uint64_t offset, ByteSpan data) {
        if (!buf_.empty() && offset != start_ + buf_.size()) flush();
        if (buf_.empty()) start_ = offset;
        buf_.insert(buf_.end(), data.begin(), data.end());
        if (buf_.size() >= kReadBlock) {
            // Everything but an unaligned tail goes out now.
            const std::size_t n = buf_.size() / align_ * align_;
            if ((start_ % align_) != 0) throw Error("internal error: an unaligned write to the disk");
            target_.write(start_, ByteSpan(buf_.data(), n));
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(n));
            start_ += n;
        }
    }

    void flush() {
        if (buf_.empty()) return;
        if (start_ % align_ != 0 || buf_.size() % align_ != 0)
            throw Error("internal error: an unaligned write to the disk");
        target_.write(start_, buf_);
        buf_.clear();
    }

private:
    DiskTarget& target_;
    std::uint32_t align_;
    std::uint64_t start_ = 0;
    Bytes buf_;
};

const Node& child(const Tree& t, const std::string& name, const std::string& what) {
    const Node* n = t.find(name);
    if (n == nullptr) throw Error(what + " is missing from the backup (" + name + ")");
    return *n;
}

}  // namespace

std::string image_path(const DiskLayout& layout) {
    return "image:" + (layout.id.empty() ? disk_id(layout) : layout.id);
}

ImageBackupResult run_image_backup(Repository& repo, const ImageBackupOptions& options) {
    if (options.disks.empty()) throw Error("no disks to back up");
    Backup b(repo, options);
    return b.run();
}

std::vector<DiskLayout> image_disks(Repository& repo, const Snapshot& snapshot) {
    if (!snapshot.image) throw Error("this version is not a disk image");
    const Tree root = parse_tree(repo.load_blob(BlobType::Tree, snapshot.tree));
    std::vector<DiskLayout> out;
    for (std::size_t k = 0;; ++k) {
        const Node* d = root.find("disk" + std::to_string(k));
        if (d == nullptr) break;
        const Tree disk = parse_tree(repo.load_blob(BlobType::Tree, d->subtree));
        const Node& l = child(disk, "layout.json", "the disk layout");
        Bytes json;
        for (const Id& id : l.content) {
            const Bytes part = repo.load_blob(BlobType::Data, id);
            json.insert(json.end(), part.begin(), part.end());
        }
        out.push_back(layout_from_json(std::string_view(reinterpret_cast<const char*>(json.data()), json.size())));
    }
    if (out.empty()) throw Error("the disk image has no disks");
    return out;
}

void run_image_restore(Repository& repo, const Snapshot& snapshot, DiskTarget& target,
                       const ImageRestoreOptions& o) {
    std::vector<DiskLayout> disks = image_disks(repo, snapshot);
    if (o.disk >= disks.size()) throw Error("the image has no disk " + std::to_string(o.disk + 1));
    DiskLayout layout = disks[o.disk];
    if (o.new_ids) renew_ids(layout);
    const std::string why = fits(layout, target.size(), target.sector_size());
    if (!why.empty()) throw Error(why);
    // The partition tables are made now, so a layout they cannot be made for is refused before
    // the first byte is written; they are written last.
    std::vector<DiskWrite> tables = partition_tables(layout, target.size());

    const Tree root = parse_tree(repo.load_blob(BlobType::Tree, snapshot.tree));
    const Tree disk = parse_tree(
        repo.load_blob(BlobType::Tree, child(root, "disk" + std::to_string(o.disk), "the disk").subtree));

    ImageProgress progress;
    for (const Partition& p : layout.partitions) progress.total += p.length;
    for (const Region& r : layout.regions) progress.total += r.length;
    auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto report = [&](bool force) {
        if (!o.on_progress) return;
        const auto now = std::chrono::steady_clock::now();
        if (!force && now - last < std::chrono::milliseconds(200)) return;
        last = now;
        o.on_progress(progress);
    };
    auto cancel = [&] {
        if (o.should_cancel && o.should_cancel()) throw Cancelled();
    };

    AlignedWriter out(target);
    const std::uint32_t align = std::max(1u, target.alignment());
    // The ids of zero chunks, by length (computed when first seen).
    std::map<std::uint32_t, Id> zero_ids;
    auto is_zero_chunk = [&](const Id& id, std::uint32_t raw_len) {
        auto it = zero_ids.find(raw_len);
        if (it == zero_ids.end()) {
            const Bytes zeros(raw_len, 0);
            it = zero_ids.emplace(raw_len, repo.blob_id(BlobType::Data, zeros)).first;
        }
        return it->second == id;
    };

    // On a disk that has data, clear the old partition tables first (and the backup GPT at its
    // end), so nothing of the old layout is left if the restore stops halfway.
    if (!target.zeroed()) {
        const std::uint64_t n = std::min<std::uint64_t>(1u << 20, target.size());
        const Bytes zeros(static_cast<std::size_t>(n), 0);
        target.write(0, zeros);
        target.write((target.size() - n) / align * align, zeros);
    }

    // Stored regions (MBR, boot loaders, EBRs), without the partition tables in them: until the
    // end the disk shows no partitions, so a restore that stops halfway leaves none half-written.
    for (std::size_t i = 0; i < layout.regions.size(); ++i) {
        const Region& r = layout.regions[i];
        const Node& n = child(disk, "r" + std::to_string(i), "a stored disk region");
        Bytes data;
        for (const Id& id : n.content) {
            const Bytes part = repo.load_blob(BlobType::Data, id);
            data.insert(data.end(), part.begin(), part.end());
        }
        if (data.size() != r.length) throw Error("a stored disk region has the wrong size");
        if (r.offset + r.length > target.size()) continue;  // past the end of a smaller disk: nothing there matters
        if (r.offset == 0 && layout.style == PartitionStyle::Mbr && data.size() >= 512) {
            // The MBR's disk signature (changed by new_ids).
            for (int b = 0; b < 4; ++b) data[440 + b] = static_cast<std::uint8_t>(layout.signature >> (8 * b));
            // An MBR disk's table is the stored first sector itself: written again at the end.
            if (data.size() >= layout.sector_size)
                tables.push_back({0, Bytes(data.begin(), data.begin() + layout.sector_size)});
        }
        blank_tables(layout, r.offset, data);
        out.write(r.offset, data);
        progress.done += r.length;
    }
    out.flush();

    // Partitions.
    for (std::size_t i = 0; i < layout.partitions.size(); ++i) {
        const Partition& p = layout.partitions[i];
        if (layout.style == PartitionStyle::Mbr && is_container(p.mbr_type)) {
            progress.done += p.length;
            continue;
        }
        progress.current = describe(p);
        report(true);
        const Tree part =
            parse_tree(repo.load_blob(BlobType::Tree, child(disk, "p" + std::to_string(i), "a partition").subtree));
        const std::uint64_t segments = (p.length + kSegmentSize - 1) / kSegmentSize;
        const bool skip_holes = p.method == ReadMethod::Used;
        std::uint64_t pos = 0;  // within the partition
        Tree group;
        std::uint64_t group_index = ~0ull;
        for (std::uint64_t s = 0; s < segments; ++s) {
            if (s / kSegmentsPerGroup != group_index) {
                group_index = s / kSegmentsPerGroup;
                group = parse_tree(repo.load_blob(
                    BlobType::Tree, child(part, group_name(group_index), "part of partition " + std::to_string(p.number)).subtree));
            }
            const Node& seg = child(group, segment_name(s), "part of partition " + std::to_string(p.number));
            const std::uint64_t seg_end = std::min(p.length, (s + 1) * kSegmentSize);
            if (seg.size != seg_end - s * kSegmentSize) throw Error("a part of partition " + std::to_string(p.number) + " has the wrong size");
            for (const Id& id : seg.content) {
                cancel();
                const BlobLocation* loc = repo.index().find(id);
                if (loc == nullptr) throw Error("data of partition " + std::to_string(p.number) + " is missing from the backup");
                const std::uint32_t len = loc->raw_len;
                if (pos + len > seg_end) throw Error("a part of partition " + std::to_string(p.number) + " is longer than it should be");
                if (is_zero_chunk(id, len)) {
                    // Zeros: nothing to write on a new virtual disk, nor in a stored hole of an NTFS
                    // partition (unused space), as long as the gap stays aligned for the disk.
                    const std::uint64_t at = p.offset + pos;
                    const bool aligned = at % align == 0 && len % align == 0;
                    if (aligned && (target.zeroed() || (skip_holes && inside(p.holes, pos, len)))) {
                        pos += len;
                        progress.done += len;
                        report(false);
                        continue;
                    }
                    out.write(at, Bytes(len, 0));
                } else {
                    const Bytes data = repo.load_blob(BlobType::Data, id);
                    out.write(p.offset + pos, data);
                }
                pos += len;
                progress.done += len;
                progress.read += len;
                report(false);
            }
            if (pos != seg_end) throw Error("a part of partition " + std::to_string(p.number) + " is shorter than it should be");
        }
        out.flush();
    }

    // The partition tables last: until they are written, the restored disk shows no partitions.
    for (const DiskWrite& w : tables) out.write(w.offset, w.data);
    out.flush();
    target.finish();
    report(true);
}

}  // namespace vaultik::image
