#include "vaultik/jobs/update_check.hpp"

#include <cstdlib>

#include <nlohmann/json.hpp>

#include "vaultik/platform/http.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::jobs {

namespace {

constexpr char kLatest[] = "https://api.github.com/repos/Bigisoft/Vaultik/releases/latest";
constexpr char kReleasePages[] = "https://github.com/Bigisoft/Vaultik/releases/";

}  // namespace

std::array<int, 3> version_numbers(const std::string& version) {
    std::array<int, 3> out{};
    const char* p = version.c_str();
    if (*p == 'v' || *p == 'V') ++p;
    for (int& part : out) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 100000) break;
        part = static_cast<int>(v);
        if (*end != '.') break;
        p = end + 1;
    }
    return out;
}

bool newer_version(const std::string& candidate, const std::string& current) {
    return version_numbers(candidate) > version_numbers(current);
}

std::optional<Release> parse_release(const std::string& json) {
    try {
        const nlohmann::json j = nlohmann::json::parse(json);
        if (!j.is_object() || j.value("draft", false) || j.value("prerelease", false)) return std::nullopt;
        Release r;
        r.version = j.value("tag_name", std::string());
        if (!r.version.empty() && (r.version[0] == 'v' || r.version[0] == 'V')) r.version.erase(0, 1);
        r.url = j.value("html_url", std::string());
        r.name = j.value("name", std::string());
        // Only a version number, and only a page of Vaultik's own releases: the link is opened in
        // the browser.
        if (r.version.empty() || r.version.size() > 20 ||
            r.version.find_first_not_of("0123456789.") != std::string::npos || version_numbers(r.version) == std::array<int, 3>{})
            return std::nullopt;
        if (!r.url.starts_with(kReleasePages) || r.url.size() > 200 ||
            r.url.find_first_of(" \"'<>\\\r\n\t") != std::string::npos)
            return std::nullopt;
        if (r.name.size() > 200) r.name.resize(200);
        for (char& c : r.name)
            if (static_cast<unsigned char>(c) < 0x20) c = ' ';
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<Release> newer_release(const std::string& current) {
    http::Client client;
    http::Request req;
    req.method = "GET";
    req.url = kLatest;
    // GitHub's API needs a User-Agent; it names the program only.
    req.headers = {{"User-Agent", "Vaultik/" + current}, {"Accept", "application/vnd.github+json"}};
    const http::Response res = client.send(req);
    if (res.status == 404) return std::nullopt;  // no release published yet
    if (res.status != 200)
        throw Error("GitHub answered the update check with HTTP " + std::to_string(res.status));
    std::optional<Release> r = parse_release(std::string(res.body.begin(), res.body.end()));
    if (!r || !newer_version(r->version, current)) return std::nullopt;
    return r;
}

}  // namespace vaultik::jobs
