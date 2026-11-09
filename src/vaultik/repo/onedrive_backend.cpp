#include "vaultik/repo/onedrive_backend.hpp"

#include <algorithm>

#include "vaultik/platform/http.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik {

namespace {

using nlohmann::json;

constexpr const char* kGraph = "https://graph.microsoft.com/v1.0/me/drive/special/approot";
constexpr const char* kScope = "Files.ReadWrite.AppFolder offline_access openid profile";
constexpr std::size_t kSimpleUploadLimit = 4u << 20;  // Graph's limit for a single PUT
constexpr std::size_t kChunk = 320u * 1024u * 32u;     // 10 MiB, a multiple of 320 KiB as Graph needs

std::string token_url(const std::string& tenant) {
    return "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/token";
}

std::string graph_error(const http::Response& r) {
    try {
        const json j = json::parse(r.body.begin(), r.body.end());
        if (j.contains("error")) {
            const json& e = j["error"];
            return e.value("code", std::string()) + ": " + e.value("message", std::string());
        }
    } catch (const json::exception&) {
    }
    return "HTTP " + std::to_string(r.status);
}

[[noreturn]] void fail(const std::string& location, const std::string& what, const http::Response& r) {
    std::string hint;
    if (r.status == 401 || r.status == 403) hint = " - sign in to OneDrive again in Vaultik";
    if (r.status == 507) hint = " - the OneDrive is full";
    throw Error(location + ": " + what + " failed: " + graph_error(r) + hint);
}

ByteSpan span_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

}  // namespace

OneDriveBackend::OneDriveBackend(const std::string& location, CloudCredentials credentials)
    : location_(location), root_(cloud_folder(location)), credentials_(std::move(credentials)),
      client_(std::make_unique<http::Client>()) {
    const json secret = cloud::parse_secret(credentials_.secret, "OneDrive");
    const std::string tenant = secret.value("tenant", std::string("common"));
    if (tenant.find_first_of("/?#") != std::string::npos) throw Error("bad OneDrive tenant in the stored sign-in");
    tokens_ = std::make_unique<cloud::OAuthTokens>(*client_, credentials_, token_url(tenant),
                                                   std::vector<std::pair<std::string, std::string>>{{"scope", kScope}});
}

OneDriveBackend::~OneDriveBackend() = default;

std::string OneDriveBackend::location() const {
    return location_;
}

std::string OneDriveBackend::item_url(const std::string& path) const {
    return std::string(kGraph) + ":/" + cloud::url_encode(path, true) + ":";
}

http::Response OneDriveBackend::graph(const std::string& method, const std::string& url, ByteSpan body,
                                      std::vector<std::pair<std::string, std::string>> headers) {
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
        return r;
    }
}

OneDriveBackend::Item OneDriveBackend::stat(const std::string& name) {
    const http::Response r = graph("GET", item_url(root_ + "/" + name) + "?$select=size,folder,file");
    Item item;
    if (r.status == 404) return item;
    if (r.status != 200) fail(location_, "checking " + name, r);
    const json j = json::parse(r.body.begin(), r.body.end());
    item.found = true;
    item.folder = j.contains("folder");
    item.size = j.value("size", std::uint64_t{0});
    return item;
}

bool OneDriveBackend::exists(const std::string& name) {
    const Item item = stat(name);
    return item.found && !item.folder;
}

std::uint64_t OneDriveBackend::size(const std::string& name) {
    const Item item = stat(name);
    if (!item.found || item.folder) throw Error(location_ + ": repository file " + name + " is missing");
    return item.size;
}

void OneDriveBackend::put(const std::string& name, ByteSpan data) {
    {
        std::lock_guard lock(mu_);
        links_.erase(name);
    }
    if (data.size() > kSimpleUploadLimit) {
        upload_session(name, data);
    } else {
        // Missing folders on the way are created; an existing file is replaced.
        const http::Response r = graph("PUT", item_url(root_ + "/" + name) + "/content", data,
                                       {{"Content-Type", "application/octet-stream"}});
        if (r.status != 200 && r.status != 201) fail(location_, "uploading " + name, r);
    }
    throttle_write(data.size());
}

void OneDriveBackend::upload_session(const std::string& name, ByteSpan data) {
    const std::string request = R"({"item":{"@microsoft.graph.conflictBehavior":"replace"}})";
    for (int attempt = 0; attempt < 3; ++attempt) {
        const http::Response s = graph("POST", item_url(root_ + "/" + name) + "/createUploadSession", span_of(request),
                                       {{"Content-Type", "application/json"}});
        if (s.status != 200) fail(location_, "starting the upload of " + name, s);
        const std::string upload_url = json::parse(s.body.begin(), s.body.end()).value("uploadUrl", std::string());
        if (upload_url.rfind("https://", 0) != 0) throw Error(location_ + ": OneDrive gave no upload address");

        bool restart = false;
        for (std::size_t off = 0; off < data.size() && !restart; off += kChunk) {
            const std::size_t n = std::min(kChunk, data.size() - off);
            // The upload address is pre-authorised: no Authorization header.
            const http::Response r = cloud::send_with_retry(
                *client_,
                [&] {
                    http::Request req;
                    req.method = "PUT";
                    req.url = upload_url;
                    req.headers = {{"Content-Range", "bytes " + std::to_string(off) + "-" + std::to_string(off + n - 1) +
                                                         "/" + std::to_string(data.size())}};
                    req.body = data.subspan(off, n);
                    return req;
                },
                location_ + ": uploading " + name);
            if (r.status == 202) continue;
            if (r.status == 200 || r.status == 201) return;
            if (r.status == 404 || r.status == 416) {  // the session expired or lost a part: start over
                restart = true;
                break;
            }
            fail(location_, "uploading " + name, r);
        }
        if (!restart) throw Error(location_ + ": uploading " + name + " did not finish");
    }
    throw Error(location_ + ": uploading " + name + " failed: OneDrive kept dropping the upload");
}

