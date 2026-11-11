// POSIX (Linux) implementation of the OS layer.

#include "vaultik/platform/os.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/vfs.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik::os {

namespace {

int as_fd(std::intptr_t h) {
    return static_cast<int>(h);
}

// FILETIME ticks count from 1601, so times before 1970 (negative Unix times) still fit. 0 means
// "unset" in the repository format; a real time of exactly 1601-01-01 cannot occur on Linux.
// Nanoseconds are kept to 100 ns, the format's resolution.
std::uint64_t to_ticks(const struct timespec& ts) {
    const std::int64_t ticks = static_cast<std::int64_t>(ts.tv_sec) * 10000000ll + ts.tv_nsec / 100 +
                               static_cast<std::int64_t>(kUnixEpochTicks);
    return ticks > 0 ? static_cast<std::uint64_t>(ticks) : 1;
}

struct timespec from_ticks(std::uint64_t ticks) {
    const std::int64_t since = static_cast<std::int64_t>(ticks) - static_cast<std::int64_t>(kUnixEpochTicks);
    // Floor division, so 1969-12-31 23:59:59.5 becomes -1 s + 500 ms, not 0 s - 500 ms.
    std::int64_t sec = since / 10000000ll;
    std::int64_t rest = since % 10000000ll;
    if (rest < 0) {
        rest += 10000000ll;
        --sec;
    }
    struct timespec ts{};
    ts.tv_sec = static_cast<time_t>(sec);
    ts.tv_nsec = static_cast<long>(rest * 100);
    return ts;
}

FileInfo info_from(const struct stat& st) {
    FileInfo info;
    if (S_ISREG(st.st_mode)) {
        info.type = EntryType::File;
        info.size = static_cast<std::uint64_t>(st.st_size);
    } else if (S_ISDIR(st.st_mode)) {
        info.type = EntryType::Dir;
    } else if (S_ISLNK(st.st_mode)) {
        info.type = EntryType::Symlink;
    } else {
        info.type = EntryType::Other;
    }
    info.mode = static_cast<std::uint32_t>(st.st_mode & 07777);
    info.uid = static_cast<std::uint32_t>(st.st_uid);
    info.gid = static_cast<std::uint32_t>(st.st_gid);
    info.ctime = to_ticks(st.st_ctim);
    info.mtime = to_ticks(st.st_mtim);
    info.device = static_cast<std::uint64_t>(st.st_dev);
    return info;
}

// Retries a system call interrupted by a signal.
template <typename F>
auto retry(F f) {
    for (;;) {
        auto r = f();
        if (r != -1 || errno != EINTR) return r;
    }
}

void sync_parent_dir(const std::string& path) {
    const std::string parent = parent_of(path);
    if (parent.empty()) return;
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return;
    ::fsync(fd);
    ::close(fd);
}

}  // namespace

// ---- errors -------------------------------------------------------------------------------

ErrorCode last_error() {
    return static_cast<ErrorCode>(errno);
}

std::string error_message(ErrorCode code) {
    char buf[256];
    const int e = static_cast<int>(code);
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
    const char* msg = strerror_r(e, buf, sizeof buf);  // GNU variant returns the message
#else
    const char* msg = strerror_r(e, buf, sizeof buf) == 0 ? buf : "unknown error";
#endif
    return std::string(msg) + " (errno " + std::to_string(e) + ")";
}

bool is_not_found(ErrorCode code) {
    return code == ENOENT || code == ENOTDIR;
}

bool is_exists(ErrorCode code) {
    return code == EEXIST;
}

bool is_not_supported(ErrorCode code) {
    return code == ENOTSUP;
}

void throw_error(const std::string& what, ErrorCode code) {
    throw OsError(what, code);
}

void throw_last_error(const std::string& what) {
    throw OsError(what, static_cast<ErrorCode>(errno));
}

// ---- paths --------------------------------------------------------------------------------

std::string absolute_path(std::string_view path) {
    std::string input(path);
    if (input.empty()) input = ".";
    if (input.front() != '/') {
        char cwd[4096];
        if (::getcwd(cwd, sizeof cwd) == nullptr) throw_last_error("cannot read the current directory");
        input = std::string(cwd) + "/" + input;
    }
    std::vector<std::string> parts;
    std::size_t pos = 0;
    while (pos <= input.size()) {
        const std::size_t end = std::min(input.find('/', pos), input.size());
        const std::string part = input.substr(pos, end - pos);
        if (part.empty() || part == ".") {
            // skip
        } else if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(part);
        }
        pos = end + 1;
    }
    std::string out;
    for (const std::string& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

std::string join(std::string_view dir, std::string_view name) {
    std::string out(dir);
    if (out.empty() || out.back() != '/') out.push_back('/');
    out.append(name);
    return out;
}

std::string parent_of(std::string_view absolute) {
    if (absolute.empty() || absolute == "/") return {};
    const std::size_t last = absolute.find_last_of('/');
    if (last == std::string_view::npos) return {};
    if (last == 0) return "/";
    return std::string(absolute.substr(0, last));
}

std::vector<std::string> path_components(std::string_view absolute) {
    if (absolute.empty() || absolute.front() != '/') throw Error("not an absolute path: " + std::string(absolute));
    std::vector<std::string> comps;
    std::size_t pos = 0;
    while (pos < absolute.size()) {
        const std::size_t end = std::min(absolute.find('/', pos), absolute.size());
        if (end > pos) comps.emplace_back(absolute.substr(pos, end - pos));
        pos = end + 1;
    }
    return comps;
}

std::string fold_case(std::string_view utf8) {
    return std::string(utf8);
}

// ---- metadata -----------------------------------------------------------------------------

bool stat(const std::string& path, FileInfo& info, ErrorCode& err) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        err = static_cast<ErrorCode>(errno);
        return false;
    }
    info = info_from(st);
    err = 0;
    return true;
}

