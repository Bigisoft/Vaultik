#include "vaultik/repo/webdav_backend.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <functional>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/base64.hpp"

namespace vaultik {

namespace {

constexpr const char* kPrefix = "webdav:";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

std::string percent_decode(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
            out += static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// XML text: the few entities a server uses in an href.
std::string xml_unescape(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, std::strlen(from), to);
            pos += std::strlen(to);
        }
    }
    return s;
}

// The content of every element named `local` (any namespace prefix) inside `xml`.
std::vector<std::string> elements(const std::string& xml, const std::string& local) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const std::size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos + 1, end - pos - 1);
        if (tag.empty() || tag[0] == '/' || tag[0] == '?' || tag[0] == '!') {
            pos = end + 1;
            continue;
        }
        const bool self_closing = tag.back() == '/';
        if (self_closing) tag.pop_back();
        const std::string name = tag.substr(0, tag.find_first_of(" \t\r\n"));
        const std::size_t colon = name.find(':');
        const std::string prefix = colon == std::string::npos ? std::string() : name.substr(0, colon + 1);
        const std::string bare = colon == std::string::npos ? name : name.substr(colon + 1);
        if (lower(bare) != lower(local)) {
            pos = end + 1;
            continue;
        }
        if (self_closing) {
            out.emplace_back();
            pos = end + 1;
            continue;
        }
        const std::string close = "</" + prefix + bare + ">";
        const std::size_t stop = xml.find(close, end + 1);
        if (stop == std::string::npos) break;
        out.push_back(xml.substr(end + 1, stop - end - 1));
        pos = stop + close.size();
    }
    return out;
}

// Only an address written out (never a name, which could say anything) in this computer's or a
// private network's range: 127/8, 10/8, 172.16/12, 192.168/16, ::1, or "localhost".
bool private_host(const std::string& host) {
    if (host == "localhost" || host == "[::1]") return true;
    unsigned octet[4] = {};
    std::size_t pos = 0;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            if (pos >= host.size() || host[pos] != '.') return false;
            ++pos;
        }
        const std::size_t start = pos;
        unsigned value = 0;
        while (pos < host.size() && host[pos] >= '0' && host[pos] <= '9' && pos - start < 3)
            value = value * 10 + static_cast<unsigned>(host[pos++] - '0');
        if (pos == start || value > 255) return false;
        octet[i] = value;
    }
    if (pos != host.size()) return false;
    return octet[0] == 127 || octet[0] == 10 || (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31) ||
           (octet[0] == 192 && octet[1] == 168);
}

}  // namespace

std::vector<std::string> parse_propfind(const std::string& xml) {
    std::vector<std::string> out;
    for (const std::string& response : elements(xml, "response")) {
        const std::vector<std::string> hrefs = elements(response, "href");
        if (hrefs.empty()) continue;
        std::string href = percent_decode(xml_unescape(hrefs.front()));
        // Absolute URLs ("https://host/path/x") become paths.
        if (const std::size_t scheme = href.find("://"); scheme != std::string::npos) {
            const std::size_t slash = href.find('/', scheme + 3);
            href = slash == std::string::npos ? "/" : href.substr(slash);
        }
        const bool collection = !elements(response, "collection").empty();
        if (collection && (href.empty() || href.back() != '/')) href += '/';
        out.push_back(href);
    }
    return out;
}

WebDavBackend::WebDavBackend(const std::string& location, CloudCredentials credentials) : location_(location) {
    if (location.rfind(kPrefix, 0) != 0) throw Error("not a WebDAV location: " + location);
    base_ = location.substr(std::strlen(kPrefix));
    while (!base_.empty() && base_.back() == '/') base_.pop_back();
    const http::Url u = http::parse_url(base_ + "/");
    if (u.scheme != "https" && !private_host(u.host))
        throw Error("a WebDAV server outside your own network must use https:// (" + base_ + ")");
    base_path_ = percent_decode(u.path_and_query);
    if (base_path_.empty() || base_path_.back() != '/') base_path_ += '/';
    if (!credentials.key_id.empty() || !credentials.secret.empty()) {
        std::string pair = credentials.key_id + ":" + credentials.secret;
        auth_ = "Basic " + base64_encode(ByteSpan(reinterpret_cast<const std::uint8_t*>(pair.data()), pair.size()));
        secure_wipe(pair.data(), pair.size());
        secure_wipe(credentials.secret.data(), credentials.secret.size());
    }
}

