#include "vaultik/ops/mount.hpp"

// Linux: FUSE 3 (libfuse3, the high-level API, run by a loop of our own so it can be stopped at
// any time). The folder is mounted read-only (FUSE's "ro") for this account only (no allow_other);
// the kernel caches listings, attributes and file content for as long as the mount lasts, since a
// version never changes. Built only when CMake finds libfuse3 (VAULTIK_HAVE_FUSE); without it
// available() is false and how_to_enable() says what to install.

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "vaultik/model/tree.hpp"
#include "vaultik/platform/mounts_posix.hpp"
#include "vaultik/platform/os.hpp"

#ifdef VAULTIK_HAVE_FUSE
#define FUSE_USE_VERSION 31
#include <fuse.h>
#include <fuse_lowlevel.h>
#endif

namespace vaultik {

namespace {

// A program on PATH (or in the usual system folders, for a service's short PATH); empty if none.
std::string find_program(const char* name) {
    std::string path = os::get_env("PATH");
    path += ":/usr/bin:/bin:/usr/local/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        const std::string dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.empty() || dir.front() != '/') continue;
        const std::string candidate = dir + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return {};
}

// Whether something is mounted exactly at `path` (absolute).
bool is_mount_point(const std::string& path) {
    for (const os::mounts::Mount& m : os::mounts::read_mounts())
        if (m.point == path) return true;
    return false;
}

}  // namespace

#ifdef VAULTIK_HAVE_FUSE

namespace {

constexpr std::uint32_t kReadOnlyAttribute = 0x1;  // FILE_ATTRIBUTE_READONLY, from Windows
constexpr std::size_t kCachedChunks = 8;

// Files and folders with names that are safe here, and symbolic links. Other items (devices,
// pipes, Windows links and junctions) have no content to show.
bool shown(const Node& n) {
    return (n.kind == NodeKind::File || n.kind == NodeKind::Dir || n.kind == NodeKind::Symlink) && is_safe_name(n.name);
}

struct timespec time_of(std::uint64_t ticks) {
    struct timespec ts{};
    if (ticks == 0) return ts;  // unset
    const std::int64_t since = static_cast<std::int64_t>(ticks) - static_cast<std::int64_t>(os::kUnixEpochTicks);
    std::int64_t sec = since / 10000000ll;
    std::int64_t rest = since % 10000000ll;
    if (rest < 0) {  // floor division, for times before 1970
        rest += 10000000ll;
        --sec;
    }
    ts.tv_sec = static_cast<time_t>(sec);
    ts.tv_nsec = static_cast<long>(rest * 100);
    return ts;
}

// An open file: where each of its chunks starts.
struct OpenFile {
    std::vector<Id> chunks;
    std::vector<std::uint64_t> starts;
    std::uint64_t size = 0;
};

}  // namespace

struct SnapshotMount::Impl {
    Repository* repo = nullptr;
    Snapshot snapshot;
    std::string folder;
    uid_t uid = 0;
    gid_t gid = 0;
    std::mutex mu;  // the repository and the caches
    std::map<Id, std::shared_ptr<const Tree>> trees;
    std::list<std::pair<Id, std::shared_ptr<const Bytes>>> chunks;  // recently read, newest first
    struct fuse* fs = nullptr;
    bool mounted = false;
    bool made_folder = false;  // the folder was made for the mount (and so is removed with it)
    int wake[2] = {-1, -1};  // written to stop the loop
    std::thread loop;
    std::atomic<bool> ended{false};

    std::shared_ptr<const Tree> tree(const Id& id) {
        const auto it = trees.find(id);
        if (it != trees.end()) return it->second;
        auto t = std::make_shared<const Tree>(parse_tree(repo->load_blob(BlobType::Tree, id)));
        if (trees.size() >= 4096) trees.clear();  // a simple bound; folders are cheap to load again
        trees.emplace(id, t);
        return t;
    }

    // A chunk's data. The kernel reads in pieces of 128 KiB or so, much smaller than a chunk, so
    // the last few chunks are kept.
    std::shared_ptr<const Bytes> chunk(const Id& id) {
        for (auto it = chunks.begin(); it != chunks.end(); ++it)
            if (it->first == id) {
                chunks.splice(chunks.begin(), chunks, it);
                return chunks.front().second;
            }
        auto data = std::make_shared<const Bytes>(repo->load_blob(BlobType::Data, id));
        chunks.emplace_front(id, data);
        if (chunks.size() > kCachedChunks) chunks.pop_back();
        return data;
    }

