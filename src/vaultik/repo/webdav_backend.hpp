#pragma once

// Repository on a WebDAV server ("webdav:https://host/path"): Nextcloud, ownCloud, Synology and
// QNAP NAS boxes, Apache or nginx with WebDAV, and many hosting providers. The user name and
// password are the credentials' key_id and secret, sent with HTTP Basic authentication, so the
// URL must use https (http only to this computer or a private network address).

#include <mutex>
#include <set>
#include <string>

#include "vaultik/platform/http.hpp"
#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"

namespace vaultik {

class WebDavBackend final : public Backend {
public:
    WebDavBackend(const std::string& location, CloudCredentials credentials);
    ~WebDavBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

private:
    std::string url_of(const std::string& name) const;  // base + encoded name
    http::Response request(const std::string& method, const std::string& url, ByteSpan body = {},
                           std::vector<std::pair<std::string, std::string>> headers = {});
    void ensure_collections(const std::string& name);  // MKCOL for each missing parent folder
    [[noreturn]] void fail(const std::string& what, const http::Response& r) const;

    std::string location_;
    std::string base_;       // "https://host/path", no trailing slash
    std::string base_path_;  // "/path" as the server writes it in PROPFIND answers (decoded)
    std::string auth_;       // "Basic ..."
    http::Client client_;
    std::mutex mu_;
    std::set<std::string> collections_;  // known to exist
};

// Splits a PROPFIND (Depth: 1) answer into the decoded paths it lists, with a trailing '/' for
// collections. Exposed for tests.
std::vector<std::string> parse_propfind(const std::string& xml);

}  // namespace vaultik
