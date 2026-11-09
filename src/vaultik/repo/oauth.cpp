#include "vaultik/repo/oauth.hpp"

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/http.hpp"
#include "vaultik/platform/loopback.hpp"
#include "vaultik/util/base64.hpp"

namespace vaultik::cloud {

Pkce make_pkce() {
    std::uint8_t rnd[32];
    random_bytes(rnd);
    Pkce p;
    p.verifier = base64url_encode(ByteSpan(rnd, sizeof rnd));
    const Key32 h = sha256(ByteSpan(reinterpret_cast<const std::uint8_t*>(p.verifier.data()), p.verifier.size()));
    p.challenge = base64url_encode(ByteSpan(h.data(), h.size()));
    return p;
}

std::string random_state() {
    std::uint8_t rnd[16];
    random_bytes(rnd);
    return base64url_encode(ByteSpan(rnd, sizeof rnd));
}

std::map<std::string, std::string> browser_sign_in(int port, const std::string& host,
                                                   const std::function<std::string(const std::string&)>& authorize_url,
                                                   const std::string& state, const std::function<bool()>& cancelled) {
    os::LoopbackServer server(port);
    const std::string redirect_uri = "http://" + host + ":" + std::to_string(server.port()) + "/";
    os::open_in_browser(authorize_url(redirect_uri));
    auto params = server.wait(std::chrono::minutes(10), cancelled,
                              "You are signed in. You can close this tab and go back to Vaultik.");
    if (params.contains("error")) {
        const std::string desc = params["error_description"];
        throw Error("signing in failed: " + params["error"] + (desc.empty() ? "" : " (" + desc + ")"));
    }
    if (params["state"] != state) throw Error("signing in failed: the answer did not come from this sign-in");
    if (params["code"].empty()) throw Error("signing in failed: no sign-in code came back");
    params["redirect_uri"] = redirect_uri;
    return params;
}

nlohmann::json jwt_claims(const std::string& token) {
    const std::size_t a = token.find('.');
    const std::size_t b = a == std::string::npos ? std::string::npos : token.find('.', a + 1);
    if (b == std::string::npos) return nlohmann::json::object();
    try {
        const Bytes payload = base64_decode(std::string_view(token).substr(a + 1, b - a - 1));
        return nlohmann::json::parse(payload.begin(), payload.end());
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

nlohmann::json token_response(const http::Response& response, const std::string& what) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(response.body.begin(), response.body.end());
    } catch (const nlohmann::json::exception&) {
        throw Error(what + ": unexpected answer (HTTP " + std::to_string(response.status) + ")");
    }
    if (response.status != 200 || j.contains("error")) {
        std::string err = j.value("error", std::string("HTTP ") + std::to_string(response.status));
        if (j.contains("error") && j["error"].is_object()) err = j["error"].value("message", std::string("error"));
        const std::string desc = j.value("error_description", std::string());
        std::string hint;
        if (err == "invalid_grant") hint = " - sign in to the service again in Vaultik (the sign-in was revoked or expired)";
        throw Error(what + ": " + err + (desc.empty() ? "" : " (" + desc + ")") + hint);
    }
    return j;
}

nlohmann::json parse_secret(const std::string& secret, const char* service) {
    try {
        nlohmann::json j = nlohmann::json::parse(secret);
        if (j.is_object()) return j;
    } catch (const nlohmann::json::exception&) {
    }
    throw Error(std::string("no ") + service + " sign-in stored: sign in to " + service + " in Vaultik first");
}

OAuthTokens::OAuthTokens(http::Client& client, CloudCredentials& credentials, std::string token_url,
                         std::vector<std::pair<std::string, std::string>> extra_fields)
    : client_(client), credentials_(credentials), token_url_(std::move(token_url)), extra_fields_(std::move(extra_fields)) {}

std::string OAuthTokens::bearer() {
    std::lock_guard lock(mu_);
    if (!access_.empty() && std::chrono::steady_clock::now() < expires_) return access_;

    nlohmann::json secret = parse_secret(credentials_.secret, "the cloud drive");
    const std::string refresh = secret.value("refresh_token", std::string());
    if (refresh.empty()) throw Error("the stored sign-in has no refresh token: sign in again in Vaultik");
    std::vector<std::pair<std::string, std::string>> form = {{"grant_type", "refresh_token"},
                                                             {"refresh_token", refresh},
                                                             {"client_id", secret.value("client_id", std::string())}};
    const std::string client_secret = secret.value("client_secret", std::string());
    if (!client_secret.empty()) form.emplace_back("client_secret", client_secret);
    for (const auto& f : extra_fields_) form.push_back(f);
    const std::string body = form_encode(form);

    const http::Response r = send_with_retry(
        client_,
        [&] {
            http::Request req;
            req.method = "POST";
            req.url = token_url_;
            req.headers = {{"Content-Type", "application/x-www-form-urlencoded"}};
            req.body = ByteSpan(reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
            return req;
        },
        "renewing the sign-in");
    const nlohmann::json j = token_response(r, "renewing the sign-in");
    access_ = j.value("access_token", std::string());
    if (access_.empty()) throw Error("renewing the sign-in: no access token in the answer");
    // Renew a minute early so a token never runs out in the middle of a request.
    const int lifetime = j.value("expires_in", 3600);
    expires_ = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(60, lifetime - 60));

    const std::string rotated = j.value("refresh_token", std::string());
    if (!rotated.empty() && rotated != refresh) {
        secret["refresh_token"] = rotated;
        credentials_.secret = secret.dump();
        if (credentials_.on_secret_changed) credentials_.on_secret_changed(credentials_.secret);
    }
    return access_;
}

void OAuthTokens::invalidate(const std::string& token) {
    std::lock_guard lock(mu_);
    if (access_ == token) access_.clear();
}

}  // namespace vaultik::cloud
