// The SFTP location syntax, kept apart from the backend (sftp_backend.cpp, which needs libssh2
// and is built on Windows only so far) so every platform can check a location typed in.

#include <algorithm>
#include <cctype>
#include <cstring>

#include "vaultik/repo/sftp_backend.hpp"

namespace vaultik {

namespace {

constexpr const char* kPrefix = "sftp:";

}  // namespace

SftpLocation parse_sftp_location(const std::string& location) {
    if (location.rfind(kPrefix, 0) != 0) throw Error("not an SFTP location: " + location);
    std::string rest = location.substr(std::strlen(kPrefix));
    if (rest.rfind("//", 0) == 0) rest = rest.substr(2);  // "sftp://user@host/..." too
    SftpLocation l;
    const std::size_t at = rest.find('@');
    const std::size_t slash = rest.find('/');
    if (at == std::string::npos || slash == std::string::npos || at > slash || at == 0)
        throw Error("an SFTP location is sftp:user@host[:port]/folder (" + location + ")");
    l.user = rest.substr(0, at);
    std::string host = rest.substr(at + 1, slash - at - 1);
    std::string port;
    if (!host.empty() && host.front() == '[') {  // an IPv6 address: "[fd00::5]" or "[fd00::5]:2200"
        const std::size_t close = host.find(']');
        if (close == std::string::npos) throw Error("an unfinished IPv6 address in " + location);
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') throw Error("not a server address in " + location);
            port = host.substr(close + 2);
            if (port.empty()) throw Error("not a port number in " + location);
        }
        host = host.substr(1, close - 1);
    } else if (const std::size_t colon = host.rfind(':'); colon != std::string::npos) {
        port = host.substr(colon + 1);
        host = host.substr(0, colon);
        if (port.empty()) throw Error("not a port number in " + location);
    }
    if (!port.empty()) {
        if (port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            throw Error("not a port number in " + location);
        l.port = std::stoi(port);
        if (l.port < 1 || l.port > 65535) throw Error("not a port number in " + location);
    }
    if (host.empty()) throw Error("an SFTP location needs a server name (" + location + ")");
    l.host = host;
    l.folder = rest.substr(slash);
    while (l.folder.size() > 1 && l.folder.back() == '/') l.folder.pop_back();
    if (l.folder.find("/../") != std::string::npos || l.folder.ends_with("/.."))
        throw Error("an SFTP folder must not contain \"..\" (" + location + ")");
    return l;
}

}  // namespace vaultik
