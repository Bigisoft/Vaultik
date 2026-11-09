#include "vaultik/repo/s3_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/http.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik {

namespace {

constexpr int kAttempts = 6;  // first try plus five retries, waiting 1, 2, 4, 8, 16 s

ByteSpan span_of(std::string_view s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string hex_of(const Key32& k) {
    return to_hex(ByteSpan(k.data(), k.size()));
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string amz_now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

}  // namespace

// ---- credentials and locations ---------------------------------------------------------------

S3Location parse_s3_location(const std::string& location) {
    if (cloud_kind(location) != CloudKind::S3) throw Error("not an S3 storage location: " + location);
    const std::string url = location.substr(3);
    const http::Url u = http::parse_url(url);
    S3Location l;
    l.endpoint = u.scheme + "://" + u.host_header();
    l.host = u.host_header();
    std::string path = u.path_and_query;
    if (path.find_first_of("?#") != std::string::npos)
        throw Error("a cloud storage location has no ? or # part: " + location);
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    while (!path.empty() && path.back() == '/') path.pop_back();
    const std::size_t slash = path.find('/');
    l.bucket = path.substr(0, slash);
    if (slash != std::string::npos) l.folder = path.substr(slash + 1);
    if (l.bucket.empty()) {
        throw Error("the cloud storage location needs a bucket: s3:https://<endpoint>/<bucket>[/<folder>] (got " +
                    location + ")");
    }
    if (l.folder.find("//") != std::string::npos) throw Error("empty folder name in " + location);
    return l;
}

std::string default_region(const std::string& host_header) {
    std::string host = lower(host_header);
    if (!host.starts_with("[")) host = host.substr(0, host.find(':'));  // drop the port
    if (ends_with(host, ".r2.cloudflarestorage.com")) return "auto";
    // s3.<region>.backblazeb2.com, s3.<region>.amazonaws.com, s3.<region>.wasabisys.com, ...
    if (host.starts_with("s3.")) {
        const std::size_t dot = host.find('.', 3);
        if (dot != std::string::npos) {
            const std::string region = host.substr(3, dot - 3);
            if (region != "amazonaws" && region.find('-') != std::string::npos) return region;
        }
    }
    return "us-east-1";
}

// ---- the protocol ----------------------------------------------------------------------------

namespace s3 {

std::string uri_encode(std::string_view s, bool keep_slash) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || (keep_slash && c == '/')) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

std::string authorization(const SignedRequest& r, const CloudCredentials& credentials, const std::string& region) {
    std::vector<std::pair<std::string, std::string>> headers = r.headers;
    std::sort(headers.begin(), headers.end());
    std::string canonical_headers;
    std::string signed_headers;
    for (const auto& [k, v] : headers) {
        // Trimmed values; S3 headers here never contain runs of spaces.
        std::string value = v;
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        while (!value.empty() && value.back() == ' ') value.pop_back();
        canonical_headers += k + ":" + value + "\n";
        signed_headers += (signed_headers.empty() ? "" : ";") + k;
    }
    const std::string canonical = r.method + "\n" + r.canonical_uri + "\n" + r.canonical_query + "\n" +
                                  canonical_headers + "\n" + signed_headers + "\n" + r.payload_hash;
    const std::string date = r.amz_date.substr(0, 8);
    const std::string scope = date + "/" + region + "/s3/aws4_request";
    const std::string to_sign =
        "AWS4-HMAC-SHA256\n" + r.amz_date + "\n" + scope + "\n" + hex_of(sha256(span_of(canonical)));

    std::string k_secret = "AWS4" + credentials.secret;
    Key32 k = hmac_sha256(span_of(k_secret), span_of(date));
    secure_wipe(k_secret.data(), k_secret.size());
    k = hmac_sha256(k, span_of(region));
    k = hmac_sha256(k, span_of("s3"));
    k = hmac_sha256(k, span_of("aws4_request"));
    const Key32 signature = hmac_sha256(k, span_of(to_sign));
    secure_wipe(k.data(), k.size());
    return "AWS4-HMAC-SHA256 Credential=" + credentials.key_id + "/" + scope + ", SignedHeaders=" + signed_headers +
           ", Signature=" + hex_of(signature);
}

std::string xml_value(std::string_view xml, std::string_view tag) {
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    const std::size_t a = xml.find(open);
    if (a == std::string_view::npos) return {};
    const std::size_t start = a + open.size();
    const std::size_t b = xml.find(close, start);
    if (b == std::string_view::npos) return {};
    const std::string_view raw = xml.substr(start, b - start);
    std::string out;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] != '&') {
            out.push_back(raw[i]);
            continue;
        }
        const std::size_t semi = raw.find(';', i);
        if (semi == std::string_view::npos) {
            out.push_back('&');
            continue;
        }
        const std::string_view ent = raw.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent.size() > 1 && ent[0] == '#') {
            const bool hex = ent[1] == 'x' || ent[1] == 'X';
            const unsigned long cp = std::strtoul(std::string(ent.substr(hex ? 2 : 1)).c_str(), nullptr, hex ? 16 : 10);
            if (cp < 0x80) out.push_back(static_cast<char>(cp));
            else out += "?";  // object names in a repository are ASCII
        } else {
            out.append(raw.substr(i, semi - i + 1));
        }
        i = semi;
    }
    return out;
}