Bytes OneDriveBackend::download(const std::string& name, std::uint64_t offset, std::size_t length, bool ranged) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string url;
        {
            std::lock_guard lock(mu_);
            auto it = links_.find(name);
            if (attempt == 0 && it != links_.end() && std::chrono::steady_clock::now() < it->second.until)
                url = it->second.url;
        }
        if (url.empty()) {
            // /content answers with a redirect to a short-lived, pre-authorised download address.
            const http::Response r = graph("GET", item_url(root_ + "/" + name) + "/content");
            if (r.status == 404) throw Error(location_ + ": repository file " + name + " is missing");
            if (r.status == 200 && !ranged) {
                throttle_read(r.body.size());
                return r.body;
            }
            if (r.status != 302 && r.status != 301 && r.status != 307) fail(location_, "downloading " + name, r);
            url = r.header("location");
            if (url.rfind("https://", 0) != 0) throw Error(location_ + ": OneDrive gave no download address");
            std::lock_guard lock(mu_);
            if (links_.size() > 256) links_.clear();
            links_[name] = {url, std::chrono::steady_clock::now() + std::chrono::minutes(5)};
        }
        http::Response r = cloud::send_with_retry(
            *client_,
            [&] {
                http::Request req;
                req.method = "GET";
                req.url = url;
                if (ranged)
                    req.headers = {{"Range", "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + length - 1)}};
                return req;
            },
            location_ + ": downloading " + name);
        if (r.status == 401 || r.status == 403 || r.status == 404 || r.status == 410) {
            std::lock_guard lock(mu_);
            links_.erase(name);
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
    throw Error(location_ + ": downloading " + name + " failed: the download address kept expiring");
}

Bytes OneDriveBackend::get(const std::string& name) {
    return download(name, 0, 0, false);
}

Bytes OneDriveBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    return download(name, offset, length, true);
}

std::vector<std::string> OneDriveBackend::list(const std::string& dir) {
    std::vector<std::string> out;
    std::vector<std::string> pending = {dir};
    while (!pending.empty()) {
        const std::string current = pending.back();
        pending.pop_back();
        const std::string path = current.empty() ? root_ : root_ + "/" + current;
        std::string url = item_url(path) + "/children?$select=name,size,folder&$top=1000";
        while (!url.empty()) {
            const http::Response r = graph("GET", url);
            if (r.status == 404) break;
            if (r.status != 200) fail(location_, "listing " + (current.empty() ? std::string("files") : current), r);
            const json j = json::parse(r.body.begin(), r.body.end());
            for (const json& item : j.value("value", json::array())) {
                const std::string name = item.value("name", std::string());
                if (name.empty()) continue;
                const std::string full = current.empty() ? name : current + "/" + name;
                if (item.contains("folder")) pending.push_back(full);
                else out.push_back(full);
            }
            url = j.value("@odata.nextLink", std::string());
            if (!url.empty() && url.rfind("https://graph.microsoft.com/", 0) != 0)
                throw Error(location_ + ": OneDrive sent an unexpected listing address");
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void OneDriveBackend::remove(const std::string& name) {
    {
        std::lock_guard lock(mu_);
        links_.erase(name);
    }
    const http::Response r = graph("DELETE", item_url(root_ + "/" + name));
    if (r.status != 204 && r.status != 200 && r.status != 404) fail(location_, "deleting " + name, r);
}

cloud::SignIn OneDriveBackend::sign_in(const cloud::OAuthApp& app, const std::function<bool()>& cancelled) {
    if (app.client_id.empty()) throw Error("enter the OneDrive app's client ID (application ID) first");
    const std::string tenant = "common";
    const cloud::Pkce pkce = cloud::make_pkce();
    const std::string state = cloud::random_state();
    auto params = cloud::browser_sign_in(
        0, "localhost",
        [&](const std::string& redirect_uri) {
            return "https://login.microsoftonline.com/" + tenant + "/oauth2/v2.0/authorize?" +
                   cloud::form_encode({{"client_id", app.client_id},
                                       {"response_type", "code"},
                                       {"redirect_uri", redirect_uri},
                                       {"response_mode", "query"},
                                       {"scope", kScope},
                                       {"state", state},
                                       {"code_challenge", pkce.challenge},
                                       {"code_challenge_method", "S256"},
                                       {"prompt", "select_account"}});
        },
        state, cancelled);

    http::Client client;
    const std::string body = cloud::form_encode({{"client_id", app.client_id},
                                                 {"grant_type", "authorization_code"},
                                                 {"code", params["code"]},
                                                 {"redirect_uri", params["redirect_uri"]},
                                                 {"code_verifier", pkce.verifier},
                                                 {"scope", kScope}});
    const http::Response r = cloud::send_with_retry(
        client,
        [&] {
            http::Request req;
            req.method = "POST";
            req.url = token_url(tenant);
            req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
            req.body = span_of(body);
            return req;
        },
        "signing in to OneDrive");
    const json token = cloud::token_response(r, "signing in to OneDrive");
    const std::string refresh = token.value("refresh_token", std::string());
    if (refresh.empty()) throw Error("signing in to OneDrive: no refresh token came back (is offline_access allowed?)");
    const json claims = cloud::jwt_claims(token.value("id_token", std::string()));
    cloud::SignIn out;
    out.account = claims.value("preferred_username", claims.value("email", claims.value("name", std::string("OneDrive account"))));
    out.secret = json{{"client_id", app.client_id}, {"tenant", tenant}, {"refresh_token", refresh}}.dump();
    return out;
}

}  // namespace vaultik
