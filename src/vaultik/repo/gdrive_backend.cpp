#include "vaultik/repo/gdrive_backend.hpp"

#include <algorithm>
#include <thread>

#include "vaultik/platform/http.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik {

namespace {

using nlohmann::json;

constexpr const char* kFiles = "https://www.googleapis.com/drive/v3/files";
constexpr const char* kUpload = "https://www.googleapis.com/upload/drive/v3/files";
constexpr const char* kTokenUrl = "https://oauth2.googleapis.com/token";
constexpr const char* kFolderType = "application/vnd.google-apps.folder";

std::string quote(const std::string& s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\\' || c == '\'') out.push_back('\\');
        out.push_back(c);
    }
    return out + "'";
}

std::string api_error(const http::Response& r) {
    try {
        const json j = json::parse(r.body.begin(), r.body.end());
        if (j.contains("error")) return j["error"].value("message", std::string("error"));
    } catch (const json::exception&) {
    }
    return "HTTP " + std::to_string(r.status);
}

[[noreturn]] void fail(const std::string& location, const std::string& what, const http::Response& r) {
    std::string hint;
    if (r.status == 401) hint = " - sign in to Google Drive again in Vaultik";
    const std::string text = api_error(r);
    if (text.find("storageQuotaExceeded") != std::string::npos || text.find("quota") != std::string::npos)
        hint = " - the Google Drive may be full";
    throw Error(location + ": " + what + " failed: " + text + hint);
}

ByteSpan span_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string parent_dir(const std::string& name) {
    const std::size_t slash = name.rfind('/');
    return slash == std::string::npos ? std::string() : name.substr(0, slash);
}

std::string leaf(const std::string& name) {
    return name.substr(name.rfind('/') + 1);
}

}  // namespace

GoogleDriveBackend::GoogleDriveBackend(const std::string& location, CloudCredentials credentials)
    : location_(location), root_(cloud_folder(location)), credentials_(std::move(credentials)),
      client_(std::make_unique<http::Client>()) {
    cloud::parse_secret(credentials_.secret, "Google Drive");
    tokens_ = std::make_unique<cloud::OAuthTokens>(*client_, credentials_, kTokenUrl);
}

GoogleDriveBackend::~GoogleDriveBackend() = default;

std::string GoogleDriveBackend::location() const {
    return location_;
}

http::Response GoogleDriveBackend::api(const std::string& method, const std::string& url, ByteSpan body,
                                       std::vector<std::pair<std::string, std::string>> headers) {
    int wait_s = 1;
    for (int attempt = 0;; ++attempt) {
        const std::string token = tokens_->bearer();
        http::Response r = cloud::send_with_retry(
            *client_,
            [&] {
                http::Request req;
                req.method = method;
                req.url = url;
                req.headers = headers;
                req.headers.emplace_back("Authorization", "Bearer " + token);
                req.body = body;
                return req;
            },
            location_ + ": " + method);
        if (r.status == 401 && attempt == 0) {
            tokens_->invalidate(token);
            continue;
        }
        // Drive reports its rate limits as 403 with a reason.
        if (r.status == 403 && attempt < 6) {
            const std::string text = cloud::body_text(r);
            if (text.find("rateLimitExceeded") != std::string::npos ||
                text.find("userRateLimitExceeded") != std::string::npos) {
                std::this_thread::sleep_for(std::chrono::seconds(wait_s));
                wait_s *= 2;
                continue;
            }
        }
        return r;
    }
}

std::vector<json> GoogleDriveBackend::children_named(const std::string& parent, const std::string& name, bool folders) {
    std::string q = "name = " + quote(name) + " and " + quote(parent) + " in parents and trashed = false";
    q += folders ? std::string(" and mimeType = '") + kFolderType + "'"
                 : std::string(" and mimeType != '") + kFolderType + "'";
    const std::string url = std::string(kFiles) + "?" +
                            cloud::form_encode({{"q", q},
                                                {"fields", "files(id,size,modifiedTime)"},
                                                {"orderBy", "modifiedTime desc"},
                                                {"spaces", "drive"},
                                                {"pageSize", "100"}});
    const http::Response r = api("GET", url);
    if (r.status != 200) fail(location_, "looking up " + name, r);
    const json j = json::parse(r.body.begin(), r.body.end());
    std::vector<json> out;
    for (const json& f : j.value("files", json::array())) out.push_back(f);
    return out;
}

