// POSIX (Linux) implementation of the OS layer.

#include "vaultik/platform/os.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
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
#include <cstdlib>
#include <cstring>

#include "vaultik/util/resources.hpp"

namespace vaultik::os {

namespace {

int as_fd(std::intptr_t h) {
    return static_cast<int>(h);
}

std::uint64_t to_ticks(const struct timespec& ts) {
    if (ts.tv_sec == 0 && ts.tv_nsec == 0) return 0;
    return from_unix(static_cast<std::int64_t>(ts.tv_sec), static_cast<std::int64_t>(ts.tv_nsec));
}

struct timespec from_ticks(std::uint64_t ticks) {
    struct timespec ts{};
    if (ticks < kUnixEpochTicks) return ts;
    const std::uint64_t since = ticks - kUnixEpochTicks;
    ts.tv_sec = static_cast<time_t>(since / 10000000ull);
    ts.tv_nsec = static_cast<long>((since % 10000000ull) * 100);
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

void set_metadata(const std::string& path, const FileInfo& info) {
    const bool link = info.type == EntryType::Symlink;
    // Ownership first: chown clears set-user-ID bits that chmod then restores.
    if (::geteuid() == 0 && (info.uid != 0 || info.gid != 0))
        (void)::lchown(path.c_str(), static_cast<uid_t>(info.uid), static_cast<gid_t>(info.gid));
    if (!link && info.mode != 0) {
        // Through a descriptor opened without following links, so a path swapped for a link
        // cannot redirect the change. O_NONBLOCK: never hang on a pipe.
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0) {
            (void)::fchmod(fd, static_cast<mode_t>(info.mode));
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

// Linux: comes with the port (by filesystem UUID under /dev/disk/by-uuid and the mount table).
VolumeLocation volume_of(const std::string& /*path*/) {
    return {};
}

std::string volume_root(const std::string& /*volume*/) {
    return {};
}

Bytes protect_secret(ByteSpan plain, bool /*machine*/) {
    return Bytes(plain.begin(), plain.end());  // protected by the 0600 file mode
}

Bytes unprotect_secret(ByteSpan sealed) {
    return Bytes(sealed.begin(), sealed.end());
}

// ---- process ------------------------------------------------------------------------------

bool metered_connection() {
    return false;
}

void set_env(const std::string& name, const std::string& value) {
    ::setenv(name.c_str(), value.c_str(), 1);
}

int run_shell(const std::string& command, std::string* output) {
    return run_command({"/bin/sh", "-c", command}, output);
}

int run_shell(const std::string& command, const ShellOptions& options) {
    return run_command({"/bin/sh", "-c", command}, options.output);  // (no timeout yet on POSIX)
}

int run_command_elevated(const std::vector<std::string>&) {
    throw Error("running as administrator is only supported on Windows");
}

int run_command(const std::vector<std::string>& argv, std::string* output) {
    if (argv.empty()) throw Error("run_command: no program");
    int pipefd[2] = {-1, -1};
    if (output != nullptr && ::pipe2(pipefd, O_CLOEXEC) != 0) throw_last_error("cannot create pipe");

    const pid_t pid = ::fork();
    if (pid < 0) throw_last_error("cannot start " + argv[0]);
    if (pid == 0) {
        if (output != nullptr) {
            ::dup2(pipefd[1], STDOUT_FILENO);
            ::dup2(pipefd[1], STDERR_FILENO);
        }
        std::vector<char*> args;
        for (const std::string& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        ::_exit(127);
    }
    if (output != nullptr) {
        ::close(pipefd[1]);
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            output->append(buf, static_cast<std::size_t>(n));
        }
        ::close(pipefd[0]);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
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
    const std::string home = get_env("HOME");
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

std::optional<FileId> file_id(const std::string& /*path*/) {
    return std::nullopt;
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