bool is_directory(const std::string& path) {
    struct stat st{};
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool exists(const std::string& path) {
    struct stat st{};
    return ::lstat(path.c_str(), &st) == 0;
}

struct DirReader::Impl {
    DIR* dir = nullptr;
};

DirReader::DirReader(const std::string& path) : impl_(std::make_unique<Impl>()), path_(path) {
    impl_->dir = ::opendir(path.c_str());
    if (impl_->dir == nullptr) error_ = static_cast<ErrorCode>(errno);
}

DirReader::~DirReader() {
    if (impl_->dir != nullptr) ::closedir(impl_->dir);
}

bool DirReader::next(std::string& name, FileInfo& info) {
    if (impl_->dir == nullptr) return false;
    for (;;) {
        errno = 0;
        const struct dirent* e = ::readdir(impl_->dir);
        if (e == nullptr) {
            if (errno != 0) error_ = static_cast<ErrorCode>(errno);
            ::closedir(impl_->dir);
            impl_->dir = nullptr;
            return false;
        }
        const std::string_view n = e->d_name;
        if (n == "." || n == "..") continue;
        struct stat st{};
        if (::fstatat(::dirfd(impl_->dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;  // deleted while listing
            // Unreadable metadata: keep the name, store it without content.
            info = FileInfo{};
            info.type = EntryType::Other;
        } else {
            info = info_from(st);
        }
        name.assign(n);
        return true;
    }
}

// ---- files --------------------------------------------------------------------------------

File::~File() {
    close();
}

File::File(File&& other) noexcept : h_(other.h_) {
    other.h_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        close();
        h_ = other.h_;
        other.h_ = -1;
    }
    return *this;
}

File File::open_read(const std::string& path, ErrorCode& err) {
    const int fd = retry([&] { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); });
    if (fd < 0) {
        err = static_cast<ErrorCode>(errno);
        return File();
    }
    err = 0;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    return File(fd);
}

File File::open_source(const std::string& path, ErrorCode& err) {
    // O_NOATIME keeps backups from touching access times; only allowed for the owner or root.
    // O_NOFOLLOW and the S_ISREG check below: the entry was a regular file when it was scanned;
    // if it has been swapped for a link (or a pipe) since, refuse it instead of reading whatever it
    // points to with the backup's (possibly root) rights. O_NONBLOCK keeps a pipe from hanging.
    constexpr int kFlags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    int fd = retry([&] { return ::open(path.c_str(), kFlags | O_NOATIME); });
    if (fd < 0 && errno == EPERM) fd = retry([&] { return ::open(path.c_str(), kFlags); });
    if (fd < 0) {
        err = static_cast<ErrorCode>(errno);
        return File();
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        err = ELOOP;
        return File();
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    err = 0;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
    return File(fd);
}

File File::open_random(const std::string& path, ErrorCode& err) {
    const int fd = retry([&] { return ::open(path.c_str(), O_RDONLY | O_CLOEXEC); });
    if (fd < 0) {
        err = static_cast<ErrorCode>(errno);
        return File();
    }
    err = 0;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    return File(fd);
}

File File::open_append(const std::string& path, ErrorCode& err, std::uint32_t mode) {
    const int fd = retry(
        [&] { return ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, static_cast<mode_t>(mode)); });
    if (fd < 0) {
        err = static_cast<ErrorCode>(errno);
        return File();
    }
    err = 0;
    return File(fd);
}

File File::create(const std::string& path, bool overwrite, ErrorCode& err, std::uint32_t mode) {
    // O_EXCL never follows a link at `path`. To overwrite, the old entry is unlinked first rather
    // than truncated: unlink removes a link itself, not its target, and a fresh file cannot be a
    // hard link to some other file.
    constexpr int kFlags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const int fd = retry([&] { return ::open(path.c_str(), kFlags, static_cast<mode_t>(mode)); });
        if (fd >= 0) {
            err = 0;
            return File(fd);
        }
        err = static_cast<ErrorCode>(errno);
        if (!overwrite || err != EEXIST) return File();
        if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
            err = static_cast<ErrorCode>(errno);  // a folder, or not ours to delete
            return File();
        }
    }
    return File();
}

bool File::valid() const {
    return h_ >= 0;
}

void File::close() {
    if (valid()) ::close(as_fd(h_));
    h_ = -1;
}

std::size_t File::read(std::uint8_t* dst, std::size_t max) {
    const ssize_t got = retry([&] { return ::read(as_fd(h_), dst, std::min<std::size_t>(max, 1u << 30)); });
    if (got < 0) throw_last_error("read failed");
    throttle_read(static_cast<std::size_t>(got));
    return static_cast<std::size_t>(got);
}

