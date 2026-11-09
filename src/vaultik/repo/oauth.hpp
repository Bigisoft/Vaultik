#pragma once

// OAuth 2.0 pieces shared by the cloud drives (pCloud, OneDrive, Google Drive): the browser sign-in
// with PKCE and a localhost redirect, and access tokens renewed from a stored refresh token.

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "vaultik/repo/cloud.hpp"

namespace vaultik::cloud {

// The app registration a service gave the user: client ID, and a client secret where the
// service wants one (Google's "desktop app" secret is not really secret; pCloud's is).
struct OAuthApp {
    std::string client_id;
    std::string client_secret;
};

struct Pkce {
    std::string verifier;   // 43 random URL-safe characters
    std::string challenge;  // base64url(SHA-256(verifier)), method S256
};
Pkce make_pkce();
std::string random_state();

// Opens the sign-in page `authorize_url(redirect_uri)` in the browser and waits (up to 10 minutes,
// or until `cancelled`) for the redirect to http://<host>:<port>/. Checks the state parameter and
// the error parameter, and returns the redirect's parameters (code, ...) plus "redirect_uri".
std::map<std::string, std::string> browser_sign_in(int port, const std::string& host,
                                                   const std::function<std::string(const std::string&)>& authorize_url,
                                                   const std::string& state, const std::function<bool()>& cancelled);

// The claims of a JWT (the id_token from a token endpoint), without checking its signature: it
// came straight from the service over TLS and is only used to show the account name.
nlohmann::json jwt_claims(const std::string& token);

// Parses a token endpoint's JSON answer and throws Error with its error description on failure.
nlohmann::json token_response(const http::Response& response, const std::string& what);

// Access tokens for a drive, renewed from the refresh token in `credentials.secret` (JSON with
// client_id, optional client_secret and refresh_token). A refresh token the service rotates is
// written back into the secret and passed to `on_secret_changed`. Thread-safe.
class OAuthTokens {
public:
    OAuthTokens(http::Client& client, CloudCredentials& credentials, std::string token_url,
                std::vector<std::pair<std::string, std::string>> extra_fields = {});

    // A valid access token ("Bearer " not included).
    std::string bearer();
    // Forgets the current access token (after a 401), so the next bearer() renews it.
    void invalidate(const std::string& token);

private:
    http::Client& client_;
    CloudCredentials& credentials_;
    std::string token_url_;
    std::vector<std::pair<std::string, std::string>> extra_fields_;
    std::mutex mu_;
    std::string access_;
    std::chrono::steady_clock::time_point expires_{};
};

// Reads a JSON secret, throwing a readable Error when it is not one (for example an S3 key typed in
// for a drive).
nlohmann::json parse_secret(const std::string& secret, const char* service);

}  // namespace vaultik::cloud
