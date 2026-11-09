#pragma once

// Repository in MEGA ("mega:<folder path>", from Cloud Drive), through MEGA's own API. MEGA
// encrypts every file and name with keys only the account holds, so this backend does that work:
// files are AES-128-CTR encrypted with a per-file key and a chunked CBC-MAC, names are AES-CBC
// encrypted attributes, and node keys are wrapped with the account's master key. Vaultik's own
// encryption still applies underneath.
//
// Signing in (in the frontend) turns the email and password into a session; the stored secret is
// that session id and the master key, never the password.

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "vaultik/repo/backend.hpp"
#include "vaultik/repo/cloud.hpp"

namespace vaultik {

// MEGA asked for the six-digit code of the account's two-factor authentication.
class MegaCodeNeeded : public Error {
public:
    using Error::Error;
};

class MegaBackend final : public Backend {
public:
    MegaBackend(const std::string& location, CloudCredentials credentials);
    ~MegaBackend() override;

    std::string location() const override;
    bool exists(const std::string& name) override;
    void put(const std::string& name, ByteSpan data) override;
    Bytes get(const std::string& name) override;
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override;
    std::uint64_t size(const std::string& name) override;
    std::vector<std::string> list(const std::string& dir) override;
    void remove(const std::string& name) override;

    // Signs in with the account's email and password (and the two-factor code when the account
    // has one; MegaCodeNeeded is thrown when it is needed but empty).
    static cloud::SignIn sign_in(const std::string& email, const std::string& password, const std::string& code);

private:
    struct Node {
        std::string handle;
        std::string parent;
        std::string name;
        int type = 0;  // 0 file, 1 folder, 2 root, 3 inbox, 4 trash
        std::uint64_t size = 0;
        std::int64_t ts = 0;
        Bytes key;  // files: 32 bytes (key, nonce, meta-MAC); folders: 16
    };

    nlohmann::json api(const nlohmann::json& command);
    void load_tree();
    void catch_up();
    bool add_node(const nlohmann::json& n);
    void drop_node(const std::string& handle);
    // Handles of the children of `parent` named `name` (newest first), of files or folders.
    std::vector<std::string> children_named(const std::string& parent, const std::string& name, bool folders) const;
    std::string folder_handle(const std::string& dir, bool create);  // "" when missing and !create
    std::optional<Node> find_file(const std::string& name);
    std::string make_folder(const std::string& parent, const std::string& name);
    std::string download_url(const std::string& handle, bool refresh);
    Bytes download(const std::string& name, std::uint64_t offset, std::size_t length, bool whole);

    std::string location_;
    std::string root_folder_;
    std::string sid_;
    Bytes master_;
    std::string user_;
    std::unique_ptr<http::Client> client_;
    std::recursive_mutex mu_;  // tree and request sequence
    std::uint64_t seq_ = 0;
    bool loaded_ = false;
    std::string sn_;  // event feed position
    std::string cloud_root_;
    std::map<std::string, Node> nodes_;
    std::map<std::string, std::set<std::string>> children_;
    std::map<std::string, std::string> folder_cache_;  // dir -> handle
    struct Link {
        std::string url;
        std::chrono::steady_clock::time_point until;
    };
    std::map<std::string, Link> links_;
};

// The pieces of MEGA's protocol, exposed for tests.
namespace mega {

// Big-endian unsigned integers: base^exp mod mod (mod odd), minimal big-endian result.
Bytes modexp(ByteSpan base, ByteSpan exp, ByteSpan mod);
// Reads one MPI (2-byte big-endian bit length, then the bytes) at `pos`, advancing it.
Bytes read_mpi(ByteSpan data, std::size_t& pos);

// The v1 (pre-2017) password key and user hash.
Bytes prepare_key(std::string_view password);
std::string stringhash(std::string_view email, ByteSpan password_key);

// Node attributes: "MEGA" + JSON, zero-padded, AES-CBC with a zero IV, base64url.
std::string encrypt_attributes(const nlohmann::json& attributes, ByteSpan key);
nlohmann::json decrypt_attributes(std::string_view b64, ByteSpan key);

// Upload chunk boundaries: 128 KiB, 256 KiB, ... 1 MiB, then 1 MiB each; returns (offset, size).
std::vector<std::pair<std::uint64_t, std::size_t>> chunks(std::uint64_t size);

// AES-128-CTR in place with MEGA's counter (nonce || 64-bit big-endian block number), starting at
// byte `offset` of the file.
void ctr_crypt(ByteSpan key, ByteSpan nonce, std::uint64_t offset, std::uint8_t* data, std::size_t size);

// The condensed 8-byte meta-MAC of a whole plaintext file.
Bytes meta_mac(ByteSpan key, ByteSpan nonce, ByteSpan plaintext);

// The 32-byte node key of a file from its AES key, nonce and meta-MAC, and back.
Bytes pack_file_key(ByteSpan key, ByteSpan nonce, ByteSpan meta_mac);
Bytes file_aes_key(ByteSpan node_key);  // 16 bytes
Bytes file_nonce(ByteSpan node_key);    // 8 bytes

}  // namespace mega

}  // namespace vaultik
