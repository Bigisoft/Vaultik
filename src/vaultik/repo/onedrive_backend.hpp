#pragma once

// Repository in OneDrive ("onedrive:<folder path>"), through Microsoft Graph, inside the app's own
// folder (OneDrive\Apps\<app name>). Vaultik asks only for Files.ReadWrite.AppFolder, so it cannot
// see or change anything else in the OneDrive. Microsoft renews the refresh token on every use;
// the new one is passed to CloudCredentials::on_secret_changed.

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/repo/oauth.hpp"

namespace vaultik {

class OneDriveBackend final : public Backend {
public:
    OneDriveBackend(const std::string& location, CloudCredentials credentials);
    ~OneDriveBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

    // Signs in through the browser with an app registered in Microsoft Entra (a public client
    // with the redirect URI http://localhost). Works with personal and work or school accounts.
    static cloud::SignIn sign_in(const cloud::OAuthApp& app, const std::function<bool()>& cancelled);

private:
    struct Item {
        bool found = false;
        bool folder = false;
        std::uint64_t size = 0;
    };
    // Sends a Graph request with the access token, renewing it once on 401. `url` is complete.
    http::Response graph(const std::string& method, const std::string& url, ByteSpan body = {},
                         std::vector<std::pair<std::string, std::string>> headers = {});
    std::string item_url(const std::string& path) const;  // ".../approot:/<path>:"
    Item stat(const std::string& name);
    void upload_session(const std::string& name, ByteSpan data);
    Bytes download(const std::string& name, std::uint64_t offset, std::size_t length, bool ranged);

    std::string location_;
    std::string root_;  // folder inside the app folder
    CloudCredentials credentials_;
    std::unique_ptr<http::Client> client_;
    std::unique_ptr<cloud::OAuthTokens> tokens_;
    std::mutex mu_;
    struct Link {
        std::string url;
        std::chrono::steady_clock::time_point until;
    };
    std::map<std::string, Link> links_;
};

}  // namespace vaultik