ListPage parse_list(std::string_view xml) {
    ListPage page;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t a = xml.find("<Contents>", pos);
        if (a == std::string_view::npos) break;
        const std::size_t b = xml.find("</Contents>", a);
        if (b == std::string_view::npos) break;
        const std::string_view item = xml.substr(a, b - a);
        const std::string key = xml_value(item, "Key");
        const std::string size = xml_value(item, "Size");
        if (!key.empty()) page.objects.emplace_back(key, std::strtoull(size.c_str(), nullptr, 10));
        pos = b;
    }
    // IsTruncated and NextContinuationToken come after the objects.
    const std::string_view tail = xml.substr(pos);
    page.truncated = xml_value(tail, "IsTruncated") == "true";
    page.next_token = xml_value(tail, "NextContinuationToken");
    return page;
}

}  // namespace s3

// ---- the backend -----------------------------------------------------------------------------

struct S3Backend::Result {
    int status = 0;
    http::Response response;
};

S3Backend::S3Backend(const std::string& location, CloudCredentials credentials)
    : location_(location), where_(parse_s3_location(location)), credentials_(std::move(credentials)),
      client_(std::make_unique<http::Client>()) {
    if (credentials_.key_id.empty() || credentials_.secret.empty()) {
        throw Error("cloud storage needs an access key: give the key ID and secret (on the command line, set "
                    "AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY)");
    }
    if (credentials_.region.empty()) credentials_.region = default_region(where_.host);
}

S3Backend::~S3Backend() = default;

std::string S3Backend::location() const {
    return location_;
}

std::string S3Backend::key_of(const std::string& name) const {
    return where_.folder.empty() ? name : where_.folder + "/" + name;
}

S3Backend::Result S3Backend::request(const std::string& method, const std::string& key, const std::string& query,
                                     std::vector<std::pair<std::string, std::string>> extra_headers, ByteSpan body) {
    const std::string path = "/" + s3::uri_encode(where_.bucket, false) + (key.empty() ? "" : "/" + s3::uri_encode(key, true));
    const std::string payload_hash = hex_of(sha256(body));
    std::string last_error;
    for (int attempt = 0; attempt < kAttempts; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(1 << (attempt - 1)));
        s3::SignedRequest sr;
        sr.method = method;
        sr.canonical_uri = path;
        sr.canonical_query = query;
        sr.payload_hash = payload_hash;
        sr.amz_date = amz_now();
        sr.headers = {{"host", where_.host}, {"x-amz-content-sha256", payload_hash}, {"x-amz-date", sr.amz_date}};
        for (const auto& h : extra_headers) sr.headers.push_back(h);

        http::Request req;
        req.method = method;
        req.url = where_.endpoint + path + (query.empty() ? "" : "?" + query);
        req.headers = {{"x-amz-content-sha256", payload_hash},
                       {"x-amz-date", sr.amz_date},
                       {"Authorization", s3::authorization(sr, credentials_, credentials_.region)}};
        for (const auto& h : extra_headers) req.headers.push_back(h);
        req.body = body;

        Result r;
        try {
            r.response = client_->send(req);
        } catch (const http::NetworkError& e) {
            last_error = e.what();
            continue;
        }
        r.status = r.response.status;
        if (r.status == 429 || r.status == 408 || r.status >= 500) {
            const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
            const std::string code = s3::xml_value(text, "Code");
            last_error = "the cloud storage answered " + std::to_string(r.status) + (code.empty() ? "" : " " + code);
            continue;
        }
        return r;
    }
    throw Error(location_ + ": " + last_error + " (gave up after " + std::to_string(kAttempts) + " tries)");
}