void File::read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length) {
    std::size_t done = 0;
    while (done < length) {
        const ssize_t got = retry([&] {
            return ::pread(as_fd(h_), dst + done, std::min<std::size_t>(length - done, 16u << 20),
                           static_cast<off_t>(offset + done));
        });
        if (got < 0) throw_last_error("read failed");
        if (got == 0) throw Error("file is truncated");
        done += static_cast<std::size_t>(got);
        throttle_read(static_cast<std::size_t>(got));
    }
}

void File::write_all(ByteSpan data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t wrote = retry([&] {
            return ::write(as_fd(h_), data.data() + done, std::min<std::size_t>(data.size() - done, 16u << 20));
        });
        if (wrote < 0) throw_last_error("write failed");
        done += static_cast<std::size_t>(wrote);
        throttle_write(static_cast<std::size_t>(wrote));
    }
}

void File::sync() {
    if (retry([&] { return ::fsync(as_fd(h_)); }) != 0) throw_last_error("flush failed");
}

void File::preallocate(std::uint64_t size) {
#ifdef __linux__
    // Plain fallocate fails quickly where unsupported; posix_fallocate would write zeros instead.
    if (size > 0) (void)::fallocate(as_fd(h_), 0, 0, static_cast<off_t>(size));
#else
    (void)size;
#endif
}

ErrorCode make_dir(const std::string& path) {
    return ::mkdir(path.c_str(), 0755) == 0 ? 0 : static_cast<ErrorCode>(errno);
}

void rename_replace(const std::string& from, const std::string& to) {
    if (::rename(from.c_str(), to.c_str()) != 0) throw_last_error("cannot finalize " + to);
    sync_parent_dir(to);  // make the new directory entry durable
}

ErrorCode remove_file(const std::string& path) {
    return ::unlink(path.c_str()) == 0 ? 0 : static_cast<ErrorCode>(errno);
}

void make_writable(const std::string& /*path*/) {}

void make_private_dirs(const std::string& path) {
    make_dirs(path);
    // Without following a link, and only a folder of this account (a shared folder is left as is).
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) throw_last_error("cannot open folder " + path);
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_uid == ::geteuid() && (st.st_mode & 077) != 0) (void)::fchmod(fd, 0700);
    ::close(fd);
}

void set_metadata(const std::string& path, const FileInfo& info) {
    const bool link = info.type == EntryType::Symlink;
    // Ownership first: chown clears set-user-ID bits that chmod then restores.
    if (::geteuid() == 0 && (info.uid != 0 || info.gid != 0))
        (void)::lchown(path.c_str(), static_cast<uid_t>(info.uid), static_cast<gid_t>(info.gid));
    // Mode 0 with Windows attributes comes from a snapshot made on Windows, which has no
    // permissions to restore (the file keeps the umask default); with attributes 0 it is a real
    // mode 0 from Linux, unless the entry has no times either: then it is one of the folders a
    // backup puts above its sources, which were never read and carry no metadata.
    if (!link && (info.mode != 0 || (info.attributes == 0 && (info.mtime != 0 || info.ctime != 0)))) {
        // Through an O_PATH descriptor opened without following links, so a path swapped for a
        // link cannot redirect the change. O_PATH needs no read access (a file restored with mode
        // 0200 could not be opened for reading) and never opens a pipe. fchmod does not take an
        // O_PATH descriptor, but chmod on its /proc/self/fd entry does, and that entry names
        // exactly the file opened.
        const int fd = ::open(path.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            struct stat st{};
            if (::fstat(fd, &st) == 0 && !S_ISLNK(st.st_mode)) {
                const std::string proc = "/proc/self/fd/" + std::to_string(fd);
                (void)::chmod(proc.c_str(), static_cast<mode_t>(info.mode));
            }
            ::close(fd);
        }
    }
    if (info.mtime != 0) {
        struct timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;  // leave the access time alone
        times[1] = from_ticks(info.mtime);
        (void)::utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW);
    }
}

ErrorCode make_symlink(const std::string& target, const std::string& path) {
    return ::symlink(target.c_str(), path.c_str()) == 0 ? 0 : static_cast<ErrorCode>(errno);
}

ErrorCode read_symlink(const std::string& path, std::string& target) {
    std::string buf(256, '\0');
    for (;;) {
        const ssize_t n = ::readlink(path.c_str(), buf.data(), buf.size());
        if (n < 0) return static_cast<ErrorCode>(errno);
        if (static_cast<std::size_t>(n) < buf.size()) {
            buf.resize(static_cast<std::size_t>(n));
            target = std::move(buf);
            return 0;
        }
        buf.resize(buf.size() * 2);
    }
}

