#include "vaultik/platform/loopback.hpp"

#include <cstdlib>

#include "vaultik/util/error.hpp"

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
// After <winsock2.h>.
#include <shellapi.h>
#include <windows.h>

#include "vaultik/util/win.hpp"

namespace vaultik::os {

namespace {

constexpr SOCKET kNone = INVALID_SOCKET;

void ensure_winsock() {
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!ok) throw Error("cannot start Windows networking");
}

std::string decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size()) {
            out.push_back(static_cast<char>(std::strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::map<std::string, std::string> query_of(std::string_view target) {
    std::map<std::string, std::string> out;
    const std::size_t q = target.find('?');
    if (q == std::string_view::npos) return out;
    std::string_view query = target.substr(q + 1);
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t end = query.find('&', pos);
        if (end == std::string_view::npos) end = query.size();
        const std::string_view part = query.substr(pos, end - pos);
        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) out[decode(part)] = "";
        else out[decode(part.substr(0, eq))] = decode(part.substr(eq + 1));
        pos = end + 1;
    }
    return out;
}

std::string html_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

void reply(SOCKET s, int status, const std::string& text) {
    const std::string body = "<!doctype html><html><head><meta charset=\"utf-8\"><title>Vaultik</title></head>"
                             "<body style=\"font-family:sans-serif;margin:3em\"><h2>Vaultik</h2><p>" +
                             html_escape(text) + "</p></body></html>";
    const std::string head = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Not Found") +
                             "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                             std::to_string(body.size()) + "\r\n\r\n";
    const std::string all = head + body;
    send(s, all.data(), static_cast<int>(all.size()), 0);
}

}  // namespace

LoopbackServer::LoopbackServer(int port) {
    ensure_winsock();
    // IPv4 first, to learn the port when any was asked for.
    SOCKET v4 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (v4 == kNone) throw Error("cannot open a local port for the browser sign-in");
    sockaddr_in a4{};
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a4.sin_port = htons(static_cast<u_short>(port));
    if (bind(v4, reinterpret_cast<sockaddr*>(&a4), sizeof a4) != 0 || listen(v4, 4) != 0) {
        closesocket(v4);
        throw Error("cannot listen on port " + std::to_string(port) +
                    " for the browser sign-in (is another program using it?)");
    }
    int len = sizeof a4;
    getsockname(v4, reinterpret_cast<sockaddr*>(&a4), &len);
    port_ = ntohs(a4.sin_port);
    sockets_[0] = v4;

    // ::1 on the same port, because browsers may try "localhost" over IPv6 first.
    SOCKET v6 = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (v6 != kNone) {
        sockaddr_in6 a6{};
        a6.sin6_family = AF_INET6;
        a6.sin6_addr = in6addr_loopback;
        a6.sin6_port = htons(static_cast<u_short>(port_));
        if (bind(v6, reinterpret_cast<sockaddr*>(&a6), sizeof a6) == 0 && listen(v6, 4) == 0) {
            sockets_[1] = v6;
        } else {
            closesocket(v6);
        }
    }
}

LoopbackServer::~LoopbackServer() {
    for (std::uintptr_t s : sockets_)
        if (s != static_cast<std::uintptr_t>(kNone)) closesocket(static_cast<SOCKET>(s));
}

std::map<std::string, std::string> LoopbackServer::wait(std::chrono::seconds timeout,
                                                        const std::function<bool()>& cancelled,
                                                        const std::string& done_message) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (cancelled && cancelled()) throw Error("sign-in cancelled");
        if (std::chrono::steady_clock::now() > deadline) throw Error("the browser sign-in was not finished in time");
        fd_set set;
        FD_ZERO(&set);
        for (std::uintptr_t s : sockets_)
            if (s != static_cast<std::uintptr_t>(kNone)) FD_SET(static_cast<SOCKET>(s), &set);
        timeval tv{0, 250000};
        if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
        for (std::uintptr_t ls : sockets_) {
            if (ls == static_cast<std::uintptr_t>(kNone) || !FD_ISSET(static_cast<SOCKET>(ls), &set)) continue;
            SOCKET c = accept(static_cast<SOCKET>(ls), nullptr, nullptr);
            if (c == kNone) continue;
            DWORD rto = 5000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rto), sizeof rto);
            std::string req;
            char buf[2048];
            while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384) {
                const int n = recv(c, buf, sizeof buf, 0);
                if (n <= 0) break;
                req.append(buf, static_cast<std::size_t>(n));
            }
            // "GET /path?query HTTP/1.1"
            const std::size_t sp1 = req.find(' ');
            const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
            const std::string target = sp2 == std::string::npos ? std::string() : req.substr(sp1 + 1, sp2 - sp1 - 1);
            auto params = query_of(target);
            if (params.empty()) {
                reply(c, 404, "Nothing here.");
                closesocket(c);
                continue;
            }
            const bool failed = params.contains("error");
            reply(c, 200,
                  failed ? "Signing in did not work: " + params["error"] + ". You can close this tab."
                         : done_message);
            closesocket(c);
            return params;
        }
    }
}

void open_in_browser(const std::string& url) {
    const auto r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r <= 32) throw Error("cannot open the web browser");
}

}  // namespace vaultik::os

#else

namespace vaultik::os {

LoopbackServer::LoopbackServer(int /*port*/) {
    throw Error("browser sign-in is only available on Windows for now");
}

LoopbackServer::~LoopbackServer() = default;

std::map<std::string, std::string> LoopbackServer::wait(std::chrono::seconds /*timeout*/,
                                                        const std::function<bool()>& /*cancelled*/,
                                                        const std::string& /*done_message*/) {
    return {};
}

void open_in_browser(const std::string& /*url*/) {
    throw Error("opening a browser is only available on Windows for now");
}

}  // namespace vaultik::os

#endif