WebDavBackend::~WebDavBackend() {
    secure_wipe(auth_.data(), auth_.size());
}

std::string WebDavBackend::location() const {
    return location_;
}

std::string WebDavBackend::url_of(const std::string& name) const {
    return base_ + "/" + cloud::url_encode(name, true);
}

http::Response WebDavBackend::request(const std::string& method, const std::string& url, ByteSpan body,
                                      std::vector<std::pair<std::string, std::string>> headers) {
    http::Request r;
    r.method = method;
    r.url = url;
    r.headers = std::move(headers);
    if (!auth_.empty()) r.headers.push_back({"Authorization", auth_});
    r.body = body;
    // Servers that are briefly busy or restarting: a few tries with a pause.
    for (int attempt = 0;; ++attempt) {
        http::Response resp;
        try {
            resp = client_.send(r);
        } catch (const http::NetworkError&) {
            if (attempt >= 3) throw;
            std::this_thread::sleep_for(std::chrono::seconds(1u << attempt));
            continue;
        }
        if ((resp.status == 429 || resp.status == 502 || resp.status == 503 || resp.status == 504) && attempt < 3) {
            std::this_thread::sleep_for(std::chrono::seconds(1u << attempt));
            continue;
        }
        if (resp.status == 401) throw Error(location_ + ": the server refused the user name or password");
        return resp;
    }
}

void WebDavBackend::fail(const std::string& what, const http::Response& r) const {
    std::string body(r.body.begin(), r.body.end());
    if (body.size() > 200) body.resize(200);
    throw Error(location_ + ": " + what + " failed: HTTP " + std::to_string(r.status) + (body.empty() ? "" : " " + body));
}

void WebDavBackend::ensure_collections(const std::string& name) {
    // The destination folder itself, and the folders above it on the server, once: a new
    // repository's folder usually does not exist yet. Folders high up may refuse MKCOL (they exist,
    // or belong to the server); only the destination's own must work.
    bool base_known = false;
    {
        std::lock_guard lock(mu_);
        base_known = collections_.contains("");
    }
    if (!base_known) {
        const http::Url u = http::parse_url(base_ + "/");
        std::string path;
        std::vector<std::string> parts;
        for (std::size_t p = 1, q; p < u.path_and_query.size(); p = q + 1) {
            q = u.path_and_query.find('/', p);
            if (q == std::string::npos) q = u.path_and_query.size();
            if (q > p) parts.push_back(u.path_and_query.substr(p, q - p));
        }
        const std::string origin = base_.substr(0, base_.size() - (u.path_and_query.size() - 1));
        for (std::size_t i = 0; i < parts.size(); ++i) {
            path += "/" + parts[i];
            const http::Response r = request("MKCOL", origin + path + "/");
            const bool last = i + 1 == parts.size();
            if (last && r.status != 201 && r.status != 405 && r.status != 301) fail("creating the folder " + path, r);
        }
        std::lock_guard lock(mu_);
        collections_.insert("");
    }
    std::string dir;
    std::size_t pos = 0;
    while ((pos = name.find('/', pos)) != std::string::npos) {
        dir = name.substr(0, pos);
        ++pos;
        {
            std::lock_guard lock(mu_);
            if (collections_.contains(dir)) continue;
        }
        const http::Response r = request("MKCOL", url_of(dir) + "/");
        // 201 made, 405 already there (some servers answer 301 or 409 for an existing one too).
        if (r.status != 201 && r.status != 405 && r.status != 301) {
            if (r.status == 409) fail("creating the folder " + dir + " (a folder above it is missing)", r);
            fail("creating the folder " + dir, r);
        }
        std::lock_guard lock(mu_);
        collections_.insert(dir);
    }
}

