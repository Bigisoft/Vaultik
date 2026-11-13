#include "vaultik/platform/progress_channel.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/hex.hpp"

#ifdef _WIN32
#include <windows.h>

#include <sddl.h>

#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"
#else
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace vaultik::os {

namespace {

constexpr std::string_view kPrefix = "vaultik-progress-";
constexpr std::size_t kRandomBytes = 16;  // 32 hex digits in the name

bool lower_hex(std::string_view s) {
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// "vaultik-progress-" and 16 to 64 lowercase hex digits.
bool valid_leaf(std::string_view leaf) {
    if (!leaf.starts_with(kPrefix)) return false;
    const std::string_view rest = leaf.substr(kPrefix.size());
    return rest.size() >= 16 && rest.size() <= 64 && lower_hex(rest);
}

std::string random_leaf() {
    std::uint8_t rnd[kRandomBytes];
    random_bytes(rnd);
    return std::string(kPrefix) + to_hex(ByteSpan(rnd, sizeof rnd));
}

using Clock = std::chrono::steady_clock;

int ms_left(Clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left <= 0 ? 0 : static_cast<int>(left);
}

// Adds what the engine read from the app to `pending`, keeping at most kMaxControlLine bytes of
// any line: the rest of a longer one is dropped up to its end (`skipping` while it is).
void add_control_input(std::string& pending, bool& skipping, std::string_view data) {
    while (!data.empty()) {
        if (skipping) {
            const std::size_t nl = data.find('\n');
            if (nl == std::string_view::npos) return;
            data.remove_prefix(nl + 1);
            skipping = false;
            continue;
        }
        const std::size_t last = pending.rfind('\n');
        const std::size_t open = last == std::string::npos ? pending.size() : pending.size() - last - 1;
        const std::size_t nl = data.find('\n');
        const std::size_t part = nl == std::string_view::npos ? data.size() : nl;
        if (open + part <= kMaxControlLine) {
            const std::size_t take = nl == std::string_view::npos ? data.size() : nl + 1;
            pending.append(data.substr(0, take));
            data.remove_prefix(take);
            continue;
        }
        // Too long: its first kMaxControlLine bytes make the line, the rest is dropped.
        const std::size_t keep = kMaxControlLine - open;
        pending.append(data.substr(0, keep));
        pending += '\n';
        data.remove_prefix(keep);
        skipping = true;
    }
}

// Takes one line out of `pending` when it holds a whole one.
bool take_line(std::string& pending, std::string& line) {
    const std::size_t end = pending.find('\n');
    if (end == std::string::npos) return false;
    line = pending.substr(0, end);
    pending.erase(0, end + 1);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return true;
}

}  // namespace

bool valid_progress_channel_name(std::string_view name) {
#ifdef _WIN32
    constexpr std::string_view kPipes = "\\\\.\\pipe\\";
    return name.starts_with(kPipes) && valid_leaf(name.substr(kPipes.size()));
#else
    // An absolute path whose last part is the channel's own name; no "." or ".." parts, no
    // empty ones.
    if (name.size() < 2 || name.front() != '/' || name.back() == '/') return false;
    const std::size_t slash = name.rfind('/');
    if (!valid_leaf(name.substr(slash + 1))) return false;
    if (slash == 0) return true;
    const std::string_view dir = name.substr(1, slash - 1);
    std::size_t at = 0;
    for (;;) {
        const std::size_t next = dir.find('/', at);
        const std::string_view part = dir.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
        if (part.empty() || part == "." || part == "..") return false;
        if (next == std::string_view::npos) return true;
        at = next + 1;
    }
#endif
}

std::string progress_channel_user(const std::string& given, const char* pkexec_uid) {
#ifdef _WIN32
    (void)pkexec_uid;
    return given;
#else
    if (pkexec_uid == nullptr || pkexec_uid[0] == '\0') return given;
    if (given.empty()) return pkexec_uid;
    if (given != pkexec_uid)
        throw Error("--progress-user " + given + " is not the user that started this with pkexec (" + pkexec_uid + ")");
    return given;
#endif
}

EngineWait wait_for_engine(ProgressServer& server, const std::function<std::optional<int>()>& ended,
                           const std::function<bool()>& abort, int slow_ms, const std::function<void()>& slow) {
    EngineWait w;
    const auto slow_at = Clock::now() + std::chrono::milliseconds(slow_ms < 0 ? 0 : slow_ms);
    bool told = slow_ms < 0;
    while (!abort()) {
        const auto round = Clock::now();
        if (server.wait_connected(200)) {
            w.connected = true;
            return w;
        }
        if ((w.code = ended())) {
            // What it sent before it ended is still read.
            w.connected = server.wait_connected(0);
            return w;
        }
        if (!told && Clock::now() >= slow_at) {
            told = true;
            if (slow) slow();
        }
        // A channel that failed for good answers at once: the engine is still waited for, but
        // without keeping a processor busy.
        const auto spent = Clock::now() - round;
        if (spent < std::chrono::milliseconds(200)) std::this_thread::sleep_for(std::chrono::milliseconds(200) - spent);
    }
    return w;
}

void listen_for_cancel(ProgressClient& channel, std::atomic<bool>& cancel) noexcept {
    try {
        std::string line;
        while (channel.read_line(line)) {
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line == "cancel") cancel = true;
        }
    } catch (...) {
        // Out of memory or anything else: only the channel ends, never the task.
        try {
            channel.set_broken();
        } catch (...) {
        }
    }
}

#ifdef _WIN32

// ---- Windows: a named pipe -----------------------------------------------------------------------

namespace {

std::string token_user_sid(HANDLE token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (size == 0) throw_last_error("cannot read the user of a process");
    std::vector<std::uint64_t> buf((size + 7) / 8);  // aligned for TOKEN_USER
    if (!GetTokenInformation(token, TokenUser, buf.data(), size, &size)) throw_last_error("cannot read the user of a process");
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &text)) throw_last_error("cannot read the user of a process");
    const std::string sid = narrow(text);
    LocalFree(text);
    return sid;
}

