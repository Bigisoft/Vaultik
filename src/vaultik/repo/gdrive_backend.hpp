#pragma once

// Repository in Google Drive ("gdrive:<folder path>", from My Drive), through the Drive v3 API.
// Vaultik asks only for the drive.file scope: it sees just the files and folders it created, so the
// backup folders are made by Vaultik itself (a folder of the same name made by hand is not used).
// Drive addresses files by id, so paths are resolved through the folders and cached.

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/repo/oauth.hpp"

namespace vaultik {

class GoogleDriveBackend final : public Backend {
public:
    GoogleDriveBackend(const std::string& location, CloudCredentials credentials);
    ~GoogleDriveBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    bool exists_now(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

    // Signs in through the browser with an OAuth client of type "Desktop app" from the Google
    // Cloud console (client ID and client secret).
    static cloud::SignIn sign_in(const cloud::OAuthApp& app, const std::function<bool()>& cancelled);

private:
    struct File {
        std::string id;
        std::uint64_t size = 0;
    };
    http::Response api(const std::string& method, const std::string& url, ByteSpan body = {},
                       std::vector<std::pair<std::string, std::string>> headers = {});
    // The files (or folders) named `name` in folder `parent`, newest first.
    std::vector<nlohmann::json> children_named(const std::string& parent, const std::string& name, bool folders);
    // The id of a folder below the backup folder ("" is the backup folder itself); empty when it
    // does not exist and `create` is false.
    std::string folder_id(const std::string& dir, bool create);
    std::optional<File> lookup(const std::string& name, std::vector<std::string>* duplicates = nullptr);
    Bytes download(const std::string& name, std::uint64_t offset, std::size_t length, bool ranged);

    std::string location_;
    std::string root_;
    CloudCredentials credentials_;
    std::unique_ptr<http::Client> client_;
    std::unique_ptr<cloud::OAuthTokens> tokens_;
    std::mutex mu_;
    std::map<std::string, std::string> folders_;  // dir -> id
    std::map<std::string, File> files_;           // name -> file
};

}  // namespace vaultik