    // The item at a path ("/" is the root, "/a/b" below it).
    bool resolve(const char* path, Node& out) {
        Node cur;
        cur.kind = NodeKind::Dir;
        cur.subtree = snapshot.tree;
        cur.mode = 0755;
        cur.uid = uid;
        cur.gid = gid;
        cur.mtime = snapshot.time;
        cur.ctime = snapshot.time;
        const std::string_view p = path != nullptr ? path : "";
        std::size_t start = 0;
        while (start < p.size()) {
            std::size_t end = p.find('/', start);
            if (end == std::string_view::npos) end = p.size();
            const std::string_view part = p.substr(start, end - start);
            start = end + 1;
            if (part.empty()) continue;
            if (cur.kind != NodeKind::Dir) return false;
            const Node* found = tree(cur.subtree)->find(part);
            if (found == nullptr || !shown(*found)) return false;
            cur = *found;
        }
        out = std::move(cur);
        return true;
    }

    void fill(const Node& n, struct stat* st) const {
        std::memset(st, 0, sizeof *st);
        // Windows attributes are never 0 (a plain file is NORMAL or ARCHIVE): such a node came
        // from Windows and has no owner or mode of its own.
        const bool posix = n.attributes == 0;
        mode_t perms = 0;
        if (posix) perms = n.mode & 07777;
        else if (n.kind == NodeKind::Dir) perms = 0755;
        else perms = (n.attributes & kReadOnlyAttribute) != 0 ? 0444 : 0644;
        switch (n.kind) {
            case NodeKind::Dir:
                st->st_mode = S_IFDIR | perms;
                st->st_nlink = 2;
                st->st_size = 4096;  // what most Linux file systems show for a folder
                break;
            case NodeKind::Symlink:
                st->st_mode = S_IFLNK | 0777;
                st->st_nlink = 1;
                st->st_size = static_cast<off_t>(n.link_target.size());
                break;
            default:
                st->st_mode = S_IFREG | perms;
                st->st_nlink = 1;
                st->st_size = static_cast<off_t>(n.size);
                break;
        }
        st->st_uid = posix ? n.uid : uid;
        st->st_gid = posix ? n.gid : gid;
        st->st_mtim = time_of(n.mtime);
        st->st_atim = st->st_mtim;
        st->st_ctim = posix && n.ctime != 0 ? time_of(n.ctime) : st->st_mtim;
        st->st_blksize = 4096;
        st->st_blocks = static_cast<blkcnt_t>((static_cast<std::uint64_t>(st->st_size) + 511) / 512);
    }
};

namespace {

SnapshotMount::Impl* self() {
    return static_cast<SnapshotMount::Impl*>(fuse_get_context()->private_data);
}

void* on_init(struct fuse_conn_info*, struct fuse_config* cfg) {
    // A version never changes: let the kernel keep what it was told.
    cfg->kernel_cache = 1;
    cfg->entry_timeout = 3600;
    cfg->attr_timeout = 3600;
    cfg->negative_timeout = 3600;
    cfg->use_ino = 0;
    return fuse_get_context()->private_data;  // becomes the private data, so hand it back
}

int on_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
    SnapshotMount::Impl* m = self();
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Node n;
        if (!m->resolve(path, n)) return -ENOENT;
        m->fill(n, st);
        return 0;
    } catch (const std::exception&) {
        return -EIO;  // damaged or unreadable backup data
    }
}

int on_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, struct fuse_file_info*,
               enum fuse_readdir_flags) {
    SnapshotMount::Impl* m = self();
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Node dir;
        if (!m->resolve(path, dir)) return -ENOENT;
        if (dir.kind != NodeKind::Dir) return -ENOTDIR;
        filler(buf, ".", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        filler(buf, "..", nullptr, 0, static_cast<fuse_fill_dir_flags>(0));
        const std::shared_ptr<const Tree> t = m->tree(dir.subtree);
        for (const Node& n : t->nodes) {
            if (!shown(n)) continue;
            struct stat st;
            m->fill(n, &st);
            if (filler(buf, n.name.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0)) != 0) break;
        }
        return 0;
    } catch (const std::exception&) {
        return -EIO;
    }
}