std::string process_user_sid(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) throw_last_error("cannot read the user of a process");
    UniqueHandle t(token);
    return token_user_sid(t.get());
}

// Waits for an overlapped operation on `h` started with `ov`; the number of bytes it moved, or
// -1 on failure (with GetLastError() set), or -2 after `timeout_ms` (the operation is still
// running then).
long long finish_io(HANDLE h, OVERLAPPED& ov, DWORD timeout_ms) {
    if (WaitForSingleObject(ov.hEvent, timeout_ms) != WAIT_OBJECT_0) return -2;
    DWORD n = 0;
    if (!GetOverlappedResult(h, &ov, &n, FALSE)) return -1;
    return n;
}

// Stops an operation still running and waits until the system no longer uses its buffer.
void abandon_io(HANDLE h, OVERLAPPED& ov) {
    CancelIoEx(h, &ov);
    DWORD n = 0;
    GetOverlappedResult(h, &ov, &n, TRUE);
}

UniqueHandle new_event() {
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (e == nullptr) throw_last_error("cannot create an event");
    return UniqueHandle(e);
}

}  // namespace

struct ProgressServer::Impl {
    std::string name;
    std::string user;
    UniqueHandle pipe;
    UniqueHandle connect_event = new_event();
    UniqueHandle read_event = new_event();
    UniqueHandle write_event = new_event();
    OVERLAPPED connect_ov{};
    OVERLAPPED read_ov{};
    bool connecting = false;
    bool connected = false;
    bool checked = false;  // the client that connected is the expected one
    // Unset: any client is taken (expect_client was never called). 0: none is (the engine's
    // process id is not known).
    std::optional<unsigned long> expected_pid;
    std::function<bool()> engine_ended;
    int rejected = 0;
    bool reading = false;
    bool closed = false;
    char buf[4096];
    std::string pending;

    // Waits (overlapped) for the next process to connect.
    void listen() {
        ResetEvent(connect_event.get());
        connect_ov = OVERLAPPED{};
        connect_ov.hEvent = connect_event.get();
        if (ConnectNamedPipe(pipe.get(), &connect_ov)) {
            connected = true;
        } else {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) connecting = true;
            else throw_last_error("cannot wait for the engine on the progress channel");
        }
    }

    ~Impl() {
        if (!pipe.valid()) return;
        if (connecting) abandon_io(pipe.get(), connect_ov);
        if (reading) abandon_io(pipe.get(), read_ov);
        if (connected) DisconnectNamedPipe(pipe.get());
    }
};

