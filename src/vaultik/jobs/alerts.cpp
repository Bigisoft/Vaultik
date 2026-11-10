#include "vaultik/jobs/alerts.hpp"

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

#include "vaultik/platform/http.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"
#include "vaultik/util/bytes.hpp"

namespace vaultik::jobs {

namespace {

std::string settings_path(const std::string& dir) {
    return os::join(dir, "alerts.json");
}

std::string secret_path(const std::string& dir, const std::string& which) {
    return os::join(os::join(dir, "secrets"), "alerts-" + which + ".key");
}

ByteSpan bytes_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

// Only https (or http to this computer, for an own server on it) carries tokens and messages.
void check_url(const std::string& url) {
    const http::Url u = http::parse_url(url);
    if (u.scheme == "https") return;
    if (u.host == "localhost" || u.host == "127.0.0.1" || u.host == "[::1]") return;
    throw Error("an alert URL must start with https:// (" + url + ")");
}

std::string level_name(Alert::Level l) {
    switch (l) {
        case Alert::Level::Info: return "info";
        case Alert::Level::Warning: return "warning";
        case Alert::Level::Critical: return "critical";
    }
    return "warning";
}

void expect_ok(const http::Response& r, const std::string& what) {
    if (r.status >= 200 && r.status < 300) return;
    std::string body(r.body.begin(), r.body.end());
    if (body.size() > 200) body.resize(200);
    throw Error(what + ": HTTP " + std::to_string(r.status) + (body.empty() ? "" : " " + body));
}

}  // namespace

AlertSettings load_alerts(const std::string& dir) {
    AlertSettings s;
    try {
        const Bytes b = os::read_file(settings_path(dir), 1 << 20);
        const nlohmann::json j = nlohmann::json::parse(std::string(b.begin(), b.end()));
        s.on_failure = j.value("on_failure", s.on_failure);
        s.on_damage = j.value("on_damage", s.on_damage);
        s.on_success = j.value("on_success", s.on_success);
        s.ntfy_url = j.value("ntfy_url", std::string());
        s.ntfy_email = j.value("ntfy_email", std::string());
        s.telegram_chat = j.value("telegram_chat", std::string());
        s.webhook_url = j.value("webhook_url", std::string());
    } catch (const std::exception&) {
        // none yet (or unreadable): no alerts
    }
    return s;
}

void save_alerts(const AlertSettings& s, const std::string& dir) {
    for (const std::string* text : {&s.ntfy_url, &s.ntfy_email, &s.telegram_chat, &s.webhook_url})
        for (const char c : *text)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) throw Error("an alert setting contains control characters");
    for (const std::string* url : {&s.ntfy_url, &s.webhook_url})
        if (!url->empty()) check_url(*url);
    const nlohmann::json j = {{"on_failure", s.on_failure},   {"on_damage", s.on_damage},
                              {"on_success", s.on_success},   {"ntfy_url", s.ntfy_url},
                              {"ntfy_email", s.ntfy_email},   {"telegram_chat", s.telegram_chat},
                              {"webhook_url", s.webhook_url}};
    os::write_file_atomic(settings_path(dir), bytes_of(j.dump(2) + "\n"), 0600);
}

std::string alert_secret(const std::string& which, const std::string& dir) {
    try {
        Bytes sealed = os::read_file(secret_path(dir, which), 64 * 1024);
        Bytes plain = os::unprotect_secret(sealed);
        std::string out(plain.begin(), plain.end());
        secure_wipe(plain.data(), plain.size());
        return out;
    } catch (const std::exception&) {
        return {};
    }
}

void set_alert_secret(const std::string& which, const std::string& secret, const std::string& dir) {
    const std::string path = secret_path(dir, which);
    if (secret.empty()) {
        os::remove_file(path);
        return;
    }
    Bytes sealed = os::protect_secret(bytes_of(secret));
    os::write_file_atomic(path, sealed, 0600);
}

std::vector<std::string> send_alert(const AlertSettings& s, const Alert& a, const std::string& dir) {
    std::vector<std::string> failed;
    if (!s.any()) return failed;
    http::Client client;
    // A computer that just woke up may have no network yet: try again a few times, but never
    // hold the run up for more than about two minutes in all.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    const auto attempt = [&](const std::string& channel, const auto& fn) {
        for (int round = 0;; ++round) {
            try {
                fn();
                return;
            } catch (const http::NetworkError& e) {
                if (round >= 3 || std::chrono::steady_clock::now() + std::chrono::seconds(15) > deadline) {
                    failed.push_back(channel + ": " + e.what());
                    return;
                }
                std::this_thread::sleep_for(std::chrono::seconds(5 + 10 * round));
            } catch (const std::exception& e) {
                failed.push_back(channel + ": " + e.what());
                return;
            }
        }
    };
    const std::string text = a.title + (a.message.empty() ? "" : "\n" + a.message);

    if (!s.ntfy_url.empty()) {
        attempt("ntfy", [&] {
            check_url(s.ntfy_url);
            http::Request r;
            r.method = "POST";
            r.url = s.ntfy_url;
            r.headers.push_back({"Content-Type", "text/plain; charset=utf-8"});
            // Header values must be plain ASCII; the title goes in the body when it is not.
            bool ascii_title = true;
            for (const unsigned char c : a.title) ascii_title = ascii_title && c >= 0x20 && c < 0x7F;
            if (ascii_title) r.headers.push_back({"Title", a.title});
            r.headers.push_back({"Priority", a.level == Alert::Level::Critical ? "urgent"
                                              : a.level == Alert::Level::Warning ? "high"
                                                                                 : "default"});
            r.headers.push_back({"Tags", a.level == Alert::Level::Info ? "floppy_disk" : "warning,floppy_disk"});
            if (!s.ntfy_email.empty()) r.headers.push_back({"Email", s.ntfy_email});
            std::string token = alert_secret("ntfy", dir);
            if (!token.empty()) r.headers.push_back({"Authorization", "Bearer " + token});
            const std::string body = ascii_title ? a.message.empty() ? a.title : a.message : text;
            r.body = bytes_of(body);
            const http::Response resp = client.send(r);
            secure_wipe(token.data(), token.size());
            expect_ok(resp, "ntfy");
        });
    }
    if (!s.telegram_chat.empty()) {
        attempt("Telegram", [&] {
            std::string token = alert_secret("telegram", dir);
            if (token.empty()) throw Error("no bot token is set");
            http::Request r;
            r.method = "POST";
            r.url = "https://api.telegram.org/bot" + token + "/sendMessage";
            r.headers.push_back({"Content-Type", "application/x-www-form-urlencoded"});
            const std::string form = cloud::form_encode({{"chat_id", s.telegram_chat}, {"text", text}});
            r.body = bytes_of(form);
            const http::Response resp = client.send(r);
            secure_wipe(r.url.data(), r.url.size());
            secure_wipe(token.data(), token.size());
            expect_ok(resp, "Telegram");
        });
    }
    if (!s.webhook_url.empty()) {
        attempt("webhook", [&] {
            check_url(s.webhook_url);
            const nlohmann::json j = {{"source", "vaultik"},   {"event", a.event},       {"level", level_name(a.level)},
                                      {"job", a.job},          {"title", a.title},       {"message", a.message},
                                      {"computer", os::hostname()}, {"time", os::format_time_local(os::now())}};
            const std::string body = j.dump();
            http::Request r;
            r.method = "POST";
            r.url = s.webhook_url;
            r.headers.push_back({"Content-Type", "application/json"});
            r.body = bytes_of(body);
            expect_ok(client.send(r), "webhook");
        });
    }
    return failed;
}

}  // namespace vaultik::jobs