int on_open(const char* path, struct fuse_file_info* fi) {
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EROFS;
    SnapshotMount::Impl* m = self();
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Node n;
        if (!m->resolve(path, n)) return -ENOENT;
        if (n.kind == NodeKind::Dir) return -EISDIR;
        if (n.kind != NodeKind::File) return -EINVAL;
        auto f = std::make_unique<OpenFile>();
        f->chunks = n.content;
        f->starts.reserve(n.content.size());
        for (const Id& id : n.content) {
            const BlobLocation* loc = m->repo->index().find(id);
            if (loc == nullptr) return -EIO;  // the backup lacks this chunk
            f->starts.push_back(f->size);
            f->size += loc->raw_len;
        }
        fi->fh = reinterpret_cast<std::uint64_t>(f.release());
        fi->keep_cache = 1;
        return 0;
    } catch (const std::exception&) {
        return -EIO;
    }
}

int on_read(const char*, char* buf, std::size_t size, off_t offset, struct fuse_file_info* fi) {
    SnapshotMount::Impl* m = self();
    const auto* f = reinterpret_cast<const OpenFile*>(fi->fh);
    if (f == nullptr || offset < 0) return -EINVAL;
    const auto from = static_cast<std::uint64_t>(offset);
    if (from >= f->size || size == 0) return 0;
    const std::uint64_t end = std::min<std::uint64_t>(f->size, from + size);
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        // The chunk holding `from`: the last one that starts at or before it.
        std::size_t i = static_cast<std::size_t>(std::upper_bound(f->starts.begin(), f->starts.end(), from) - f->starts.begin()) - 1;
        std::uint64_t pos = from;
        while (pos < end && i < f->chunks.size()) {
            const std::shared_ptr<const Bytes> data = m->chunk(f->chunks[i]);
            const std::uint64_t in_chunk = pos - f->starts[i];
            if (in_chunk >= data->size()) break;  // shorter than the index said: stop there
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(data->size() - in_chunk, end - pos));
            std::memcpy(buf + (pos - from), data->data() + in_chunk, n);
            pos += n;
            ++i;
        }
        return static_cast<int>(pos - from);
    } catch (const std::exception&) {
        return -EIO;
    }
}

int on_release(const char*, struct fuse_file_info* fi) {
    delete reinterpret_cast<OpenFile*>(fi->fh);
    fi->fh = 0;
    return 0;
}

int on_readlink(const char* path, char* buf, std::size_t size) {
    SnapshotMount::Impl* m = self();
    if (size == 0) return -EINVAL;
    try {
        std::lock_guard<std::mutex> lock(m->mu);
        Node n;
        if (!m->resolve(path, n)) return -ENOENT;
        if (n.kind != NodeKind::Symlink) return -EINVAL;
        const std::size_t len = std::min(size - 1, n.link_target.size());
        std::memcpy(buf, n.link_target.data(), len);
        buf[len] = '\0';
        return 0;
    } catch (const std::exception&) {
        return -EIO;
    }
}

int on_statfs(const char*, struct statvfs* st) {
    SnapshotMount::Impl* m = self();
    std::memset(st, 0, sizeof *st);
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_blocks = static_cast<fsblkcnt_t>((m->snapshot.stats.bytes + 4095) / 4096);
    st->f_namemax = 255;
    return 0;
}

