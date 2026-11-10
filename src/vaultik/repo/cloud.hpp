#pragma once

// Cloud storage locations and the keys that open them. A location names the service and a folder:
//   s3:https://<endpoint>/<bucket>[/<folder>]   S3-compatible storage (s3_backend.hpp)
//   pcloud:<folder path>                        pCloud, from the root of the account
//   onedrive:<folder path>                      OneDrive, inside the app's own folder (Apps\<app>)
//   gdrive:<folder path>                        Google Drive, from "My Drive"
//   mega:<folder path>                          MEGA, from "Cloud Drive"
// Every repository file is one file there, in the repository's usual layout.

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "vaultik/util/bytes.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik {

namespace http {
class Client;
struct Request;
struct Response;
}  // namespace http

enum class CloudKind { None, S3, PCloud, OneDrive, GoogleDrive, Mega, WebDav };

// The service a location names; None for a folder on this computer.
CloudKind cloud_kind(std::string_view location);
inline bool is_cloud_location(std::string_view location) {
    return cloud_kind(location) != CloudKind::None;
}
// "pCloud", "OneDrive", ...
const char* cloud_kind_name(CloudKind kind);
// The location prefix with its colon ("pcloud:").
const char* cloud_prefix(CloudKind kind);

// The folder of a pcloud:, onedrive:, gdrive: or mega: location, '/'-separated, without leading or
// trailing '/'. Throws Error when it is empty or holds "." or ".." parts.
std::string cloud_folder(const std::string& location);

// Keys for cloud storage. For S3, key_id and secret are the access key; for the other services
// key_id is the account (for display) and secret is the service's sign-in token, as JSON. The
// secret is wiped when the object goes away.
struct CloudCredentials {
    std::string key_id;
    std::string secret;
    std::string region;  // S3 only; empty: worked out from the endpoint
    // S3 only: upload repository files with Object Lock for this many days, so they cannot be
    // deleted or overwritten for good meanwhile. The bucket must have Object Lock turned on. 0: no
    // lock. Governance mode (the default) stops these keys only if they lack the
    // s3:BypassGovernanceRetention permission (and the right to change the bucket's lock
    // settings); compliance mode stops everyone, the account owner too, until the date passes.
    int lock_days = 0;
    bool lock_compliance = false;
    // Called with the new secret when a service hands out a new sign-in token (OneDrive renews its
    // refresh token on every use), so the caller can store it. May be empty.
    std::function<void(const std::string& secret)> on_secret_changed;

    CloudCredentials() = default;
    CloudCredentials(std::string id, std::string s, std::string r = {})
        : key_id(std::move(id)), secret(std::move(s)), region(std::move(r)) {}
    CloudCredentials(const CloudCredentials&) = default;
    CloudCredentials& operator=(const CloudCredentials&) = default;
    ~CloudCredentials();

    bool empty() const { return key_id.empty() && secret.empty(); }

    // AWS_ACCESS_KEY_ID, AWS_REGION (or AWS_DEFAULT_REGION), and VAULTIK_CLOUD_SECRET or else
    // AWS_SECRET_ACCESS_KEY.
    static CloudCredentials from_environment();
};

namespace cloud {

// The outcome of signing in to a drive: the account (for display, stored as the job's
// cloud_key_id) and the token to store as the secret.
struct SignIn {
    std::string account;
    std::string secret;
};

// Percent-encodes everything but A-Z a-z 0-9 - _ . ~ (and '/' when `keep_slash`).
std::string url_encode(std::string_view s, bool keep_slash = false);
// "a=1&b=2", values encoded.
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& fields);
// The query parameters of a URL or of a "a=1&b=2" string, decoded.
std::map<std::string, std::string> parse_query(std::string_view query);

// Sends the request made by `make` (called again for every attempt), retrying network failures,
// 408, 429 and 5xx answers with backoff (honouring Retry-After), up to `attempts` tries. Other
// answers are returned as they are. Throws Error naming `what` when every try failed.
http::Response send_with_retry(http::Client& client, const std::function<http::Request()>& make,
                               const std::string& what, int attempts = 6);

// The response body as text.
std::string body_text(const http::Response& response);

}  // namespace cloud

}  // namespace vaultik
