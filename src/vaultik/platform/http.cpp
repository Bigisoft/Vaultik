#include "vaultik/platform/http.hpp"

#include <cstdlib>

#ifdef _WIN32
#include "vaultik/util/win.hpp"

#include <windows.h>
// After <windows.h>.
#include <winhttp.h>
#else
#include <curl/curl.h>

#include <algorithm>
#include <cstring>
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

// libcurl, with the system's certificate store and the usual http_proxy / https_proxy variables.

namespace {

// One easy handle per thread, reset for every request: its connection cache keeps connections to
// the same server open between requests, as WinHTTP does.
struct ThreadHandle {
    CURL* h = nullptr;
    ~ThreadHandle() {
        if (h != nullptr) curl_easy_cleanup(h);
    }
};

void global_init() {
    static const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!ok) throw NetworkError("cannot start the web client");
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

struct Upload {
    ByteSpan body;
    std::size_t done = 0;
};

std::size_t on_body(char* data, std::size_t size, std::size_t count, void* user) {
    Bytes& body = *static_cast<Bytes*>(user);
    body.insert(body.end(), reinterpret_cast<const std::uint8_t*>(data),
                reinterpret_cast<const std::uint8_t*>(data) + size * count);
    return size * count;
}

std::size_t on_header(char* data, std::size_t size, std::size_t count, void* user) {
    auto& headers = *static_cast<std::vector<std::pair<std::string, std::string>>*>(user);
    std::string line(data, size * count);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    if (line.starts_with("HTTP/")) {
        headers.clear();  // a new response (after "100 Continue" or a proxy's answer)
    } else if (const std::size_t c = line.find(':'); c != std::string::npos) {
        std::size_t v = c + 1;
        while (v < line.size() && line[v] == ' ') ++v;
        headers.emplace_back(lower(line.substr(0, c)), line.substr(v));
    }
    return size * count;
}

std::size_t on_read(char* buffer, std::size_t size, std::size_t count, void* user) {
    Upload& u = *static_cast<Upload*>(user);
    const std::size_t n = std::min(size * count, u.body.size() - u.done);
    std::memcpy(buffer, u.body.data() + u.done, n);
    u.done += n;
    return n;
}

[[noreturn]] void fail(const std::string& what, CURLcode code, const char* detail) {
    std::string reason;
    switch (code) {
        case CURLE_OPERATION_TIMEDOUT: reason = "the server did not answer in time"; break;
        case CURLE_COULDNT_RESOLVE_HOST: reason = "the server name was not found"; break;
        case CURLE_COULDNT_CONNECT: reason = "cannot connect to the server"; break;
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE: reason = "the connection was broken"; break;
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_CONNECT_ERROR: reason = "the server's certificate is not trusted"; break;
        default: reason = curl_easy_strerror(code);
    }
    if (detail != nullptr && *detail != '\0') reason += " (" + std::string(detail) + ")";
    throw NetworkError(what + ": " + reason);
}

}  // namespace

Client::Client() {
    global_init();
}

Client::~Client() = default;

Response Client::send(const Request& request) {
    const Url url = parse_url(request.url);
    const std::string what = request.method + " " + url.host_header();
    thread_local ThreadHandle handle;
    if (handle.h == nullptr) handle.h = curl_easy_init();
    if (handle.h == nullptr) throw NetworkError(what + ": cannot start the web client");
    CURL* h = handle.h;
    curl_easy_reset(h);

    Response resp;
    char error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, error);
    curl_easy_setopt(h, CURLOPT_URL, request.url.c_str());
    curl_easy_setopt(h, CURLOPT_PATH_AS_IS, 1L);  // sent as is, like WinHTTP's escape-disable flags
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);  // threads
    curl_easy_setopt(h, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    curl_easy_setopt(h, CURLOPT_USERAGENT, "Vaultik");
    // As on Windows: 30 s to connect; a transfer that stalls (under 1 byte/s) for 5 minutes fails.
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 300L);

    curl_slist* headers = nullptr;
    bool has_type = false;
    bool has_accept = false;
    for (const auto& [k, v] : request.headers) {
        const std::string name = lower(k);
        has_type = has_type || name == "content-type";
        has_accept = has_accept || name == "accept";
        headers = curl_slist_append(headers, (k + ": " + v).c_str());
    }
    // No headers the caller did not ask for (signed requests list exactly what they send).
    if (!has_type) headers = curl_slist_append(headers, "Content-Type:");
    if (!has_accept) headers = curl_slist_append(headers, "Accept:");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);

    Upload upload{request.body, 0};
    if (request.method == "HEAD") {
        curl_easy_setopt(h, CURLOPT_NOBODY, 1L);
    } else if (request.method == "GET" && request.body.empty()) {
        curl_easy_setopt(h, CURLOPT_HTTPGET, 1L);
    } else {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!request.body.empty() || request.method == "PUT" || request.method == "POST" || request.method == "PATCH") {
            // Read from memory as it is sent; no copy of a large pack.
            curl_easy_setopt(h, CURLOPT_UPLOAD, 1L);
            curl_easy_setopt(h, CURLOPT_READFUNCTION, on_read);
            curl_easy_setopt(h, CURLOPT_READDATA, &upload);
            curl_easy_setopt(h, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(request.body.size()));
        }
    }
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &resp.body);
    curl_easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(h, CURLOPT_HEADERDATA, &resp.headers);

    const CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(headers);
    if (rc != CURLE_OK) fail(what, rc, error);
    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    resp.status = static_cast<int>(status);
    return resp;
}

#endif

}  // namespace vaultik::http
