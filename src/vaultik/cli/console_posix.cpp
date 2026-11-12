// POSIX terminal I/O.

#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "vaultik/cli/console.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::cli {

namespace {

bool g_status_shown = false;
std::size_t g_status_len = 0;

volatile sig_atomic_t g_interrupted = 0;
// The terminal settings to put back when a signal arrives while the password prompt has echo off.
struct termios g_saved_tty{};
volatile sig_atomic_t g_tty_quiet = 0;

void on_signal(int sig) {
    if (g_tty_quiet) {
        ::tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tty);  // async-signal-safe
        g_tty_quiet = 0;
    }
    if (g_interrupted) {
        // The second one: stop now, the default way.
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return;
    }
    g_interrupted = 1;
}

void write_fd(int fd, std::string_view text) {
    std::size_t done = 0;
    while (done < text.size()) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        done += static_cast<std::size_t>(n);
    }
}

// Code points, for padding and truncation of UTF-8 text.
std::size_t width(std::string_view s) {
    std::size_t n = 0;
    for (const unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

// Byte offset of the code point with index `cp`.
std::size_t offset_of(std::string_view s, std::size_t cp) {
    std::size_t seen = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == cp) return i;
            ++seen;
        }
    }
    return s.size();
}

}  // namespace

void init_console() {
    struct sigaction sa{};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART: a blocking read returns, so the prompt can stop
    for (const int sig : {SIGINT, SIGTERM, SIGHUP}) ::sigaction(sig, &sa, nullptr);
    // Writing to a closed pipe (the GUI went away) gives an error instead of ending the run.
    ::signal(SIGPIPE, SIG_IGN);
}

bool interrupted() {
    return g_interrupted != 0;
}

void out(std::string_view text) {
    clear_status();
    write_fd(STDOUT_FILENO, text);
}

void err(std::string_view text) {
    clear_status();
    write_fd(STDERR_FILENO, text);
}

void status(std::string_view text) {
    if (!::isatty(STDERR_FILENO)) return;
    constexpr std::size_t kMaxWidth = 110;
    std::string t(text);
    const std::size_t w = width(t);
    if (w > kMaxWidth) t = t.substr(0, offset_of(t, 40)) + "..." + t.substr(offset_of(t, w - (kMaxWidth - 43)));
    std::string line = "\r" + t;
    const std::size_t tw = width(t);
    if (tw < g_status_len) line.append(g_status_len - tw, ' ');
    write_fd(STDERR_FILENO, line);
    g_status_shown = true;
    g_status_len = tw;
}

void clear_status() {
    if (!g_status_shown) return;
    write_fd(STDERR_FILENO, "\r" + std::string(g_status_len, ' ') + "\r");
    g_status_shown = false;
    g_status_len = 0;
}

std::string read_password(std::string_view prompt, std::string_view without_console) {
    if (!::isatty(STDIN_FILENO))
        throw Error(without_console.empty() ? std::string("no terminal to ask for the password; set VAULTIK_PASSWORD or use --password-file")
                                            : std::string(without_console));
    err(prompt);
    struct termios old{};
    ::tcgetattr(STDIN_FILENO, &old);
    struct termios quiet = old;
    quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
    quiet.c_lflag |= ICANON;
    g_saved_tty = old;
    g_tty_quiet = 1;
    ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    // Echo comes back however this ends: a normal return, an exception, or a signal (on_signal).
    struct Restore {
        const struct termios& tty;
        ~Restore() {
            if (g_tty_quiet) ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &tty);
            g_tty_quiet = 0;
        }
    } restore{old};
    std::string line;
    char c = 0;
    for (;;) {
        if (g_interrupted) {
            secure_wipe(line.data(), line.size());
            err("\n");
            throw Error("cancelled");
        }
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || c == '\n') break;
        line.push_back(c);
    }
    c = 0;
    err("\n");
    while (!line.empty() && line.back() == '\r') line.pop_back();
    std::string result = line;
    secure_wipe(line.data(), line.size());
    return result;
}

std::string read_line(std::string_view prompt) {
    err(prompt);
    std::string line;
    char c = 0;
    bool any = false;
    for (;;) {
        const ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        any = true;
        if (c == '\n') break;
        line.push_back(c);
    }
    if (!any) throw Error("no more input");
    while (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

}  // namespace vaultik::cli