ProgressServer::ProgressServer() : impl_(std::make_unique<Impl>()) {
    Impl& s = *impl_;
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) throw_last_error("cannot read this process's user");
        UniqueHandle t(token);
        s.user = token_user_sid(t.get());
    }
    // Only this user and the administrators (the engine may run as another administrator
    // account, when one was given at the approval) may open it; nobody may change that. Its
    // integrity label is medium, with no write up and no read up: a program of this user that
    // runs with low integrity (a sandboxed browser) can neither read nor write it.
    const std::wstring sddl = L"D:P(A;;GA;;;" + widen(s.user) + L")(A;;GA;;;BA)S:(ML;;NWNR;;;ME)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        throw_last_error("cannot describe who may open the progress channel");
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    s.name = "\\\\.\\pipe\\" + random_leaf();
    // The first instance: when another program made a pipe of that name before (it cannot
    // guess the random name, but still), this fails instead of joining that one.
    HANDLE h = CreateNamedPipeW(widen(s.name).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 64 * 1024,
                                64 * 1024, 0, &sa);
    const DWORD error = GetLastError();
    LocalFree(sd);
    if (h == INVALID_HANDLE_VALUE) {
        SetLastError(error);
        throw_last_error("cannot create the progress channel");
    }
    s.pipe.reset(h);
    s.connect_ov.hEvent = s.connect_event.get();
    s.read_ov.hEvent = s.read_event.get();
    s.listen();
}

ProgressServer::~ProgressServer() = default;

const std::string& ProgressServer::name() const {
    return impl_->name;
}

const std::string& ProgressServer::user() const {
    return impl_->user;
}

void ProgressServer::expect_client(unsigned long pid, std::function<bool()> engine_ended) {
    impl_->expected_pid = pid;
    impl_->engine_ended = std::move(engine_ended);
}

int ProgressServer::rejected_clients() const {
    return impl_->rejected;
}

bool ProgressServer::wait_connected(int timeout_ms) {
    Impl& s = *impl_;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        if (s.connected) {
            if (s.checked || !s.expected_pid) return s.checked = true;
            // Only the engine this app started: any other process that opened the pipe (one of
            // this user's, or an administrator's) is dropped, and the pipe waits for the engine.
            // An engine whose process id is not known (0) never matches: then nobody is taken.
            const unsigned long expected = *s.expected_pid;
            ULONG pid = 0;
            const bool known = expected != 0 && GetNamedPipeClientProcessId(s.pipe.get(), &pid) != FALSE;
            if (known && pid == expected) return s.checked = true;
            // The process id of a client that already closed its end may not be readable: when
            // the engine has ended meanwhile, that client was the engine, and the lines it sent
            // last (its error, the drives in use) are read. Its exit code still decides how it
            // ended.
            if (!known && expected != 0 && s.engine_ended && s.engine_ended()) return s.checked = true;
            ++s.rejected;
            DisconnectNamedPipe(s.pipe.get());
            s.connected = false;
            try {
                s.listen();
            } catch (const std::exception&) {
                s.closed = true;
                return false;
            }
            continue;
        }
        if (!s.connecting) return false;
        const long long r = finish_io(s.pipe.get(), s.connect_ov, static_cast<DWORD>(ms_left(deadline)));
        if (r == -2) return false;
        s.connecting = false;
        // A client that connected and left at once still counts: its lines (if any) are read and
        // then the channel reads as closed.
        s.connected = r >= 0 || GetLastError() == ERROR_PIPE_CONNECTED || GetLastError() == ERROR_NO_DATA;
        if (!s.connected) {
            s.closed = true;
            return false;
        }
    }
}

ProgressServer::Read ProgressServer::read_line(std::string& line, int timeout_ms) {
    Impl& s = *impl_;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (take_line(s.pending, line)) return Read::Line;
        if (s.closed) {
            if (!s.pending.empty()) {
                line.swap(s.pending);
                s.pending.clear();
                return Read::Line;
            }
            return Read::Closed;
        }
        if ((!s.connected || !s.checked) && !wait_connected(ms_left(deadline))) return s.closed ? Read::Closed : Read::Timeout;
        if (!s.reading) {
            ResetEvent(s.read_event.get());
            if (!ReadFile(s.pipe.get(), s.buf, sizeof s.buf, nullptr, &s.read_ov) && GetLastError() != ERROR_IO_PENDING) {
                s.closed = true;
                continue;
            }
            s.reading = true;
        }
        const long long n = finish_io(s.pipe.get(), s.read_ov, static_cast<DWORD>(ms_left(deadline)));
        if (n == -2) return Read::Timeout;
        s.reading = false;
        if (n < 0) {
            if (GetLastError() == ERROR_MORE_DATA) continue;  // not for a byte pipe, but harmless
            s.closed = true;
            continue;
        }
        if (n == 0) continue;
        s.pending.append(s.buf, static_cast<std::size_t>(n));
    }
}