// Serves the kernel's requests until stopped (the wake pipe) or unmounted from outside.
void run_loop(SnapshotMount::Impl& m) {
    struct fuse_session* se = fuse_get_session(m.fs);
    const int fd = fuse_session_fd(se);
    struct fuse_buf buf{};
    for (;;) {
        struct pollfd p[2] = {{fd, POLLIN, 0}, {m.wake[0], POLLIN, 0}};
        const int r = ::poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (p[1].revents != 0) break;  // stop
        if ((p[0].revents & POLLIN) == 0) {
            if ((p[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) break;  // unmounted
            continue;
        }
        const int n = fuse_session_receive_buf(se, &buf);
        if (n == -EINTR || n == -EAGAIN) continue;
        if (n <= 0) break;  // -ENODEV: unmounted from outside
        fuse_session_process_buf(se, &buf);
        if (fuse_session_exited(se)) break;
    }
    std::free(buf.mem);
    m.ended = true;
}

void tear_down(SnapshotMount::Impl& m) {
    if (m.loop.joinable()) {
        const char c = 1;
        if (::write(m.wake[1], &c, 1) < 0) {
            // the pipe is never full; nothing else to do
        }
        m.loop.join();
    }
    if (m.fs != nullptr) {
        if (m.mounted) fuse_unmount(m.fs);
        fuse_destroy(m.fs);
        m.fs = nullptr;
        m.mounted = false;
    }
    for (int& fd : m.wake)
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    // Only a folder made for the mount: an empty one the user chose stays. (rmdir: only if empty.)
    if (m.made_folder && !m.folder.empty() && !is_mount_point(m.folder)) ::rmdir(m.folder.c_str());
}

}  // namespace

bool SnapshotMount::available() {
    // Root mounts by itself; everyone else through the setuid fusermount3.
    return ::access("/dev/fuse", R_OK | W_OK) == 0 && (::geteuid() == 0 || !find_program("fusermount3").empty());
}

const char* SnapshotMount::how_to_enable() {
    return "browsing a version as a folder needs FUSE 3: install it (Debian and Ubuntu: sudo apt install fuse3, "
           "Fedora: sudo dnf install fuse3, Arch: sudo pacman -S fuse3) and make sure /dev/fuse can be opened "
           "(in a container or sandbox, pass it in)";
}

SnapshotMount::SnapshotMount(Repository& repo, const Snapshot& snapshot, std::string folder)
    : impl_(std::make_unique<Impl>()) {
    if (!available()) throw Error(how_to_enable());
    Impl& m = *impl_;
    m.repo = &repo;
    m.snapshot = snapshot;
    m.folder = os::absolute_path(folder);
    m.uid = ::getuid();
    m.gid = ::getgid();
    const std::filesystem::path root(std::u8string(m.folder.begin(), m.folder.end()));
    std::error_code ec;
    m.made_folder = std::filesystem::create_directories(root, ec);
    if (ec) throw Error("cannot create " + m.folder + ": " + ec.message());
    if (!std::filesystem::is_empty(root, ec) || ec) {
        m.made_folder = false;  // not ours to remove
        throw Error(folder + " is not empty; choose an empty or new folder");
    }
    if (::pipe2(m.wake, O_CLOEXEC) != 0) {
        const int err = errno;
        tear_down(m);
        throw Error(std::string("cannot start the folder: ") + std::strerror(err));
    }

    struct fuse_operations ops{};
    ops.init = on_init;
    ops.getattr = on_getattr;
    ops.readdir = on_readdir;
    ops.open = on_open;
    ops.read = on_read;
    ops.release = on_release;
    ops.readlink = on_readlink;
    ops.statfs = on_statfs;
    // Read-only, for this account only; unmounted by fusermount3 if this process is killed.
    std::string name = "vaultik";
    std::string dash_o = "-o";
    std::string options = "ro,fsname=vaultik,subtype=vaultik,auto_unmount";
    char* argv[] = {name.data(), dash_o.data(), options.data(), nullptr};
    struct fuse_args args = FUSE_ARGS_INIT(3, argv);
    m.fs = fuse_new(&args, &ops, sizeof ops, &m);
    fuse_opt_free_args(&args);
    if (m.fs == nullptr) {
        tear_down(m);
        throw Error("cannot start FUSE for " + m.folder);
    }
    if (fuse_mount(m.fs, m.folder.c_str()) != 0) {
        tear_down(m);
        throw Error("cannot mount the version at " + m.folder + " (FUSE refused it; is fuse3 installed?)");
    }
    m.mounted = true;
    m.loop = std::thread([&m] { run_loop(m); });
}

SnapshotMount::~SnapshotMount() {
    tear_down(*impl_);
}

const std::string& SnapshotMount::folder() const {
    return impl_->folder;
}

bool SnapshotMount::active() const {
    return !impl_->ended;
}

#else  // no libfuse3 at build time

struct SnapshotMount::Impl {
    std::string folder;
};

bool SnapshotMount::available() {
    return false;
}

const char* SnapshotMount::how_to_enable() {
    return "this copy of Vaultik was built without FUSE 3, which browsing a version as a folder needs: install "
           "the FUSE 3 development files (Debian and Ubuntu: libfuse3-dev, Fedora: fuse3-devel, Arch and SteamOS: "
           "fuse3) and build it again";
}

SnapshotMount::SnapshotMount(Repository&, const Snapshot&, std::string) {
    throw Error(how_to_enable());
}

SnapshotMount::~SnapshotMount() = default;

const std::string& SnapshotMount::folder() const {
    return impl_->folder;
}

bool SnapshotMount::active() const {
    return false;
}

#endif

bool SnapshotMount::remove_leftover(const std::string& folder) {
    const std::string path = os::absolute_path(folder);
    if (is_mount_point(path)) {
        // Still mounted (its process was killed before fusermount3 could unmount it): lazily, so a
        // file manager still showing it does not keep it.
        const std::string fusermount = find_program("fusermount3");
        try {
            if (!fusermount.empty()) os::run_command({fusermount, "-u", "-z", "-q", path});
        } catch (const std::exception&) {
        }
        if (is_mount_point(path)) return false;  // never delete through a mount
    }
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return !ec;
}

}  // namespace vaultik