bool is_pseudo_filesystem(const std::string& path) {
#ifdef __linux__
    struct statfs fs{};
    if (::statfs(path.c_str(), &fs) != 0) return false;
    switch (static_cast<unsigned long>(fs.f_type)) {
        case 0x9fa0:      // proc
        case 0x62656572:  // sysfs
        case 0x1cd1:      // devpts
        case 0x01021994:  // tmpfs and devtmpfs: memory only, gone after a reboot
        case 0x27e0eb:    // cgroup
        case 0x63677270:  // cgroup2
        case 0x64626720:  // debugfs
        case 0x74726163:  // tracefs
        case 0x73636673:  // securityfs
        case 0x6165676c:  // pstore
        case 0xcafe4a11:  // bpf
        case 0x65735543:  // fusectl
        case 0x62656570:  // configfs
        case 0x42494e4d:  // binfmt_misc
        case 0x19800202:  // mqueue
        case 0x958458f6:  // hugetlbfs
        case 0xde5e81e4:  // efivarfs
        case 0xf97cff8c:  // selinuxfs
        case 0x6e736673:  // nsfs
            return true;
        default:
            return false;
    }
#else
    (void)path;
    return false;
#endif
}

// ---- volumes: the mount table and file system UUIDs ----------------------------------------

namespace {

struct Mount {
    unsigned major = 0;
    unsigned minor = 0;
    std::string root;   // the folder of the file system mounted here ("/" unless a bind mount)
    std::string point;  // where it is mounted
    std::string type;
    std::string source;  // "/dev/sda1", or a name such as "tmpfs"
};

// Undoes the octal escapes of /proc/self/mountinfo ("\040" for a space).
std::string unescape_mount(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '3') {
            out.push_back(static_cast<char>(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0')));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<Mount> read_mounts() {
    std::vector<Mount> mounts;
    Bytes data;
    try {
        data = read_file("/proc/self/mountinfo", 16u << 20);
    } catch (const Error&) {
        return mounts;
    }
    const std::string text(data.begin(), data.end());
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        // "36 35 98:0 /mnt1 /mnt/parent rw,noatime master:1 - ext3 /dev/root rw,errors=continue"
        std::vector<std::string_view> f;
        const std::string_view line(text.data() + pos, end - pos);
        std::size_t p = 0;
        while (p < line.size()) {
            std::size_t q = line.find(' ', p);
            if (q == std::string_view::npos) q = line.size();
            f.push_back(line.substr(p, q - p));
            p = q + 1;
        }
        pos = end + 1;
        std::size_t dash = 0;
        for (std::size_t i = 6; i < f.size(); ++i)
            if (f[i] == "-") {
                dash = i;
                break;
            }
        if (f.size() < 7 || dash == 0 || dash + 1 >= f.size()) continue;
        Mount m;
        const std::string dev(f[2]);
        if (std::sscanf(dev.c_str(), "%u:%u", &m.major, &m.minor) != 2) continue;
        m.root = unescape_mount(f[3]);
        m.point = unescape_mount(f[4]);
        m.type = std::string(f[dash + 1]);
        if (dash + 2 < f.size()) m.source = unescape_mount(f[dash + 2]);
        // Btrfs (and other file systems with subvolumes) show an anonymous device number here;
        // the block device it lives on is the source.
        if (m.major == 0 && m.source.starts_with("/dev/")) {
            struct stat st{};
            if (::stat(m.source.c_str(), &st) == 0 && S_ISBLK(st.st_mode)) {
                m.major = ::major(st.st_rdev);
                m.minor = ::minor(st.st_rdev);
            }
        }
        mounts.push_back(std::move(m));
    }
    return mounts;
}

bool under(const std::string& path, const std::string& folder) {
    if (folder == "/") return true;
    return path == folder || (path.size() > folder.size() && path.starts_with(folder) && path[folder.size()] == '/');
}

// The mount `path` is on: the latest (top-most) mount whose point holds it.
const Mount* mount_of(const std::vector<Mount>& mounts, const std::string& path) {
    const Mount* best = nullptr;
    for (const Mount& m : mounts)
        if (under(path, m.point) && (best == nullptr || m.point.size() >= best->point.size())) best = &m;
    return best;
}

bool network_type(const std::string& type) {
    return type == "nfs" || type == "nfs4" || type == "cifs" || type == "smb3" || type == "smbfs" ||
           type == "ncpfs" || type == "afs" || type == "9p" || type.starts_with("fuse.sshfs") ||
           type == "fuse.rclone" || type == "davfs" || type == "fuse.davfs2" || type == "ceph" || type == "glusterfs";
}

constexpr const char* kByUuid = "/dev/disk/by-uuid";

// The file system UUID of a block device, from the udev links in /dev/disk/by-uuid.
std::string uuid_of(unsigned major, unsigned minor) {
    DirReader dir(kByUuid);
    std::string name;
    FileInfo info;
    while (dir.next(name, info)) {
        struct stat st{};
        if (::stat(join(kByUuid, name).c_str(), &st) == 0 && S_ISBLK(st.st_mode) && ::major(st.st_rdev) == major &&
            ::minor(st.st_rdev) == minor)
            return name;
    }
    return {};
}

// The sysfs folder of the whole disk a block device is on ("/sys/devices/.../block/sda").
std::string disk_of(unsigned major, unsigned minor) {
    char buf[4096];
    const std::string link = "/sys/dev/block/" + std::to_string(major) + ":" + std::to_string(minor);
    if (::realpath(link.c_str(), buf) == nullptr) return {};
    std::string dev = buf;
    if (exists(join(dev, "partition"))) dev = parent_of(dev);
    return dev;
}

std::string read_small(const std::string& path) {
    try {
        const Bytes b = read_file(path, 4096);
        std::string s(b.begin(), b.end());
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    } catch (const Error&) {
        return {};
    }
}

constexpr std::string_view kUuidPrefix = "uuid:";

}  // namespace

// Linux: the volume is named by its file system UUID ("uuid:1234-ABCD"), which stays the same
// wherever the drive is mounted (/run/media/<user>/<label> on SteamOS and most desktops).
VolumeLocation volume_of(const std::string& path) {
    VolumeLocation loc;
    const std::string full = absolute_path(path);
    const std::vector<Mount> mounts = read_mounts();
    const Mount* m = mount_of(mounts, full);
    if (m == nullptr || network_type(m->type) || m->major == 0) return loc;  // network, or no device (tmpfs, overlay)
    const std::string uuid = uuid_of(m->major, m->minor);
    if (uuid.empty()) return loc;
    loc.volume = std::string(kUuidPrefix) + uuid;
    loc.root = m->point;
    // Below the file system's own root, so a bind mount of a subfolder gives the same answer.
    std::string rel = full.size() > m->point.size() ? full.substr(m->point.size() + (m->point == "/" ? 0 : 1)) : "";
    if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
    if (m->root != "/") {
        // A bind mount: the root of the volume itself is wherever it is mounted whole.
        std::string inner = m->root.substr(1);
        rel = rel.empty() ? inner : join(inner, rel);
        const std::string whole = volume_root(loc.volume);
        loc.root = whole;
        if (whole.empty()) {
            loc.volume.clear();  // only this bind mount is there: cannot tell where the drive is
            return loc;
        }
    }
    loc.relative = rel;
    const std::string disk = disk_of(m->major, m->minor);
    const Mount* sys = mount_of(mounts, "/");
    loc.system = sys != nullptr && ((sys->major == m->major && sys->minor == m->minor) ||
                                    (!disk.empty() && disk == disk_of(sys->major, sys->minor)));
    // External: on a USB or FireWire bus, a memory card (the Steam Deck's microSD slot), or a disk
    // that calls itself removable.
    loc.external = !loc.system && !disk.empty() &&
                   (disk.find("/usb") != std::string::npos || disk.find("/firewire") != std::string::npos ||
                    disk.find("/mmc_host/") != std::string::npos || read_small(join(disk, "removable")) == "1");
    return loc;
}

std::string volume_root(const std::string& volume) {
    if (!volume.starts_with(kUuidPrefix)) return {};
    const std::string uuid = volume.substr(kUuidPrefix.size());
    if (uuid.empty() || uuid.find('/') != std::string::npos) return {};
    struct stat st{};
    if (::stat(join(kByUuid, uuid).c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) return {};
    const unsigned major = ::major(st.st_rdev);
    const unsigned minor = ::minor(st.st_rdev);
    // The shortest mount point of the whole file system ("/run/media/deck/USB"), not a bind mount.
    std::string best;
    for (const Mount& m : read_mounts())
        if (m.major == major && m.minor == minor && m.root == "/" && (best.empty() || m.point.size() < best.size()))
            best = m.point;
    return best;
}

std::optional<DiskSpace> disk_space(const std::string& path) {
    struct statfs st{};
    if (statfs(path.c_str(), &st) != 0) return std::nullopt;
    return DiskSpace{static_cast<std::uint64_t>(st.f_bavail) * st.f_bsize, static_cast<std::uint64_t>(st.f_blocks) * st.f_bsize};
}

// ---- secrets: sealed with a key kept in this account's own settings ---------------------------

namespace {

constexpr std::string_view kSealedMagic = "VKS1";  // AES-256-GCM with the account's local key
constexpr std::string_view kSealedAad = "vaultik local secret v1";

// Where the key lives: the settings folder of this account on this computer (VAULTIK_CONFIG_DIR,
// else $XDG_CONFIG_HOME/vaultik or ~/.config/vaultik), never the portable Settings-Linux folder
// next to the programs, so a backup drive carrying a portable copy does not carry the key too.
std::string secret_key_file() {
    std::string base = get_env("VAULTIK_CONFIG_DIR");
    if (!base.empty()) {
        base = absolute_path(base);
    } else {
        const std::string config = user_folder(UserFolder::RoamingAppData);
        if (config.empty()) throw Error("cannot keep a secret: the home folder is not known (HOME is not set)");
        base = join(config, "vaultik");
    }
    return join(join(base, "secrets"), "local.key");
}

// The key; created (random) when `create` and there is none yet. Empty when there is none.
std::optional<Key32> secret_key(bool create) {
    const std::string path = secret_key_file();
    const std::string dir = parent_of(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        try {
            if (is_directory(dir)) make_private_dirs(dir);  // tightens a folder left open
            Bytes b = read_file(path, 64);
            if (b.size() != 32) throw Error("the key for saved passwords is damaged: " + path);
            Key32 key{};
            std::memcpy(key.data(), b.data(), key.size());
            secure_wipe(b.data(), b.size());
            return key;
        } catch (const OsError& e) {
            if (!is_not_found(e.code())) throw;
        }
        if (!create) return std::nullopt;
        // Written whole under a temporary name, then linked in: link() never replaces, so two
        // processes making the key at once agree on one, and nobody reads a half-written key.
        make_private_dirs(dir);
        Key32 key{};
        random_bytes(key);
        std::uint8_t rnd[6];
        random_bytes(rnd);
        const std::string tmp = path + ".tmp-" + to_hex(rnd);
        ErrorCode err = 0;
        File f = File::create(tmp, false, err, 0600);
        if (!f.valid()) throw_error("cannot create " + tmp, err);
        try {
            f.write_all(ByteSpan(key.data(), key.size()));
            f.sync();
        } catch (...) {
            f.close();
            ::unlink(tmp.c_str());
            throw;
        }
        f.close();
        const int linked = ::link(tmp.c_str(), path.c_str());
        const int link_err = errno;
        ::unlink(tmp.c_str());
        if (linked == 0) {
            sync_parent_dir(path);
            return key;
        }
        secure_wipe(key.data(), key.size());
        if (link_err != EEXIST) throw_error("cannot create " + path, static_cast<ErrorCode>(link_err));
        // Another process made it first: read that one.
    }
    throw Error("cannot read the key for saved passwords: " + path);
}

}  // namespace

// Sealed with a random key kept in this account's own settings folder (secret_key_file), so a
// copy of the secrets alone (a portable copy's Settings-Linux folder on a backup drive) does not
// give them away. On top of that the files are 0600 in a 0700 folder.
Bytes protect_secret(ByteSpan plain, bool /*machine*/) {
    std::optional<Key32> key = secret_key(true);
    const AesGcm gcm(*key);
    secure_wipe(key->data(), key->size());
    const Bytes sealed = gcm.seal(plain, ByteSpan(reinterpret_cast<const std::uint8_t*>(kSealedAad.data()), kSealedAad.size()));
    Bytes out(kSealedMagic.begin(), kSealedMagic.end());
    out.insert(out.end(), sealed.begin(), sealed.end());
    return out;
}

Bytes unprotect_secret(ByteSpan sealed) {
    if (sealed.size() < kSealedMagic.size() ||
        std::memcmp(sealed.data(), kSealedMagic.data(), kSealedMagic.size()) != 0)
        throw Error("a saved secret is not in a form this program can read; enter it again");
    std::optional<Key32> key = secret_key(false);
    if (!key)
        throw Error("a saved secret was sealed on another computer or account (its key is not here); enter it again");
    const AesGcm gcm(*key);
    secure_wipe(key->data(), key->size());
    try {
        return gcm.open(sealed.subspan(kSealedMagic.size()),
                        ByteSpan(reinterpret_cast<const std::uint8_t*>(kSealedAad.data()), kSealedAad.size()));
    } catch (const AuthError&) {
        throw Error("a saved secret was sealed on another computer or account (another key); enter it again");
    }
}

// ---- process ------------------------------------------------------------------------------

bool metered_connection() {
    return false;
}

void set_env(const std::string& name, const std::string& value) {
    ::setenv(name.c_str(), value.c_str(), 1);
}

namespace {

struct Spawn {
    const std::vector<std::string>* argv = nullptr;
    std::string* output = nullptr;
    std::size_t max_output = 1u << 20;
    std::chrono::seconds timeout = std::chrono::hours(4);
    const std::function<bool()>* cancelled = nullptr;
    const std::vector<std::string>* environment = nullptr;  // null: this process's
    std::string directory;
};

// Starts a program with posix_spawn, which (unlike fork) is safe in a process with other threads
// (the lock refresh and the stdin reader) and reports a program that cannot be started as an
// error instead of exit code 127. Standard input is empty; standard output and error go to
// `output` or nowhere, never to this process's own (which may carry the --json event stream).
// The program runs in its own process group, so a timeout or cancel stops what it started too.
int spawn_and_wait(const Spawn& s) {
    const std::vector<std::string>& argv = *s.argv;
    if (argv.empty()) throw Error("run_command: no program");
    std::vector<char*> args;
    for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<char*> env;
    if (s.environment != nullptr) {
        for (const std::string& e : *s.environment) env.push_back(const_cast<char*>(e.c_str()));
        env.push_back(nullptr);
    }

    int pipefd[2] = {-1, -1};
    if (s.output != nullptr && ::pipe2(pipefd, O_CLOEXEC) != 0) throw_last_error("cannot create a pipe");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (s.output != nullptr) {
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    if (!s.directory.empty()) posix_spawn_file_actions_addchdir_np(&actions, s.directory.c_str());
#endif
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setpgroup(&attr, 0);
    // The child starts with default signal handling, whatever this process installed.
    sigset_t all;
    sigfillset(&all);
    posix_spawnattr_setsigdefault(&attr, &all);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, args[0], &actions, &attr, args.data(), s.environment != nullptr ? env.data() : environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (pipefd[1] >= 0) ::close(pipefd[1]);
    if (rc != 0) {
        if (pipefd[0] >= 0) ::close(pipefd[0]);
        throw_error("cannot start " + argv[0], static_cast<ErrorCode>(rc));
    }
    if (pipefd[0] >= 0) ::fcntl(pipefd[0], F_SETFL, ::fcntl(pipefd[0], F_GETFL) | O_NONBLOCK);

    const auto deadline = std::chrono::steady_clock::now() + s.timeout;
    const auto drain = [&] {
        if (pipefd[0] < 0) return false;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return n == 0;  // true at end of output
            const std::size_t room = s.max_output > s.output->size() ? s.max_output - s.output->size() : 0;
            s.output->append(buf, std::min(room, static_cast<std::size_t>(n)));
        }
    };
    int status = 0;
    std::string stop_reason;
    for (;;) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            drain();  // what is left in the pipe; programs it left running may keep it open
            break;
        }
        if (done < 0 && errno != EINTR) {
            // Its exit code is lost (it was reaped elsewhere): never report that as success.
            const int e = errno;
            if (pipefd[0] >= 0) ::close(pipefd[0]);
            throw_error("cannot wait for " + argv[0], static_cast<ErrorCode>(e));
        }
        if (s.cancelled != nullptr && *s.cancelled && (*s.cancelled)()) stop_reason = "cancelled";
        else if (std::chrono::steady_clock::now() > deadline) stop_reason = "did not finish in time";
        if (!stop_reason.empty()) {
            ::kill(-pid, SIGTERM);
            for (int i = 0; i < 20 && ::waitpid(pid, &status, WNOHANG) == 0; ++i) ::usleep(100000);
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            break;
        }
        if (pipefd[0] >= 0) {
            struct pollfd p{pipefd[0], POLLIN, 0};
            if (::poll(&p, 1, 250) > 0 && drain()) {
                ::close(pipefd[0]);
                pipefd[0] = -1;
            }
        } else {
            ::usleep(100000);
        }
    }
    if (pipefd[0] >= 0) ::close(pipefd[0]);
    if (!stop_reason.empty()) throw Error(argv[0] + " " + stop_reason);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

// The environment for a shell command: this process's, or with `clean` only a fixed PATH and
// the account's basics, plus `extra`.
std::vector<std::string> shell_environment(bool clean, const std::vector<std::pair<std::string, std::string>>& extra) {
    std::vector<std::string> env;
    if (clean) {
        env.emplace_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
        for (const char* name : {"HOME", "USER", "LOGNAME", "LANG", "TZ"})
            if (const char* v = std::getenv(name); v != nullptr) env.push_back(std::string(name) + "=" + v);
    } else {
        for (char** e = environ; *e != nullptr; ++e) env.emplace_back(*e);
    }
    for (const auto& [name, value] : extra) {
        const std::string prefix = name + "=";
        std::erase_if(env, [&](const std::string& e) { return e.starts_with(prefix); });
        env.push_back(prefix + value);
    }
    return env;
}

}  // namespace

int run_shell(const std::string& command, std::string* output) {
    ShellOptions options;
    options.output = output;
    return run_shell(command, options);
}

int run_shell(const std::string& command, const ShellOptions& options) {
    const std::vector<std::string> argv = {"/bin/sh", "-c", command};
    const std::vector<std::string> env = shell_environment(options.clean_environment, options.extra_environment);
    Spawn s;
    s.argv = &argv;
    s.output = options.output;
    s.max_output = 64u << 10;
    s.timeout = options.timeout;
    s.cancelled = &options.cancelled;
    s.environment = &env;
    s.directory = options.directory;
    return spawn_and_wait(s);
}

int run_command(const std::vector<std::string>& argv, const CommandOptions& options) {
    // The same environment rules as run_shell: a clean one when asked, extra variables on top.
    std::vector<std::string> env;
    const bool own_env = options.clean_environment || !options.extra_environment.empty();
    if (own_env) env = shell_environment(options.clean_environment, options.extra_environment);
    Spawn s;
    s.argv = &argv;
    s.output = options.output;
    s.timeout = options.timeout;
    s.cancelled = &options.cancelled;
    s.environment = own_env ? &env : nullptr;
    return spawn_and_wait(s);
}

int run_command_elevated(const std::vector<std::string>&, bool) {
    throw Error("running as administrator is only supported on Windows");
}

// Streamed programs are used for WSL and Hyper-V only, which exist on Windows alone.
struct PipedProcess::Impl {};
PipedProcess::PipedProcess(const std::vector<std::string>&, Direction,
                           const std::vector<std::pair<std::string, std::string>>&) {
    throw Error("streaming a program's output is only supported on Windows");
}
PipedProcess::~PipedProcess() = default;
std::size_t PipedProcess::read(std::uint8_t*, std::size_t) {
    return 0;
}
void PipedProcess::write(ByteSpan) {}
int PipedProcess::wait() {
    return -1;
}
void PipedProcess::stop() {}
std::string PipedProcess::messages() const {
    return {};
}

int run_command(const std::vector<std::string>& argv, std::string* output) {
    CommandOptions options;
    options.output = output;
    return run_command(argv, options);
}

std::string executable_path() {
    std::string buf(4096, '\0');
    const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
    if (n < 0) throw_last_error("cannot find the program path");
    buf.resize(static_cast<std::size_t>(n));
    return buf;
}

void enable_backup_privileges() {}
void enable_restore_privileges() {}

void system_log(LogLevel level, std::uint32_t event_id, const std::string& text) {
    const int priority = level == LogLevel::Error ? LOG_ERR : level == LogLevel::Warning ? LOG_WARNING : LOG_INFO;
    openlog("vaultik", LOG_PID, LOG_USER);
    syslog(priority, "[%u] %.30000s", static_cast<unsigned>(event_id), text.c_str());
    closelog();
}

std::string hostname() {
    char buf[256] = {};
    if (::gethostname(buf, sizeof buf - 1) != 0) return "unknown";
    return buf;
}

std::string machine_id() {
    for (const char* path : {"/etc/machine-id", "/var/lib/dbus/machine-id"}) {
        try {
            const Bytes b = read_file(path);
            std::string id(b.begin(), b.end());
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
            if (!id.empty()) return id;
        } catch (const Error&) {
        }
    }
    return "host:" + hostname();
}

std::string username() {
    if (const struct passwd* pw = ::getpwuid(::geteuid()); pw != nullptr && pw->pw_name != nullptr)
        return pw->pw_name;
    const std::string env = get_env("USER");
    return env.empty() ? "unknown" : env;
}

std::uint32_t process_id() {
    return static_cast<std::uint32_t>(::getpid());
}

bool process_alive(std::uint32_t pid) {
    if (pid == 0) return true;
    // No process has an id that big (pid_max is at most 2^22), and as a negative pid_t kill()
    // would test a whole process group instead.
    if (pid > static_cast<std::uint32_t>(std::numeric_limits<pid_t>::max())) return false;
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

std::uint64_t now() {
    struct timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return to_ticks(ts);
}

std::string format_time_local(std::uint64_t time) {
    if (time == 0) return "-";
    const struct timespec ts = from_ticks(time);
    struct tm local{};
    if (::localtime_r(&ts.tv_sec, &local) == nullptr) return "?";
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &local);
    return buf;
}

std::string get_env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

std::string user_folder(UserFolder folder) {
    std::string home = get_env("HOME");
    // Without HOME (some service managers and cron set none) the password database knows it.
    if (home.empty())
        if (const struct passwd* pw = ::getpwuid(::geteuid()); pw != nullptr && pw->pw_dir != nullptr) home = pw->pw_dir;
    if (home.empty()) return {};
    const auto xdg = [&](const char* var, const char* fallback) {
        const std::string v = get_env(var);
        return !v.empty() ? v : join(home, fallback);
    };
    switch (folder) {
        case UserFolder::Profile: return home;
        case UserFolder::Documents: return join(home, "Documents");
        case UserFolder::Desktop: return join(home, "Desktop");
        case UserFolder::Pictures: return join(home, "Pictures");
        case UserFolder::Music: return join(home, "Music");
        case UserFolder::Videos: return join(home, "Videos");
        case UserFolder::Downloads: return join(home, "Downloads");
        case UserFolder::SavedGames: return {};
        case UserFolder::RoamingAppData: return xdg("XDG_CONFIG_HOME", ".config");
        case UserFolder::LocalAppData: return xdg("XDG_DATA_HOME", ".local/share");
    }
    return {};
}

std::optional<FileId> File::id() const {
    return std::nullopt;
}

std::uint32_t File::names() const {
    struct stat st{};
    if (!valid() || ::fstat(as_fd(h_), &st) != 0 || st.st_nlink == 0) return 1;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(st.st_nlink, 0xFFFFFFFFu));
}