bool WebDavBackend::exists(const std::string& name) {
    const http::Response r = request("HEAD", url_of(name));
    if (r.status == 200) return true;
    if (r.status == 404 || r.status == 410) return false;
    fail("checking " + name, r);
}

void WebDavBackend::put(const std::string& name, ByteSpan data) {
    ensure_collections(name);
    const http::Response r = request("PUT", url_of(name), data, {{"Content-Type", "application/octet-stream"}});
    if (r.status != 200 && r.status != 201 && r.status != 204) fail("uploading " + name, r);
}

Bytes WebDavBackend::get(const std::string& name) {
    http::Response r = request("GET", url_of(name));
    if (r.status == 404 || r.status == 410) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status != 200) fail("downloading " + name, r);
    return std::move(r.body);
}

Bytes WebDavBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    http::Response r = request("GET", url_of(name), {},
                               {{"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1)}});
    if (r.status == 404 || r.status == 410) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status == 206) {
        if (r.body.size() != length) throw Error(location_ + ": repository file " + name + " is shorter than expected");
        return std::move(r.body);
    }
    if (r.status == 200) {  // the server ignored the range
        if (r.body.size() < offset + length) throw Error(location_ + ": repository file " + name + " is too short");
        return Bytes(r.body.begin() + static_cast<std::ptrdiff_t>(offset),
                     r.body.begin() + static_cast<std::ptrdiff_t>(offset + length));
    }
    fail("downloading " + name, r);
}

std::uint64_t WebDavBackend::size(const std::string& name) {
    const http::Response r = request("HEAD", url_of(name));
    if (r.status == 404 || r.status == 410) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status != 200) fail("checking " + name, r);
    const std::string length = r.header("content-length");
    if (length.empty()) throw Error(location_ + ": the server did not say the size of " + name);
    return std::stoull(length);
}

std::vector<std::string> WebDavBackend::list(const std::string& dir) {
    std::vector<std::string> out;
    static const char kBody[] =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/>"
        "</d:prop></d:propfind>";
    // Depth 1 per folder (servers often refuse "infinity"), walking down.
    const std::function<void(const std::string&)> walk = [&](const std::string& rel) {
        const std::string url = rel.empty() ? base_ + "/" : url_of(rel) + "/";
        const http::Response r = request("PROPFIND", url, ByteSpan(reinterpret_cast<const std::uint8_t*>(kBody), sizeof kBody - 1),
                                         {{"Depth", "1"}, {"Content-Type", "application/xml; charset=utf-8"}});
        if (r.status == 404 || r.status == 410) return;
        if (r.status != 207) fail("listing " + (rel.empty() ? std::string("files") : rel), r);
        const std::string self = base_path_ + (rel.empty() ? std::string() : rel + "/");
        for (const std::string& path : parse_propfind(std::string(r.body.begin(), r.body.end()))) {
            if (path == self || path.size() <= self.size() || path.compare(0, self.size(), self) != 0) continue;
            std::string child = path.substr(self.size());
            const bool folder = child.back() == '/';
            if (folder) child.pop_back();
            if (child.empty() || child.find('/') != std::string::npos) continue;
            const std::string full = rel.empty() ? child : rel + "/" + child;
            if (folder) walk(full);
            else out.push_back(full);
        }
    };
    walk(dir);
    std::sort(out.begin(), out.end());
    return out;
}

void WebDavBackend::remove(const std::string& name) {
    const http::Response r = request("DELETE", url_of(name));
    if (r.status != 200 && r.status != 204 && r.status != 404 && r.status != 410) fail("deleting " + name, r);
}

}  // namespace vaultik