std::string GoogleDriveBackend::folder_id(const std::string& dir, bool create) {
    {
        std::lock_guard lock(mu_);
        auto it = folders_.find(dir);
        if (it != folders_.end()) return it->second;
    }
    // The backup folder itself, from My Drive ("root"), or a folder inside it.
    std::string parent;
    std::string name;
    if (dir.empty()) {
        std::string current = "root";
        std::size_t pos = 0;
        while (pos <= root_.size()) {
            std::size_t end = root_.find('/', pos);
            if (end == std::string::npos) end = root_.size();
            const std::string part = root_.substr(pos, end - pos);
            pos = end + 1;
            const bool last = pos > root_.size();
            if (last) {
                parent = current;
                name = part;
                break;
            }
            const auto found = children_named(current, part, true);
            if (!found.empty()) {
                current = found.front().value("id", std::string());
                continue;
            }
            if (!create) return {};
            const std::string meta = json{{"name", part}, {"mimeType", kFolderType}, {"parents", {current}}}.dump();
            const http::Response r = api("POST", std::string(kFiles) + "?fields=id", span_of(meta),
                                         {{"Content-Type", "application/json; charset=UTF-8"}});
            if (r.status != 200) fail(location_, "creating folder " + part, r);
            current = json::parse(r.body.begin(), r.body.end()).value("id", std::string());
        }
    } else {
        parent = folder_id(parent_dir(dir), create);
        if (parent.empty()) return {};
        name = leaf(dir);
    }
    std::string id;
    // With several folders of this name (two runs created it at once), every run uses the one
    // with the smallest id, so they all end up writing to the same folder.
    const auto first_of = [](const std::vector<json>& folders) {
        std::string best;
        for (const json& f : folders) {
            const std::string fid = f.value("id", std::string());
            if (!fid.empty() && (best.empty() || fid < best)) best = fid;
        }
        return best;
    };
    const auto found = children_named(parent, name, true);
    if (!found.empty()) {
        id = first_of(found);
    } else {
        if (!create) return {};
        const std::string meta = json{{"name", name}, {"mimeType", kFolderType}, {"parents", {parent}}}.dump();
        const http::Response r = api("POST", std::string(kFiles) + "?fields=id", span_of(meta),
                                     {{"Content-Type", "application/json; charset=UTF-8"}});
        if (r.status != 200) fail(location_, "creating folder " + name, r);
        id = json::parse(r.body.begin(), r.body.end()).value("id", std::string());
        // Another run may have created one too meanwhile: settle on the same one as it will.
        if (const std::string settled = first_of(children_named(parent, name, true)); !settled.empty()) id = settled;
    }
    if (id.empty()) throw Error(location_ + ": Google Drive gave no folder id");
    std::lock_guard lock(mu_);
    folders_[dir] = id;
    return id;
}

std::optional<GoogleDriveBackend::File> GoogleDriveBackend::lookup(const std::string& name,
                                                                   std::vector<std::string>* duplicates) {
    if (duplicates == nullptr) {
        std::lock_guard lock(mu_);
        auto it = files_.find(name);
        if (it != files_.end()) return it->second;
    }
    const std::string parent = folder_id(parent_dir(name), false);
    if (parent.empty()) return std::nullopt;
    auto found = children_named(parent, leaf(name), false);
    if (found.empty() && !parent_dir(name).empty()) {
        // Files a run put into a duplicate of the folder (made before runs settled on one).
        const std::string dir = parent_dir(name);
        const std::string grandparent = folder_id(parent_dir(dir), false);
        if (!grandparent.empty()) {
            for (const json& f : children_named(grandparent, leaf(dir), true)) {
                const std::string other = f.value("id", std::string());
                if (other == parent || other.empty()) continue;
                found = children_named(other, leaf(name), false);
                if (!found.empty()) break;
            }
        }
    }
    if (found.empty()) return std::nullopt;
    File f;
    f.id = found.front().value("id", std::string());
    f.size = std::strtoull(found.front().value("size", std::string("0")).c_str(), nullptr, 10);
    if (duplicates != nullptr)
        for (std::size_t i = 1; i < found.size(); ++i) duplicates->push_back(found[i].value("id", std::string()));
    std::lock_guard lock(mu_);
    files_[name] = f;
    return f;
}