File File::create_directory(const std::string& /*path*/, ErrorCode& err) {
    err = ENOTSUP;
    return File();
}

File File::hold_directory(const std::string& /*path*/, bool /*real_folder*/, ErrorCode& err) {
    err = ENOTSUP;
    return File();
}

ErrorCode File::set_security(ByteSpan /*descriptor*/) {
    return ENOTSUP;
}

File File::open_for_delete(const std::string& /*path*/, ErrorCode& err) {
    err = ENOTSUP;
    return File();
}

std::string File::final_path() const {
    return {};
}

ErrorCode File::discard() {
    return ENOTSUP;
}

std::string final_path(const std::string& /*path*/) {
    return {};
}

std::string long_name(const std::string& path) {
    return path;
}

std::optional<FileId> file_id(const std::string& /*path*/) {
    return std::nullopt;
}

bool running_as_root() {
    return ::geteuid() == 0;
}

bool others_can_change(const std::string& path) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return true;
    return st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)) != 0;
}

File File::create_stream(const std::string& /*path*/, const std::string& /*name*/, const FileId& /*expected*/,
                         ErrorCode& err) {
    err = ENOTSUP;
    return File();
}

// Windows extras: POSIX keeps permissions in the node itself (mode, uid, gid).
Bytes read_security(const std::string& /*path*/) {
    return {};
}

ErrorCode write_security(const std::string& /*path*/, ByteSpan /*descriptor*/, const FileId* /*expected*/) {
    return ENOTSUP;
}

std::vector<StreamEntry> list_streams(const std::string& /*path*/) {
    return {};
}

std::string stream_path(const std::string& path, const std::string& name) {
    return path + ":" + name;
}

bool read_link(const std::string& /*path*/, ReparseLink& /*out*/) {
    return false;
}

ErrorCode create_link(const std::string& /*path*/, const ReparseLink& /*link*/, bool /*directory*/) {
    return ENOTSUP;
}

}  // namespace vaultik::os