namespace {

// An Error for an unexpected answer, with S3's own code and message when it sent them.
[[noreturn]] void refuse(const std::string& location, const std::string& what, const http::Response& resp) {
    const std::string_view text(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    const std::string code = s3::xml_value(text, "Code");
    const std::string message = s3::xml_value(text, "Message");
    std::string hint;
    if (resp.status == 403 || code == "SignatureDoesNotMatch" || code == "InvalidAccessKeyId")
        hint = " - check the access key ID and secret, and that the key may use this bucket";
    else if (code == "NoSuchBucket") hint = " - create the bucket first";
    else if (code == "RequestTimeTooSkewed") hint = " - this computer's clock is wrong";
    throw Error(location + ": " + what + " failed: " + std::to_string(resp.status) + (code.empty() ? "" : " " + code) +
                (message.empty() ? "" : " (" + message + ")") + hint);
}

}  // namespace

bool S3Backend::exists(const std::string& name) {
    const Result r = request("HEAD", key_of(name), {}, {}, {});
    if (r.status == 200) return true;
    if (r.status == 404) return false;
    refuse(location_, "checking " + name, r.response);
}

void S3Backend::put(const std::string& name, ByteSpan data) {
    const Result r = request("PUT", key_of(name), {}, {}, data);
    if (r.status != 200) refuse(location_, "uploading " + name, r.response);
    throttle_write(data.size());
}

Bytes S3Backend::get(const std::string& name) {
    Result r = request("GET", key_of(name), {}, {}, {});
    if (r.status == 404) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status != 200) refuse(location_, "downloading " + name, r.response);
    throttle_read(r.response.body.size());
    return std::move(r.response.body);
}

Bytes S3Backend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    const std::string range = "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1);
    Result r = request("GET", key_of(name), {}, {{"range", range}}, {});
    if (r.status == 404) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status == 200 && offset == 0 && r.response.body.size() >= length) {
        r.response.body.resize(length);  // a server that ignores Range sends everything
    } else if (r.status != 206) {
        refuse(location_, "downloading part of " + name, r.response);
    }
    if (r.response.body.size() != length)
        throw Error(location_ + ": repository file " + name + " is shorter than expected");
    throttle_read(length);
    return std::move(r.response.body);
}

std::uint64_t S3Backend::size(const std::string& name) {
    const Result r = request("HEAD", key_of(name), {}, {}, {});
    if (r.status == 404) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status != 200) refuse(location_, "checking " + name, r.response);
    const std::string len = r.response.header("content-length");
    if (len.empty()) throw Error(location_ + ": no size for " + name);
    return std::strtoull(len.c_str(), nullptr, 10);
}

std::vector<std::string> S3Backend::list(const std::string& dir) {
    const std::string root = where_.folder.empty() ? std::string() : where_.folder + "/";
    const std::string prefix = root + (dir.empty() ? std::string() : dir + "/");
    std::vector<std::string> out;
    std::string token;
    for (;;) {
        // Query parameters in sorted order, as the signature needs them.
        std::string query;
        if (!token.empty()) query += "continuation-token=" + s3::uri_encode(token, false) + "&";
        query += "list-type=2&prefix=" + s3::uri_encode(prefix, false);
        const Result r = request("GET", {}, query, {}, {});
        if (r.status != 200) refuse(location_, "listing " + (dir.empty() ? std::string("files") : dir), r.response);
        const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
        s3::ListPage page = s3::parse_list(text);
        for (auto& [key, size] : page.objects) {
            if (key.compare(0, root.size(), root) != 0 || key.size() == root.size()) continue;
            if (key.back() == '/') continue;  // a "folder" placeholder some tools create
            out.push_back(key.substr(root.size()));
        }
        if (!page.truncated) break;
        if (page.next_token.empty() || page.next_token == token)
            throw Error(location_ + ": the cloud storage sent an incomplete listing");
        token = page.next_token;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void S3Backend::remove(const std::string& name) {
    const Result r = request("DELETE", key_of(name), {}, {}, {});
    if (r.status != 204 && r.status != 200 && r.status != 404) refuse(location_, "deleting " + name, r.response);
}

}  // namespace vaultik