bool GoogleDriveBackend::exists(const std::string& name) {
    return lookup(name).has_value();
}

std::uint64_t GoogleDriveBackend::size(const std::string& name) {
    const auto f = lookup(name);
    if (!f) throw Error(location_ + ": repository file " + name + " is missing");
    return f->size;
}

void GoogleDriveBackend::put(const std::string& name, ByteSpan data) {
    const std::string parent = folder_id(parent_dir(name), true);
    std::vector<std::string> duplicates;
    const auto existing = lookup(name, &duplicates);

    // A resumable upload, sent in one piece: an update keeps the file's id, so readers never see
    // it missing.
    std::string start_url;
    std::string method;
    std::string meta;
    if (existing) {
        start_url = std::string(kUpload) + "/" + existing->id + "?uploadType=resumable&fields=id,size";
        method = "PATCH";
        meta = "{}";
    } else {
        start_url = std::string(kUpload) + "?uploadType=resumable&fields=id,size";
        method = "POST";
        meta = json{{"name", leaf(name)}, {"parents", {parent}}}.dump();
    }
    const http::Response s = api(method, start_url, span_of(meta),
                                 {{"Content-Type", "application/json; charset=UTF-8"},
                                  {"X-Upload-Content-Type", "application/octet-stream"},
                                  {"X-Upload-Content-Length", std::to_string(data.size())}});
    if (s.status != 200) fail(location_, "starting the upload of " + name, s);
    const std::string session = s.header("location");
    if (session.rfind("https://www.googleapis.com/", 0) != 0)
        throw Error(location_ + ": Google Drive gave no upload address");
    const http::Response r = api("PUT", session, data, {{"Content-Type", "application/octet-stream"}});
    if (r.status != 200 && r.status != 201) fail(location_, "uploading " + name, r);
    const json j = json::parse(r.body.begin(), r.body.end());
    {
        std::lock_guard lock(mu_);
        files_[name] = File{j.value("id", std::string()), data.size()};
    }
    // Copies made by two runs at once: keep the one just written.
    for (const std::string& id : duplicates) api("DELETE", std::string(kFiles) + "/" + id);
    throttle_write(data.size());
}

Bytes GoogleDriveBackend::download(const std::string& name, std::uint64_t offset, std::size_t length, bool ranged) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto f = lookup(name);
        if (!f) throw Error(location_ + ": repository file " + name + " is missing");
        std::vector<std::pair<std::string, std::string>> headers;
        if (ranged) headers.emplace_back("Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1));
        http::Response r = api("GET", std::string(kFiles) + "/" + f->id + "?alt=media", {}, headers);
        if (r.status == 404) {  // replaced or removed by another run: look it up again
            std::lock_guard lock(mu_);
            files_.erase(name);
            continue;
        }
        if (ranged && r.status == 200 && r.body.size() >= offset + length) {
            Bytes part(r.body.begin() + static_cast<std::ptrdiff_t>(offset),
                       r.body.begin() + static_cast<std::ptrdiff_t>(offset + length));
            throttle_read(length);
            return part;
        }
        if (r.status != (ranged ? 206 : 200)) fail(location_, "downloading " + name, r);
        if (ranged && r.body.size() != length)
            throw Error(location_ + ": repository file " + name + " is shorter than expected");
        throttle_read(r.body.size());
        return std::move(r.body);
    }
    throw Error(location_ + ": repository file " + name + " is missing");
}

Bytes GoogleDriveBackend::get(const std::string& name) {
    return download(name, 0, 0, false);
}

Bytes GoogleDriveBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    return download(name, offset, length, true);
}