bool ProgressServer::send_line(std::string_view line) {
    Impl& s = *impl_;
    if (!s.connected || !s.checked || s.closed) return false;
    std::string data(line);
    data += '\n';
    OVERLAPPED ov{};
    ov.hEvent = s.write_event.get();
    ResetEvent(ov.hEvent);
    if (!WriteFile(s.pipe.get(), data.data(), static_cast<DWORD>(data.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING)
        return false;
    const long long n = finish_io(s.pipe.get(), ov, 5000);
    if (n == -2) {
        abandon_io(s.pipe.get(), ov);
        return false;
    }
    return n == static_cast<long long>(data.size());
}

struct ProgressClient::Impl {
    UniqueHandle pipe;
    std::mutex write_lock;
    UniqueHandle write_event = new_event();
    UniqueHandle read_event = new_event();
    std::atomic<bool> broken{false};
    std::string pending;
    bool skipping = false;
};

ProgressClient::ProgressClient(const std::string& name, const std::string& expected_user, int timeout_ms)
    : impl_(std::make_unique<Impl>()) {
    if (!valid_progress_channel_name(name)) throw Error("not a Vaultik progress channel: " + name);
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    const std::wstring wname = widen(name);
    HANDLE h = INVALID_HANDLE_VALUE;
    for (;;) {
        // Anonymous: the app's end cannot act with this (administrator) process's rights.
        h = CreateFileW(wname.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        const DWORD e = GetLastError();
        if (ms_left(deadline) == 0) {
            SetLastError(e);
            throw_last_error("cannot connect to the app's progress channel");
        }
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(wname.c_str(), static_cast<DWORD>(std::min(ms_left(deadline), 1000)));
        else if (e == ERROR_FILE_NOT_FOUND) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        else throw_last_error("cannot connect to the app's progress channel");
    }
    impl_->pipe.reset(h);
    // The process serving the pipe must run as the expected user: a pipe of that name made by
    // another user's program gets nothing.
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h, &pid)) throw_last_error("cannot tell who serves the progress channel");
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p == nullptr) throw_last_error("cannot tell who serves the progress channel");
    UniqueHandle process(p);
    std::string want = expected_user;
    if (want.empty()) want = process_user_sid(GetCurrentProcess());
    const std::string owner = process_user_sid(process.get());
    if (_stricmp(owner.c_str(), want.c_str()) != 0)
        throw Error("the progress channel is served by another user (" + owner + ", not " + want + ")");
}

ProgressClient::~ProgressClient() {
    if (impl_->pipe.valid()) CancelIoEx(impl_->pipe.get(), nullptr);
}

bool ProgressClient::broken() const {
    return impl_->broken.load();
}

void ProgressClient::set_broken() {
    impl_->broken = true;
}

