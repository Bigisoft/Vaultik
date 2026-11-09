#include "vaultik/platform/http.hpp"

#include <cstdlib>

#ifdef _WIN32
#include "vaultik/util/win.hpp"

#include <windows.h>
// After <windows.h>.
#include <winhttp.h>
#endif

namespace vaultik::http {

std::string Response::header(std::string_view name) const {
    for (const auto& [k, v] : headers)
        if (k == name) return v;
    return {};
}

std::string Url::host_header() const {
    const bool default_port = (scheme == "https" && port == 443) || (scheme == "http" && port == 80);
    return default_port ? host : host + ":" + std::to_string(port);
}

Url parse_url(const std::string& url) {
    Url u;
    const std::size_t colon = url.find("://");
    if (colon == std::string::npos) throw Error("not a web address: " + url);
    u.scheme = url.substr(0, colon);
    for (char& c : u.scheme)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (u.scheme != "http" && u.scheme != "https") throw Error("only http and https addresses work: " + url);
    const std::size_t start = colon + 3;
    std::size_t slash = url.find('/', start);
    if (slash == std::string::npos) slash = url.size();
    std::string authority = url.substr(start, slash - start);
    if (authority.empty() || authority.find('@') != std::string::npos) throw Error("bad web address: " + url);
    u.port = u.scheme == "https" ? 443 : 80;
    std::size_t port_colon = std::string::npos;
    if (authority.front() == '[') {  // [IPv6]:port
        const std::size_t close = authority.find(']');
        if (close == std::string::npos) throw Error("bad web address: " + url);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':') throw Error("bad web address: " + url);
            port_colon = close + 1;
        }
    } else {
        port_colon = authority.find(':');
    }
    if (port_colon != std::string::npos) {
        const std::string p = authority.substr(port_colon + 1);
        char* end = nullptr;
        const long n = std::strtol(p.c_str(), &end, 10);
        if (p.empty() || *end != '\0' || n < 1 || n > 65535) throw Error("bad port in " + url);
        u.port = static_cast<int>(n);
        authority.resize(port_colon);
    }
    u.host = authority;
    u.path_and_query = slash < url.size() ? url.substr(slash) : "/";
    return u;
}

#ifdef _WIN32

namespace {

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
};

[[noreturn]] void fail(const std::string& what) {
    const DWORD err = GetLastError();
    std::string reason;
    switch (err) {
        case ERROR_WINHTTP_TIMEOUT: reason = "the server did not answer in time"; break;
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: reason = "the server name was not found"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: reason = "cannot connect to the server"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: reason = "the connection was broken"; break;
        case ERROR_WINHTTP_SECURE_FAILURE: reason = "the server's certificate is not trusted"; break;
        default: reason = win32_message(err);
    }
    throw NetworkError(what + ": " + reason);
}

}  // namespace

Client::Client() {
    session_ = WinHttpOpen(L"Vaultik", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (session_ == nullptr) {
        // Before Windows 8.1 there is no automatic proxy type.
        session_ = WinHttpOpen(L"Vaultik", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (session_ == nullptr) fail("cannot start the web client");
    // Resolve, connect, send, receive. Uploads of large packs over slow links need long send times.
    WinHttpSetTimeouts(session_, 30000, 30000, 300000, 300000);
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session_, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
}

Client::~Client() {
    if (session_ != nullptr) WinHttpCloseHandle(session_);
}

Response Client::send(const Request& request) {
    const Url url = parse_url(request.url);
    const std::string what = request.method + " " + url.host_header();

    Handle connect{WinHttpConnect(session_, widen(url.host).c_str(), static_cast<INTERNET_PORT>(url.port), 0)};
    if (connect.h == nullptr) fail(what);
    DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY;
    if (url.scheme == "https") flags |= WINHTTP_FLAG_SECURE;
    Handle req{WinHttpOpenRequest(connect.h, widen(request.method).c_str(), widen(url.path_and_query).c_str(),
                                  nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (req.h == nullptr) fail(what);
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof redirects);

    std::wstring headers;
    for (const auto& [k, v] : request.headers) headers += widen(k) + L": " + widen(v) + L"\r\n";
    if (request.body.size() > 0xFFFFFFFFull) throw Error("request body too large");
    const DWORD body_size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            headers.empty() ? 0 : static_cast<DWORD>(-1L),
                            body_size > 0 ? const_cast<std::uint8_t*>(request.body.data()) : WINHTTP_NO_REQUEST_DATA,
                            body_size, body_size, 0))
        fail(what);
    if (!WinHttpReceiveResponse(req.h, nullptr)) fail(what);

    Response resp;
    DWORD status = 0;
    DWORD size = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                             &status, &size, WINHTTP_NO_HEADER_INDEX))
        fail(what);
    resp.status = static_cast<int>(status);

    size = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (size > 0) {
        std::wstring raw(size / sizeof(wchar_t), L'\0');
        if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size,
                                WINHTTP_NO_HEADER_INDEX)) {
            raw.resize(size / sizeof(wchar_t));
            const std::string text = narrow(raw);
            std::size_t pos = text.find("\r\n");  // skip the status line
            while (pos != std::string::npos && pos + 2 < text.size()) {
                const std::size_t start = pos + 2;
                const std::size_t end = text.find("\r\n", start);
                const std::string line = text.substr(start, (end == std::string::npos ? text.size() : end) - start);
                const std::size_t c = line.find(':');
                if (c != std::string::npos) {
                    std::string name = line.substr(0, c);
                    for (char& ch : name)
                        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
                    std::size_t v = c + 1;
                    while (v < line.size() && line[v] == ' ') ++v;
                    resp.headers.emplace_back(std::move(name), line.substr(v));
                }
                pos = end;
            }
        }
    }

    if (request.method != "HEAD") {
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail)) fail(what);
            if (avail == 0) break;
            const std::size_t old = resp.body.size();
            resp.body.resize(old + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req.h, resp.body.data() + old, avail, &got)) fail(what);
            resp.body.resize(old + got);
            if (got == 0) break;
        }
    }
    return resp;
}

#else

Client::Client() = default;
Client::~Client() = default;

Response Client::send(const Request& /*request*/) {
    throw Error("cloud storage is not available on this system yet");
}

#endif

}  // namespace vaultik::http