std::vector<std::string> GoogleDriveBackend::list(const std::string& dir) {
    std::vector<std::string> out;
    const std::string start = folder_id(dir, false);
    if (start.empty()) return out;
    std::vector<std::pair<std::string, std::string>> pending = {{dir, start}};
    std::map<std::string, File> seen;
    while (!pending.empty()) {
        const auto [path, id] = pending.back();
        pending.pop_back();
        std::string token;
        do {
            std::vector<std::pair<std::string, std::string>> q = {
                {"q", quote(id) + " in parents and trashed = false"},
                {"fields", "nextPageToken,files(id,name,mimeType,size)"},
                {"spaces", "drive"},
                {"pageSize", "1000"}};
            if (!token.empty()) q.emplace_back("pageToken", token);
            const http::Response r = api("GET", std::string(kFiles) + "?" + cloud::form_encode(q));
            if (r.status != 200) fail(location_, "listing " + (path.empty() ? std::string("files") : path), r);
            const json j = json::parse(r.body.begin(), r.body.end());
            for (const json& f : j.value("files", json::array())) {
                const std::string name = f.value("name", std::string());
                if (name.empty()) continue;
                const std::string full = path.empty() ? name : path + "/" + name;
                if (f.value("mimeType", std::string()) == kFolderType) {
                    pending.emplace_back(full, f.value("id", std::string()));
                    std::lock_guard lock(mu_);
                    folders_[full] = f.value("id", std::string());
                } else if (!seen.contains(full)) {
                    seen[full] = File{f.value("id", std::string()),
                                      std::strtoull(f.value("size", std::string("0")).c_str(), nullptr, 10)};
                    out.push_back(full);
                }
            }
            token = j.value("nextPageToken", std::string());
        } while (!token.empty());
    }
    {
        std::lock_guard lock(mu_);
        for (auto& [name, f] : seen) files_[name] = f;
    }
    std::sort(out.begin(), out.end());
    return out;
}

void GoogleDriveBackend::remove(const std::string& name) {
    std::vector<std::string> duplicates;
    const auto f = lookup(name, &duplicates);
    {
        std::lock_guard lock(mu_);
        files_.erase(name);
    }
    if (!f) return;
    duplicates.push_back(f->id);
    for (const std::string& id : duplicates) {
        // A real delete, not the trash: pruned data should stop counting against the quota.
        const http::Response r = api("DELETE", std::string(kFiles) + "/" + id);
        if (r.status != 204 && r.status != 200 && r.status != 404) fail(location_, "deleting " + name, r);
    }
}

cloud::SignIn GoogleDriveBackend::sign_in(const cloud::OAuthApp& app, const std::function<bool()>& cancelled) {
    if (app.client_id.empty() || app.client_secret.empty())
        throw Error("enter the Google app's client ID and client secret first");
    const std::string scope = "https://www.googleapis.com/auth/drive.file openid email";
    const cloud::Pkce pkce = cloud::make_pkce();
    const std::string state = cloud::random_state();
    auto params = cloud::browser_sign_in(
        0, "127.0.0.1",
        [&](const std::string& redirect_uri) {
            return "https://accounts.google.com/o/oauth2/v2/auth?" +
                   cloud::form_encode({{"client_id", app.client_id},
                                       {"redirect_uri", redirect_uri},
                                       {"response_type", "code"},
                                       {"scope", scope},
                                       {"state", state},
                                       {"code_challenge", pkce.challenge},
                                       {"code_challenge_method", "S256"},
                                       {"access_type", "offline"},
                                       {"prompt", "consent"}});
        },
        state, cancelled);

    http::Client client;
    const std::string body = cloud::form_encode({{"client_id", app.client_id},
                                                 {"client_secret", app.client_secret},
                                                 {"code", params["code"]},
                                                 {"code_verifier", pkce.verifier},
                                                 {"grant_type", "authorization_code"},
                                                 {"redirect_uri", params["redirect_uri"]}});
    const http::Response r = cloud::send_with_retry(
        client,
        [&] {
            http::Request req;
            req.method = "POST";
            req.url = kTokenUrl;
            req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
            req.body = span_of(body);
            return req;
        },
        "signing in to Google Drive");
    const json token = cloud::token_response(r, "signing in to Google Drive");
    const std::string refresh = token.value("refresh_token", std::string());
    if (refresh.empty()) throw Error("signing in to Google Drive: no refresh token came back");
    const json claims = cloud::jwt_claims(token.value("id_token", std::string()));
    cloud::SignIn out;
    out.account = claims.value("email", std::string("Google account"));
    out.secret =
        json{{"client_id", app.client_id}, {"client_secret", app.client_secret}, {"refresh_token", refresh}}.dump();
    return out;
}

}  // namespace vaultik
