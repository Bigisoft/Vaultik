#include "vaultik/repo/pcloud_backend.hpp"

#include <algorithm>
#include <thread>

#include "vaultik/platform/http.hpp"
#include "vaultik/util/resources.hpp"

namespace vaultik {

namespace {

using nlohmann::json;

int result_of(const json& j) {
    return j.value("result", -1);
}

bool is_not_found(int code) {
    // Not found, a missing parent folder, an invalid path.
    return code == 2002 || code == 2005 || code == 2009 || code == 2010 || code == 2055;
}

std::string parent_of(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == 0 || slash == std::string::npos ? std::string("/") : path.substr(0, slash);
}

std::string leaf_of(const std::string& path) {
    return path.substr(path.rfind('/') + 1);
}

[[noreturn]] void fail(const std::string& location, const std::string& what, const json& j) {
    const int code = result_of(j);
    std::string hint;
    if (code == 1000 || code == 2094 || code == 2000) hint = " - sign in to pCloud again in Vaultik";
    if (code == 2008) hint = " - the pCloud account is full";
    throw Error(location + ": " + what + " failed: pCloud error " + std::to_string(code) + " (" +
                j.value("error", std::string("no description")) + ")" + hint);
}

}  // namespace

PCloudBackend::PCloudBackend(const std::string& location, CloudCredentials credentials)
    : location_(location), client_(std::make_unique<http::Client>()) {
    root_ = "/" + cloud_folder(location);
    const json secret = cloud::parse_secret(credentials.secret, "pCloud");
    token_ = secret.value("access_token", std::string());
    host_ = secret.value("host", std::string("api.pcloud.com"));
    if (token_.empty()) throw Error("no pCloud sign-in stored: sign in to pCloud in Vaultik first");
    // Only pCloud's own API hosts: the stored host decides where the token is sent.
    if (host_ != "api.pcloud.com" && host_ != "eapi.pcloud.com") throw Error("unexpected pCloud API host " + host_);
}

PCloudBackend::~PCloudBackend() = default;

std::string PCloudBackend::location() const {
    return location_;
}

std::string PCloudBackend::path_of(const std::string& name) const {
    return root_ + "/" + name;
}

json PCloudBackend::call(const std::string& method, std::vector<std::pair<std::string, std::string>> params,
                         ByteSpan upload) {
    params.emplace_back("access_token", token_);
    const std::string url = "https://" + host_ + "/" + method + "?" + cloud::form_encode(params);
    int wait_s = 1;
    for (int attempt = 0;; ++attempt) {
        const http::Response r = cloud::send_with_retry(
            *client_,
            [&] {
                http::Request req;
                req.method = upload.empty() ? "GET" : "PUT";
                req.url = url;
                req.body = upload;
                return req;
            },
            location_ + ": " + method);
        json j;
        try {
            j = json::parse(r.body.begin(), r.body.end());
        } catch (const json::exception&) {
            throw Error(location_ + ": " + method + ": unexpected answer (HTTP " + std::to_string(r.status) + ")");
        }
        const int code = result_of(j);
        // 4xxx: rate limited, 5xxx: try again later.
        if ((code >= 4000 && code < 6000) && attempt < 5) {
            std::this_thread::sleep_for(std::chrono::seconds(wait_s));
            wait_s *= 2;
            continue;
        }
        return j;
    }
}

void PCloudBackend::ensure_folder(const std::string& path) {
    {
        std::lock_guard lock(mu_);
        if (folders_.contains(path)) return;
    }
    // Top-down from the first component.
    std::string current;
    std::size_t pos = 1;
    while (pos <= path.size()) {
        std::size_t end = path.find('/', pos);
        if (end == std::string::npos) end = path.size();
        current += "/" + path.substr(pos, end - pos);
        pos = end + 1;
        {
            std::lock_guard lock(mu_);
            if (folders_.contains(current)) continue;
        }
        const json j = call("createfolderifnotexists", {{"path", current}});
        if (result_of(j) != 0) fail(location_, "creating folder " + current, j);
        std::lock_guard lock(mu_);
        folders_.insert(current);
    }
}

bool PCloudBackend::exists(const std::string& name) {
    const json j = call("stat", {{"path", path_of(name)}});
    const int code = result_of(j);
    if (code == 0) return !j["metadata"].value("isfolder", false);
    if (is_not_found(code)) return false;
    fail(location_, "checking " + name, j);
}

std::uint64_t PCloudBackend::size(const std::string& name) {
    const json j = call("stat", {{"path", path_of(name)}});
    const int code = result_of(j);
    if (is_not_found(code)) throw Error(location_ + ": repository file " + name + " is missing");
    if (code != 0) fail(location_, "checking " + name, j);
    return j["metadata"].value("size", std::uint64_t{0});
}

void PCloudBackend::put(const std::string& name, ByteSpan data) {
    const std::string path = path_of(name);
    const std::string dir = parent_of(path);
    for (int attempt = 0; attempt < 2; ++attempt) {
        ensure_folder(dir);
        // An existing file is replaced (pCloud keeps the old one as a revision). nopartial: an
        // interrupted upload leaves nothing behind.
        const json j = call("uploadfile", {{"path", dir}, {"filename", leaf_of(path)}, {"nopartial", "1"}}, data);
        const int code = result_of(j);
        if (code == 0) {
            std::lock_guard lock(mu_);
            links_.erase(path);
            throttle_write(data.size());
            return;
        }
        if (code == 2005 && attempt == 0) {  // the folder went away meanwhile
            std::lock_guard lock(mu_);
            folders_.clear();
            continue;
        }
        fail(location_, "uploading " + name, j);
    }
}

std::string PCloudBackend::download_url(const std::string& path, bool refresh) {
    {
        std::lock_guard lock(mu_);
        auto it = links_.find(path);
        if (!refresh && it != links_.end() && std::chrono::steady_clock::now() < it->second.until) return it->second.url;
    }
    const json j = call("getfilelink", {{"path", path}, {"forcedownload", "1"}});
    const int code = result_of(j);
    if (is_not_found(code)) return {};
    if (code != 0) fail(location_, "downloading " + path, j);
    if (!j.contains("hosts") || j["hosts"].empty()) throw Error(location_ + ": pCloud gave no download server");
    const std::string url = "https://" + j["hosts"][0].get<std::string>() + j.value("path", std::string());
    std::lock_guard lock(mu_);
    if (links_.size() > 256) links_.clear();
    // Links stay valid for hours; reuse one for a few minutes.
    links_[path] = {url, std::chrono::steady_clock::now() + std::chrono::minutes(10)};
    return url;
}

Bytes PCloudBackend::download(const std::string& name, std::uint64_t offset, std::size_t length, bool ranged) {
    const std::string path = path_of(name);
    for (int attempt = 0; attempt < 2; ++attempt) {
        const std::string url = download_url(path, attempt > 0);
        if (url.empty()) throw Error(location_ + ": repository file " + name + " is missing");
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
        if (r.status == 403 || r.status == 404 || r.status == 410) continue;  // the link expired
        if (ranged && r.status == 200) {
            // A server that ignores Range sends everything.
            if (r.body.size() < offset + length) throw Error(location_ + ": repository file " + name + " is too short");
            Bytes part(r.body.begin() + static_cast<std::ptrdiff_t>(offset),
                       r.body.begin() + static_cast<std::ptrdiff_t>(offset + length));
            throttle_read(length);
            return part;
        }
        if (r.status != (ranged ? 206 : 200))
            throw Error(location_ + ": downloading " + name + " failed: HTTP " + std::to_string(r.status));
        if (ranged && r.body.size() != length)
            throw Error(location_ + ": repository file " + name + " is shorter than expected");
        throttle_read(r.body.size());
        return std::move(r.body);
    }
    throw Error(location_ + ": downloading " + name + " failed: the download link kept expiring");
}

Bytes PCloudBackend::get(const std::string& name) {
    return download(name, 0, 0, false);
}

Bytes PCloudBackend::get_range(const std::string& name, std::uint64_t offset, std::size_t length) {
    if (length == 0) return {};
    return download(name, offset, length, true);
}

std::vector<std::string> PCloudBackend::list(const std::string& dir) {
    const std::string path = dir.empty() ? root_ : root_ + "/" + dir;
    const json j = call("listfolder", {{"path", path}, {"recursive", "1"}, {"noshares", "1"}});
    const int code = result_of(j);
    std::vector<std::string> out;
    if (is_not_found(code)) return out;
    if (code != 0) fail(location_, "listing " + (dir.empty() ? std::string("files") : dir), j);
    const std::function<void(const json&, const std::string&)> walk = [&](const json& folder, const std::string& prefix) {
        if (!folder.contains("contents")) return;
        for (const json& item : folder["contents"]) {
            const std::string name = item.value("name", std::string());
            if (name.empty()) continue;
            if (item.value("isfolder", false)) walk(item, prefix + name + "/");
            else out.push_back(prefix + name);
        }
    };
    walk(j["metadata"], dir.empty() ? std::string() : dir + "/");
    std::sort(out.begin(), out.end());
    return out;
}

void PCloudBackend::remove(const std::string& name) {
    const std::string path = path_of(name);
    const json j = call("deletefile", {{"path", path}});
    const int code = result_of(j);
    if (code != 0 && !is_not_found(code)) fail(location_, "deleting " + name, j);
    std::lock_guard lock(mu_);
    links_.erase(path);
}

cloud::SignIn PCloudBackend::sign_in(const cloud::OAuthApp& app, const std::function<bool()>& cancelled) {
    if (app.client_id.empty() || app.client_secret.empty())
        throw Error("enter the pCloud app's client ID and client secret first");
    const std::string state = cloud::random_state();
    auto params = cloud::browser_sign_in(
        kRedirectPort, "localhost",
        [&](const std::string& redirect_uri) {
            return "https://my.pcloud.com/oauth2/authorize?" +
                   cloud::form_encode({{"client_id", app.client_id},
                                       {"response_type", "code"},
                                       {"redirect_uri", redirect_uri},
                                       {"state", state}});
        },
        state, cancelled);
    // The account's region: later calls must go to this host.
    std::string host = params["hostname"];
    if (host != "eapi.pcloud.com") host = "api.pcloud.com";

    http::Client client;
    const auto get_json = [&](const std::string& url, const std::string& what) {
        const http::Response r = cloud::send_with_retry(
            client,
            [&] {
                http::Request req;
                req.method = "GET";
                req.url = url;
                return req;
            },
            what);
        json j;
        try {
            j = json::parse(r.body.begin(), r.body.end());
        } catch (const json::exception&) {
            throw Error(what + ": unexpected answer from pCloud");
        }
        if (result_of(j) != 0) throw Error(what + ": pCloud error " + std::to_string(result_of(j)) + " (" +
                                           j.value("error", std::string("no description")) + ")");
        return j;
    };
    const json token = get_json("https://" + host + "/oauth2_token?" +
                                    cloud::form_encode({{"client_id", app.client_id},
                                                        {"client_secret", app.client_secret},
                                                        {"code", params["code"]}}),
                                "signing in to pCloud");
    const std::string access = token.value("access_token", std::string());
    if (access.empty()) throw Error("signing in to pCloud: no token came back");
    const json info = get_json("https://" + host + "/userinfo?" + cloud::form_encode({{"access_token", access}}),
                               "reading the pCloud account");
    cloud::SignIn out;
    out.account = info.value("email", std::string("pCloud account"));
    out.secret = json{{"access_token", access}, {"host", host}}.dump();
    return out;
}

}  // namespace vaultik
