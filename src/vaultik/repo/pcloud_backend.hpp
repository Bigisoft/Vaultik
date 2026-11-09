#pragma once

// Repository in pCloud ("pcloud:<folder path>"), through pCloud's HTTP JSON API with an OAuth token.
// The token does not expire; it is kept with the API host of the account's region (US or EU).

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/repo/oauth.hpp"

namespace vaultik {

class PCloudBackend final : public Backend {
public:
    PCloudBackend(const std::string& location, CloudCredentials credentials);
    ~PCloudBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

    // Signs in through the browser with the app registered in pCloud's developer console, whose
    // redirect URI must be http://localhost:53682/.
    static cloud::SignIn sign_in(const cloud::OAuthApp& app, const std::function<bool()>& cancelled);
    static constexpr int kRedirectPort = 53682;

private:
    // Calls an API method; retries pCloud's "try again later" results (4xxx, 5xxx). Returns the
    // answer whatever its result code.
    nlohmann::json call(const std::string& method, std::vector<std::pair<std::string, std::string>> params,
                        ByteSpan upload = {});
    std::string path_of(const std::string& name) const;
    void ensure_folder(const std::string& path);
    std::string download_url(const std::string& path, bool refresh);
    Bytes download(const std::string& name, std::uint64_t offset, std::size_t length, bool ranged);

    std::string location_;
    std::string root_;  // "/Backups/my-pc"
    std::string host_;
    std::string token_;
    std::unique_ptr<http::Client> client_;
    std::mutex mu_;
    std::set<std::string> folders_;  // known to exist
    struct Link {
        std::string url;
        std::chrono::steady_clock::time_point until;
    };
    std::map<std::string, Link> links_;
};

}  // namespace vaultik
