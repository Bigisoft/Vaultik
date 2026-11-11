#include "vaultik/repo/s3_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/crypto/md5.hpp"
#include "vaultik/util/base64.hpp"
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

std::string iso_time(std::int64_t unix_seconds) {
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::vector<std::pair<std::string, std::string>> lock_headers(ByteSpan body, std::int64_t until, bool compliance) {
    const auto digest = md5(body);
    return {{"content-md5", base64_encode(ByteSpan(digest.data(), digest.size()))},
            {"x-amz-object-lock-mode", compliance ? "COMPLIANCE" : "GOVERNANCE"},
            {"x-amz-object-lock-retain-until-date", iso_time(until)}};
}

std::vector<std::pair<std::string, std::string>> put_headers(const std::string& name, ByteSpan body,
                                                             const CloudCredentials& credentials, bool data_class,
                                                             std::int64_t now) {
    std::vector<std::pair<std::string, std::string>> headers;
    // Everything but the lock files (which runs remove when they end) is locked, when asked.
    if (credentials.lock_days > 0 && !name.starts_with("locks/"))
        headers = lock_headers(body, now + static_cast<std::int64_t>(credentials.lock_days) * 86400,
                               credentials.lock_compliance);
    if (data_class && !credentials.storage_class.empty())
        headers.emplace_back("x-amz-storage-class", credentials.storage_class);
    return headers;
}

std::string restore_request(int days, const std::string& tier) {
    if (days < 1 || days > 365) throw Error("a thaw lasts from 1 to 365 days");
    std::string t = lower(tier);
    if (t == "standard") t = "Standard";
    else if (t == "bulk") t = "Bulk";
    else if (t == "expedited") t = "Expedited";
    else throw Error("unknown thaw tier " + tier + " (use Standard, Bulk or Expedited)");
    return "<RestoreRequest xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Days>" + std::to_string(days) +
           "</Days><GlacierJobParameters><Tier>" + t + "</Tier></GlacierJobParameters></RestoreRequest>";
}

RestoreHeader parse_restore_header(std::string_view value) {
    RestoreHeader h;
    const auto field = [value](std::string_view key, std::string& out) {
        const std::string open = std::string(key) + "=\"";
        const std::size_t a = value.find(open);
        if (a == std::string_view::npos) return false;
        const std::size_t start = a + open.size();
        const std::size_t b = value.find('"', start);
        if (b == std::string_view::npos) return false;
        out = std::string(value.substr(start, b - start));
        return true;
    };
    std::string ongoing;
    if (!field("ongoing-request", ongoing)) return h;
    h.present = true;
    h.ongoing = ongoing == "true";
    field("expiry-date", h.expiry);
    return h;
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

VersionPage parse_versions(std::string_view xml) {
    VersionPage page;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t v = xml.find("<Version>", pos);
        const std::size_t d = xml.find("<DeleteMarker>", pos);
        if (v == std::string_view::npos && d == std::string_view::npos) break;
        const bool marker = d < v;
        const std::size_t a = marker ? d : v;
        const std::size_t b = xml.find(marker ? "</DeleteMarker>" : "</Version>", a);
        if (b == std::string_view::npos) break;
        const std::string_view item = xml.substr(a, b - a);
        ObjectVersion o;
        o.key = xml_value(item, "Key");
        o.version_id = xml_value(item, "VersionId");
        o.last_modified = xml_value(item, "LastModified");
        o.latest = xml_value(item, "IsLatest") == "true";
        o.delete_marker = marker;
        if (!o.key.empty()) page.versions.push_back(std::move(o));
        pos = b;
    }
    page.truncated = xml_value(xml, "IsTruncated") == "true";
    page.next_key_marker = xml_value(xml, "NextKeyMarker");
    page.next_version_marker = xml_value(xml, "NextVersionIdMarker");
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
    if (code == "InvalidObjectState") {
        throw ArchivedError(location + ": " + what + " failed: the file is in archive storage and must be thawed "
                            "before it can be read - run \"vaultik cloud-thaw\" for the version to restore, then "
                            "try again once the thaw is done (this takes hours)");
    }
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
    put_object(name, data, false);
}

void S3Backend::put_data(const std::string& name, ByteSpan data) {
    put_object(name, data, true);
}

bool S3Backend::data_needs_thaw() const {
    return storage_class_needs_thaw(credentials_.storage_class);
}

void S3Backend::put_object(const std::string& name, ByteSpan data, bool data_class) {
    const bool lock = credentials_.lock_days > 0 && !name.starts_with("locks/");
    const std::int64_t now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const Result r = request("PUT", key_of(name), {}, s3::put_headers(name, data, credentials_, data_class, now), data);
    if (r.status == 400 && data_class && !credentials_.storage_class.empty()) {
        const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
        if (s3::xml_value(text, "Code") == "InvalidStorageClass")
            throw Error(location_ + ": the cloud storage does not offer the storage class " + credentials_.storage_class +
                        " - choose another one (or Standard) for this job");
    }
    if (r.status == 400 && lock) {
        const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
        throw Error(location_ + ": uploading " + name + " with a lock failed: " + s3::xml_value(text, "Message") +
                    " - the bucket needs Object Lock turned on (it can only be chosen when the bucket is created), "
                    "or turn off locking for this job");
    }
    if (r.status != 200) refuse(location_, "uploading " + name, r.response);
    throttle_write(data.size());
}

S3Backend::ArchiveState S3Backend::archive_state(const std::string& name) {
    const Result r = request("HEAD", key_of(name), {}, {}, {});
    if (r.status == 404) throw Error(location_ + ": repository file " + name + " is missing");
    if (r.status != 200) refuse(location_, "checking " + name, r.response);
    ArchiveState s;
    s.storage_class = r.response.header("x-amz-storage-class");
    s.archived = storage_class_needs_thaw(s.storage_class);
    const s3::RestoreHeader h = s3::parse_restore_header(r.response.header("x-amz-restore"));
    s.thawing = h.present && h.ongoing;
    s.thawed = h.present && !h.ongoing;
    s.expiry = h.expiry;
    return s;
}

S3Backend::ThawOutcome S3Backend::thaw(const std::string& name, int days, const std::string& tier) {
    const std::string body = s3::restore_request(days, tier);
    const ArchiveState state = archive_state(name);
    if (!state.archived || state.thawed) return ThawOutcome::Available;
    if (state.thawing) return ThawOutcome::InProgress;
    const ByteSpan span(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
    const auto digest = md5(span);
    const Result r = request("POST", key_of(name), "restore=",
                             {{"content-md5", base64_encode(ByteSpan(digest.data(), digest.size()))}}, span);
    if (r.status == 202) return ThawOutcome::Requested;
    if (r.status == 200) return ThawOutcome::Available;  // a thawed copy was there already
    const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
    const std::string code = s3::xml_value(text, "Code");
    if (r.status == 409 || code == "RestoreAlreadyInProgress") return ThawOutcome::InProgress;
    if (code == "InvalidObjectState") return ThawOutcome::Available;  // not in an archive class
    refuse(location_, "thawing " + name, r.response);
}

bool S3Backend::object_lock_enabled() {
    const Result r = request("GET", {}, "object-lock=", {}, {});
    if (r.status != 200) {  // ObjectLockConfigurationNotFoundError: the bucket has none
        const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
        if (s3::xml_value(text, "Code").find("ObjectLockConfigurationNotFound") != std::string::npos) return false;
        refuse(location_, "checking Object Lock", r.response);
    }
    const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
    return s3::xml_value(text, "ObjectLockEnabled") == "Enabled";
}

S3Backend::RollbackResult S3Backend::roll_back(std::int64_t time, bool dry_run,
                                               const std::function<void(const std::string&)>& on_file) {
    // Every version of every repository file, by key, newest first (the server's order).
    const std::string root = where_.folder.empty() ? std::string() : where_.folder + "/";
    std::vector<s3::ObjectVersion> all;
    std::string key_marker, version_marker;
    for (;;) {
        // Query parameters in sorted order, as the signature needs them.
        std::string query;
        if (!key_marker.empty()) query += "key-marker=" + s3::uri_encode(key_marker, false) + "&";
        query += "prefix=" + s3::uri_encode(root, false);
        if (!version_marker.empty()) query += "&version-id-marker=" + s3::uri_encode(version_marker, false);
        query += "&versions=";
        const Result r = request("GET", {}, query, {}, {});
        if (r.status != 200) refuse(location_, "listing old versions", r.response);
        const std::string_view text(reinterpret_cast<const char*>(r.response.body.data()), r.response.body.size());
        s3::VersionPage page = s3::parse_versions(text);
        for (auto& v : page.versions) all.push_back(std::move(v));
        if (!page.truncated) break;
        if (page.next_key_marker.empty() || (page.next_key_marker == key_marker && page.next_version_marker == version_marker))
            throw Error(location_ + ": the cloud storage sent an incomplete list of old versions");
        key_marker = page.next_key_marker;
        version_marker = page.next_version_marker;
    }

    const std::string at = s3::iso_time(time).substr(0, 19);  // compared with LastModified's first 19
    const auto later = [&at](const s3::ObjectVersion& v) { return v.last_modified.substr(0, 19) > at; };
    RollbackResult result;
    for (std::size_t i = 0; i < all.size();) {
        std::size_t end = i;
        while (end < all.size() && all[end].key == all[i].key) ++end;
        const std::string key = all[i].key;
        const std::string name = key.substr(std::min(root.size(), key.size()));
        const std::size_t first = i;
        i = end;
        if (name.empty() || name.starts_with("locks/") || key.back() == '/') continue;  // never bring back old locks
        ++result.files_checked;

        // The version that was current at `time`, and everything after it.
        std::size_t then = first;
        while (then < end && later(all[then])) ++then;
        if (then == end || all[then].delete_marker) {
            ++result.newer;  // did not exist then
            continue;
        }
        if (then == first) {
            ++result.unchanged;
            continue;
        }
        if (on_file) on_file(name);
        if (dry_run) {
            ++result.restored;
            continue;
        }
        // Remove the later delete markers and versions, newest first; a locked later version
        // cannot go, so then the old version is copied over it.
        bool done = true;
        for (std::size_t k = first; k < then; ++k) {
            const Result r = request("DELETE", key, "versionId=" + s3::uri_encode(all[k].version_id, false), {}, {});
            if (r.status == 204 || r.status == 200) continue;
            done = false;
            const std::string source = "/" + s3::uri_encode(where_.bucket, false) + "/" + s3::uri_encode(key, true) +
                                       "?versionId=" + s3::uri_encode(all[then].version_id, false);
            const Result c = request("PUT", key, {}, {{"x-amz-copy-source", source}}, {});
            // A copy can fail after S3 has answered 200: then the body holds an <Error> instead
            // of a <CopyObjectResult>.
            const std::string_view body(reinterpret_cast<const char*>(c.response.body.data()), c.response.body.size());
            if (c.status == 200 && body.find("<CopyObjectResult") != std::string_view::npos) {
                done = true;
            } else {
                const std::string code = s3::xml_value(body, "Code");
                result.failed.push_back(name + ": the cloud storage refused (" + std::to_string(c.status) +
                                        (code.empty() ? "" : " " + code) + ")");
            }
            break;
        }
        if (done) ++result.restored;
    }
    return result;
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
