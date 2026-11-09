#include "vaultik/repo/cloud.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/http.hpp"
#include "vaultik/platform/os.hpp"

namespace vaultik {

namespace {

struct KindPrefix {
    CloudKind kind;
    const char* prefix;
    const char* name;
};

constexpr KindPrefix kKinds[] = {
    {CloudKind::S3, "s3:", "S3"},
    {CloudKind::PCloud, "pcloud:", "pCloud"},
    {CloudKind::OneDrive, "onedrive:", "OneDrive"},
    {CloudKind::GoogleDrive, "gdrive:", "Google Drive"},
    {CloudKind::Mega, "mega:", "MEGA"},
};

bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != prefix[i]) return false;
    }
    return true;
}

}  // namespace

CloudKind cloud_kind(std::string_view location) {
    for (const KindPrefix& k : kKinds)
        if (starts_with_nocase(location, k.prefix)) return k.kind;
    return CloudKind::None;
}

const char* cloud_kind_name(CloudKind kind) {
    for (const KindPrefix& k : kKinds)
        if (k.kind == kind) return k.name;
    return "folder";
}

const char* cloud_prefix(CloudKind kind) {
    for (const KindPrefix& k : kKinds)
        if (k.kind == kind) return k.prefix;
    return "";
}

std::string cloud_folder(const std::string& location) {
    const CloudKind kind = cloud_kind(location);
    if (kind == CloudKind::None || kind == CloudKind::S3) throw Error("not a cloud drive location: " + location);
    std::string folder = location.substr(std::string_view(cloud_prefix(kind)).size());
    std::replace(folder.begin(), folder.end(), '\\', '/');
    std::string out;
    std::size_t pos = 0;
    while (pos <= folder.size()) {
        std::size_t end = folder.find('/', pos);
        if (end == std::string::npos) end = folder.size();
        const std::string part = folder.substr(pos, end - pos);
        if (part == "." || part == "..") throw Error("a cloud folder cannot contain \"" + part + "\": " + location);
        if (!part.empty()) out += (out.empty() ? "" : "/") + part;
        pos = end + 1;
    }
    if (out.empty()) {
        throw Error(std::string("name a folder for the backup, e.g. ") + cloud_prefix(kind) + "Backups/my-pc (got " +
                    location + ")");
    }
    return out;
}

CloudCredentials::~CloudCredentials() {
    secure_wipe(secret.data(), secret.size());
}

CloudCredentials CloudCredentials::from_environment() {
    CloudCredentials c;
    c.key_id = os::get_env("AWS_ACCESS_KEY_ID");
    c.secret = os::get_env("VAULTIK_CLOUD_SECRET");
    if (c.secret.empty()) c.secret = os::get_env("AWS_SECRET_ACCESS_KEY");
    c.region = os::get_env("AWS_REGION");
    if (c.region.empty()) c.region = os::get_env("AWS_DEFAULT_REGION");
    c.lock_days = std::atoi(os::get_env("VAULTIK_S3_LOCK_DAYS").c_str());
    return c;
}

namespace cloud {

std::string url_encode(std::string_view s, bool keep_slash) {
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

std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out;
    for (const auto& [k, v] : fields) out += (out.empty() ? "" : "&") + url_encode(k) + "=" + url_encode(v);
    return out;
}

std::map<std::string, std::string> parse_query(std::string_view query) {
    const std::size_t q = query.find('?');
    if (q != std::string_view::npos) query = query.substr(q + 1);
    const std::size_t hash = query.find('#');
    if (hash != std::string_view::npos) query = query.substr(0, hash);
    const auto decode = [](std::string_view s) {
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '+') {
                out.push_back(' ');
            } else if (s[i] == '%' && i + 2 < s.size()) {
                out.push_back(static_cast<char>(std::strtol(std::string(s.substr(i + 1, 2)).c_str(), nullptr, 16)));
                i += 2;
            } else {
                out.push_back(s[i]);
            }
        }
        return out;
    };
    std::map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < query.size()) {
        std::size_t end = query.find('&', pos);
        if (end == std::string_view::npos) end = query.size();
        const std::string_view part = query.substr(pos, end - pos);
        const std::size_t eq = part.find('=');
        if (eq == std::string_view::npos) out[decode(part)] = "";
        else out[decode(part.substr(0, eq))] = decode(part.substr(eq + 1));
        pos = end + 1;
    }
    return out;
}

http::Response send_with_retry(http::Client& client, const std::function<http::Request()>& make,
                               const std::string& what, int attempts) {
    std::string last;
    int wait_s = 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (attempt > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(wait_s));
            wait_s = std::min(wait_s * 2, 60);
        }
        http::Response r;
        try {
            r = client.send(make());
        } catch (const http::NetworkError& e) {
            last = e.what();
            continue;
        }
        if (r.status == 408 || r.status == 429 || r.status >= 500) {
            last = "the server answered " + std::to_string(r.status);
            const std::string retry_after = r.header("retry-after");
            if (!retry_after.empty()) {
                const long s = std::strtol(retry_after.c_str(), nullptr, 10);
                if (s > 0) wait_s = static_cast<int>(std::min<long>(s, 300));
            }
            continue;
        }
        return r;
    }
    throw Error(what + ": " + last + " (gave up after " + std::to_string(attempts) + " tries)");
}

std::string body_text(const http::Response& response) {
    return std::string(response.body.begin(), response.body.end());
}

}  // namespace cloud

}  // namespace vaultik