bool ProgressClient::send_line(std::string_view line, int write_timeout_ms) {
    Impl& c = *impl_;
    std::lock_guard<std::mutex> lock(c.write_lock);
    if (c.broken) return false;
    std::string data(line);
    data += '\n';
    OVERLAPPED ov{};
    ov.hEvent = c.write_event.get();
    ResetEvent(ov.hEvent);
    if (!WriteFile(c.pipe.get(), data.data(), static_cast<DWORD>(data.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
        c.broken = true;
        return false;
    }
    const long long n = finish_io(c.pipe.get(), ov, static_cast<DWORD>(write_timeout_ms));
    if (n == -2) abandon_io(c.pipe.get(), ov);
    if (n != static_cast<long long>(data.size())) {
        c.broken = true;
        return false;
    }
    return true;
}

bool ProgressClient::read_line(std::string& line) {
    Impl& c = *impl_;
    char buf[1024];
    for (;;) {
        if (take_line(c.pending, line)) return true;
        if (c.broken) return false;
        OVERLAPPED ov{};
        ov.hEvent = c.read_event.get();
        ResetEvent(ov.hEvent);
        if (!ReadFile(c.pipe.get(), buf, sizeof buf, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return false;
        const long long n = finish_io(c.pipe.get(), ov, INFINITE);
        if (n < 0) return false;
        add_control_input(c.pending, c.skipping, std::string_view(buf, static_cast<std::size_t>(n)));
    }
}

#else

// ---- POSIX: two FIFOs in a private directory -----------------------------------------------------

namespace {

// Writing to a FIFO whose reader is gone raises SIGPIPE, which would end the program: the
// channel reports it as a broken channel (EPIPE) instead.
void ignore_sigpipe() {
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
}

std::string runtime_dir() {
    if (const char* x = std::getenv("XDG_RUNTIME_DIR"); x != nullptr && x[0] == '/') {
        struct stat st {};
        if (::lstat(x, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 077) == 0) return x;
    }
    return "/tmp";
}

[[noreturn]] void fail(const std::string& what) {
    throw Error(what + ": " + std::strerror(errno));
}

// Writes all of `data` to the non-blocking `fd`, waiting up to `timeout_ms` for room.
bool write_all(int fd, const std::string& data, int timeout_ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    std::size_t at = 0;
    while (at < data.size()) {
        const ssize_t n = ::write(fd, data.data() + at, data.size() - at);
        if (n > 0) {
            at += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno != EAGAIN) return false;
        pollfd p{fd, POLLOUT, 0};
        const int left = ms_left(deadline);
        if (left == 0 || ::poll(&p, 1, left) <= 0 || (p.revents & (POLLERR | POLLHUP))) return false;
    }
    return true;
}

}  // namespace

struct ProgressServer::Impl {
    std::string name;
    std::string user;
    int events = -1;   // read
    int control = -1;  // write, once the engine connected
    bool closed = false;
    std::string pending;

    ~Impl() {
        if (events >= 0) ::close(events);
        if (control >= 0) ::close(control);
        if (!name.empty()) {
            ::unlink((name + "/events").c_str());
            ::unlink((name + "/control").c_str());
            ::rmdir(name.c_str());
        }
    }
};

ProgressServer::ProgressServer() : impl_(std::make_unique<Impl>()) {
    ignore_sigpipe();
    Impl& s = *impl_;
    s.user = std::to_string(::getuid());
    const std::string dir = runtime_dir() + "/" + random_leaf();
    if (::mkdir(dir.c_str(), 0700) != 0) fail("cannot create the progress channel " + dir);
    s.name = dir;
    if (::chmod(dir.c_str(), 0700) != 0) fail("cannot protect the progress channel " + dir);
    for (const char* f : {"/events", "/control"})
        if (::mkfifo((dir + f).c_str(), 0600) != 0) fail("cannot create the progress channel " + dir + f);
    s.events = ::open((dir + "/events").c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (s.events < 0) fail("cannot open the progress channel " + dir);
}

ProgressServer::~ProgressServer() = default;

const std::string& ProgressServer::name() const {
    return impl_->name;
}

const std::string& ProgressServer::user() const {
    return impl_->user;
}

void ProgressServer::expect_client(unsigned long, std::function<bool()>) {}

int ProgressServer::rejected_clients() const {
    return 0;
}

bool ProgressServer::wait_connected(int timeout_ms) {
    Impl& s = *impl_;
    if (s.control >= 0) return true;
    // The engine opens "control" for reading first: opening it for writing succeeds from then on.
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        s.control = ::open((s.name + "/control").c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (s.control >= 0) return true;
        if (errno != ENXIO && errno != EINTR) return false;
        // An engine that connected and already left (it ended at once): what it sent is read.
        pollfd p{s.events, POLLIN, 0};
        if (::poll(&p, 1, 0) > 0 && (p.revents & (POLLIN | POLLHUP))) return true;
        const int left = ms_left(deadline);
        if (left == 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(left, 20)));
    }
}

ProgressServer::Read ProgressServer::read_line(std::string& line, int timeout_ms) {
    Impl& s = *impl_;
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    for (;;) {
        if (take_line(s.pending, line)) return Read::Line;
        if (s.closed) {
            if (!s.pending.empty()) {
                line.swap(s.pending);
                s.pending.clear();
                return Read::Line;
            }
            return Read::Closed;
        }
        // Before any writer came, the FIFO reads as empty (no hang-up): it is only waited on.
        pollfd p{s.events, POLLIN, 0};
        const int r = ::poll(&p, 1, ms_left(deadline));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return Read::Timeout;
        const ssize_t n = ::read(s.events, buf, sizeof buf);
        if (n > 0) {
            s.pending.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0 && (p.revents & POLLHUP)) {
            s.closed = true;
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            s.closed = true;
            continue;
        }
        if (ms_left(deadline) == 0) return Read::Timeout;
    }
}

bool ProgressServer::send_line(std::string_view line) {
    Impl& s = *impl_;
    if (s.control < 0) return false;
    std::string data(line);
    data += '\n';
    return write_all(s.control, data, 5000);
}

struct ProgressClient::Impl {
    int events = -1;   // write
    int control = -1;  // read
    std::mutex write_lock;
    std::atomic<bool> broken{false};
    std::string pending;
    bool skipping = false;

    // Also when the constructor of ProgressClient throws after it opened one of them.
    ~Impl() {
        if (events >= 0) ::close(events);
        if (control >= 0) ::close(control);
    }
};

ProgressClient::ProgressClient(const std::string& name, const std::string& expected_user, int timeout_ms)
    : impl_(std::make_unique<Impl>()) {
    ignore_sigpipe();
    if (!valid_progress_channel_name(name)) throw Error("not a Vaultik progress channel: " + name);
    uid_t want = ::getuid();
    if (!expected_user.empty()) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(expected_user.c_str(), &end, 10);
        if (end == expected_user.c_str() || *end != '\0') throw Error("not a user id: " + expected_user);
        want = static_cast<uid_t>(v);
    }
    // The directory and both FIFOs belong to that user and nobody else can open them. The
    // directory is opened once (not through a link) and checked, and the FIFOs are opened
    // through it: one swapped for another between the check and the open is never used.
    const int dir = ::open(name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) fail("cannot open the app's progress channel " + name);
    struct CloseDir {
        int fd;
        ~CloseDir() { ::close(fd); }
    } close_dir{dir};
    struct stat st {};
    if (::fstat(dir, &st) != 0) fail("cannot open the app's progress channel " + name);
    if (!S_ISDIR(st.st_mode) || st.st_uid != want || (st.st_mode & 077) != 0)
        throw Error("the progress channel " + name + " is not a private folder of the expected user");
    const auto check = [&](int fd, const std::string& path) {
        struct stat f {};
        if (::fstat(fd, &f) != 0) fail("cannot open the app's progress channel " + path);
        if (!S_ISFIFO(f.st_mode) || f.st_uid != want || (f.st_mode & 077) != 0)
            throw Error("the progress channel " + path + " is not a private pipe of the expected user");
    };
    Impl& c = *impl_;
    const std::string control = name + "/control";
    c.control = ::openat(dir, "control", O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (c.control < 0) fail("cannot open the app's progress channel " + control);
    check(c.control, control);
    const std::string events = name + "/events";
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        c.events = ::openat(dir, "events", O_WRONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (c.events >= 0) break;
        // ENXIO: nobody reads it (yet).
        if ((errno != ENXIO && errno != EINTR) || ms_left(deadline) == 0) fail("cannot open the app's progress channel " + events);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    check(c.events, events);
}

ProgressClient::~ProgressClient() = default;

bool ProgressClient::broken() const {
    return impl_->broken.load();
}

void ProgressClient::set_broken() {
    impl_->broken = true;
}

bool ProgressClient::send_line(std::string_view line, int write_timeout_ms) {
    Impl& c = *impl_;
    std::lock_guard<std::mutex> lock(c.write_lock);
    if (c.broken) return false;
    std::string data(line);
    data += '\n';
    if (!write_all(c.events, data, write_timeout_ms)) {
        c.broken = true;
        return false;
    }
    return true;
}

bool ProgressClient::read_line(std::string& line) {
    Impl& c = *impl_;
    char buf[1024];
    for (;;) {
        if (take_line(c.pending, line)) return true;
        pollfd p{c.control, POLLIN, 0};
        const int r = ::poll(&p, 1, -1);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) return false;
        const ssize_t n = ::read(c.control, buf, sizeof buf);
        if (n > 0) {
            add_control_input(c.pending, c.skipping, std::string_view(buf, static_cast<std::size_t>(n)));
            continue;
        }
        if (n == 0 && (p.revents & POLLHUP)) return false;  // the app closed its end
        if (n < 0 && errno != EAGAIN && errno != EINTR) return false;
    }
}

#endif

}  // namespace vaultik::os
