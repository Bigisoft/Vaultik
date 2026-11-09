#pragma once

// A small HTTP(S) client for cloud storage: one request at a time per call, whole bodies in memory.
// Windows uses WinHTTP (the system's TLS, proxy settings and certificate store); other systems are
// not supported yet.

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vaultik/util/bytes.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::http {

struct Request {
    std::string method;  // "GET", "PUT", "HEAD", "DELETE", ...
    // "https://host[:port]/path?query", with path and query already percent-encoded; sent as is.
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;  // besides Host and Content-Length
    ByteSpan body;
};

struct Response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;  // names in lower case
    Bytes body;

    // The value of header `name` (lower case), or empty.
    std::string header(std::string_view name) const;
};

// No HTTP response at all: the name did not resolve, the connection failed or broke, a timeout.
class NetworkError : public Error {
public:
    using Error::Error;
};

// The parts of a URL: scheme "http" or "https", host, port, and path with query ("/" at least).
struct Url {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string path_and_query;

    // "host" for the scheme's default port, "host:port" otherwise (the Host header).
    std::string host_header() const;
};
// Throws Error for anything that is not an absolute http or https URL.
Url parse_url(const std::string& url);

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Sends the request and reads the whole response. Thread-safe. Throws NetworkError when no
    // response arrives; any HTTP status is returned, never thrown. Redirects are not followed.
    Response send(const Request& request);

private:
    void* session_ = nullptr;
};

}  // namespace vaultik::http
