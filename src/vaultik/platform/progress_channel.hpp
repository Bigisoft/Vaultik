#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// A private two-way line channel between the app (running with the user's own rights) and the
// engine it started with administrator (root) rights: the engine sends its --json event lines
// (progress, steps, the drives in use, the result), the app sends "cancel". Nothing secret is
// ever sent over it.
//
// Windows: a named pipe \\.\pipe\vaultik-progress-<random>, made by the app as its first and
// only instance, that only the user and the Administrators group may open, and that programs
// running with low integrity (a sandboxed browser) may neither read nor write. The engine checks
// that the process serving it runs as the expected user; the app checks that the process that
// connected is the engine it started (expect_client) and drops any other.
// POSIX: a directory <runtime dir>/vaultik-progress-<random> of mode 0700 holding two FIFOs,
// "events" (engine to app) and "control" (app to engine). The engine checks that the directory
// and both FIFOs belong to the expected user and that nobody else can open them, opening the
// FIFOs through the directory it checked (no path is looked up twice).

namespace vaultik::os {

// True for a channel name the app makes (see above): anything else is refused by the engine,
// so it can never be pointed at another pipe or file.
bool valid_progress_channel_name(std::string_view name);

// The user the engine expects to serve its channel: `given` (--progress-user); POSIX, under
// pkexec, the user that ran pkexec (`pkexec_uid`, from PKEXEC_UID) when none is given, and an
// Error when one is given that is not that user (a root engine opens only the channel of the
// user that asked for it). Empty: this process's own user.
std::string progress_channel_user(const std::string& given, const char* pkexec_uid);

// The longest line the engine takes from the app ("cancel" is all it ever sends): the rest of
// a longer line is dropped, so a program that floods the channel cannot make it run out of
// memory.
constexpr std::size_t kMaxControlLine = 4096;

// The app's end. Creating it makes a new channel with a random name.
class ProgressServer {
public:
    // Throws Error when the channel cannot be made.
    ProgressServer();
    ~ProgressServer();
    ProgressServer(const ProgressServer&) = delete;
    ProgressServer& operator=(const ProgressServer&) = delete;

    // For --progress-pipe.
    const std::string& name() const;
    // This process's user, as ProgressClient expects it (Windows: a SID such as S-1-5-21-...;
    // POSIX: the user id), for --progress-user.
    const std::string& user() const;

    // Waits up to `timeout_ms` for the engine to connect; true once it did. Can be called again
    // after it returned false: an engine that connects later is still taken.
    bool wait_connected(int timeout_ms);

    // Windows: only the process `pid` (the engine started) is taken as the engine; another one
    // that connects is dropped at once and the channel waits on for the engine. Set it before
    // wait_connected. POSIX: nothing (a FIFO cannot tell who opened it; the directory is private
    // to the user instead).
    void expect_client(unsigned long pid);
    // How many processes that were not the expected one were dropped.
    int rejected_clients() const;

    enum class Read { Line, Timeout, Closed };
    // The next line (without its end), waiting up to `timeout_ms`. Closed once the engine's end
    // is gone (it exited) and every line it sent was read.
    Read read_line(std::string& line, int timeout_ms);
    // Sends one line; false when the engine is not connected or gone.
    bool send_line(std::string_view line);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The engine's end.
class ProgressClient {
public:
    // Connects to channel `name`, made by `expected_user` (as ProgressServer::user(); empty:
    // this process's own user). Throws Error when the name is not a channel name, nothing serves
    // it within `timeout_ms`, or it belongs to another user.
    ProgressClient(const std::string& name, const std::string& expected_user, int timeout_ms);
    ~ProgressClient();
    ProgressClient(const ProgressClient&) = delete;
    ProgressClient& operator=(const ProgressClient&) = delete;

    // Sends one line (the line end is added); safe from any thread. When the app's end is gone
    // or has not read anything for `write_timeout_ms`, the channel counts as broken: this and
    // every later call return false at once. The work itself goes on.
    bool send_line(std::string_view line, int write_timeout_ms = 10000);
    bool broken() const;
    // Waits for the next line from the app (for one reader thread); false once the channel is
    // closed or broken.
    bool read_line(std::string& line);
    // Marks the channel as broken (as when the app's end is gone): nothing is sent any more.
    void set_broken();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// How wait_for_engine ended.
struct EngineWait {
    bool connected = false;   // the engine connected
    std::optional<int> code;  // or it ended (its exit code) before it did
};
// The app, after it started the engine: waits until the engine connects to `server`, it ended
// (`ended` returns its exit code; it may throw), or `abort` returns true. When it has not
// connected after `slow_ms` (negative: never), `slow` is called once and the wait goes on, so
// an engine that connects later (a slow start, or a password prompt answered late) is still
// taken.
EngineWait wait_for_engine(ProgressServer& server, const std::function<std::optional<int>()>& ended,
                           const std::function<bool()>& abort, int slow_ms, const std::function<void()>& slow);

// The engine's listener (run on a thread of its own): reads the app's lines until the channel
// closes, and sets `cancel` on a line "cancel". When the app goes away (closed, or it crashed)
// it returns without touching `cancel`: the task is not stopped for that. It never throws: any
// failure only marks the channel as broken.
void listen_for_cancel(ProgressClient& channel, std::atomic<bool>& cancel) noexcept;

}  // namespace vaultik::os
