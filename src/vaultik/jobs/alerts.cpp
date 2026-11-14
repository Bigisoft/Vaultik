#include "vaultik/jobs/alerts.hpp"

#include "vaultik/jobs/admin_jobs.hpp"

#include <chrono>
#include <cstdio>
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

// The file of an alert channel's token. Only the known channels: the name becomes part of a file
// name, and one such as "x/../<job id>" would reach a job's saved password.
std::string secret_path(const std::string& dir, const std::string& which) {
    if (!alert_secret_name(which)) throw Error("there is no alert channel \"" + which + "\"");
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

// "1.50 GiB", as the command line writes sizes.
std::string size_text(std::uint64_t bytes) {
    static constexpr const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    char buf[32];
    if (unit == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.2f %s", v, kUnits[unit]);
    return buf;
}

}  // namespace

std::optional<Alert> alert_for_run(const AlertSettings& s, const std::string& job_name, const RunRecord& r,
                                   const std::string& computer) {
    // Nothing to tell: no channel, or a run that only waited for its drive (the overdue reminder
    // is what speaks up when that goes on too long).
    if (!s.any() || r.status == RunStatus::Waiting) return std::nullopt;
    Alert a;
    a.job = job_name;
    const std::string where = " Open Vaultik on " + computer + " for the details.";
    const bool check = is_check(r.kind) || r.kind == RunKind::RestoreTest;
    if (check && s.on_damage && r.rot_files > 0 && !(r.status == RunStatus::Failed && r.errors > 0)) {
        // Files on this computer that changed by themselves: the backup is fine, the drive is not.
        a.level = Alert::Level::Critical;
        a.event = "rot";
        a.title = "Health check of " + job_name + " found files that changed by themselves";
        a.message = std::to_string(r.rot_files) +
                    " file(s) on the computer changed although nothing wrote to them, which a failing drive does. "
                    "The backup holds the earlier copy." + where;
    } else if (check_not_finished(r)) {
        // A health check that could not run or stopped part way (the drive was unplugged, say): it
        // does not count as done, and nobody would know the backup went unchecked.
        if (!s.on_failure) return std::nullopt;
        a.level = Alert::Level::Warning;
        a.event = "check-failed";
        a.title = "Health check of " + job_name + " could not finish";
        a.message = "The backup was not fully checked; the check runs again after the next backup." + where;
    } else if (check) {
        // Damage found.
        if (!s.on_damage || r.status != RunStatus::Failed || r.errors == 0) return std::nullopt;
        a.level = Alert::Level::Critical;
        a.event = "damage";
        a.title = (r.kind == RunKind::RestoreTest ? "Test restore of " : "Health check of ") + job_name + " found damage";
        a.message = "Some backed-up data could not be read back or restored (" + std::to_string(r.errors) +
                    " problem(s))." + where;
    } else if (!r.alert.empty()) {
        a.level = Alert::Level::Critical;
        a.event = "ransomware";
        a.title = job_name + ": files may be encrypted by ransomware";
        a.message = "Many files changed the way ransomware changes them. Disconnect the backup drive." + where;
    } else if (r.status == RunStatus::Failed && r.reason == "password") {
        if (!s.on_failure) return std::nullopt;
        a.level = Alert::Level::Critical;
        a.event = "password";
        a.title = "Backup of " + job_name + " cannot run: its password is not saved on this PC";
        a.message = "Scheduled backups of this job do not run until the password is saved in the job." + where;
    } else if (r.status == RunStatus::Failed) {
        if (!s.on_failure) return std::nullopt;
        a.level = Alert::Level::Critical;
        a.event = "failed";
        a.title = "Backup of " + job_name + " failed";
        a.message = "No version was saved." + where;
    } else if (!r.drive.empty() && s.on_damage) {
        a.level = Alert::Level::Warning;
        a.event = "drive";
        a.title = "The backup drive of " + job_name + " reports a problem";
        a.message = "Copy the backup to another drive soon, before it fails." + where;
    } else if (r.status == RunStatus::Partial) {
        if (!s.on_failure) return std::nullopt;
        a.level = Alert::Level::Warning;
        a.event = "partial";
        a.title = "Backup of " + job_name + " skipped " + std::to_string(r.errors) + " item(s)";
        a.message = "A version was saved without them." + where;
    } else if (r.status == RunStatus::Success && r.kind == RunKind::Backup && s.on_success) {
        a.level = Alert::Level::Info;
        a.event = "success";
        a.title = "Backup of " + job_name + " completed";
        a.message = std::to_string(r.files) + " files, " + size_text(r.new_bytes) + " new";
    } else {
        return std::nullopt;
    }
    a.title += " (" + computer + ")";
    return a;
}

AlertSettings load_alerts(const std::string& dir) {
    AlertSettings s;
    try {
        const Bytes b = os::read_file(settings_path(dir), 1 << 20);
        const nlohmann::json j = nlohmann::json::parse(std::string(b.begin(), b.end()));
        s.on_failure = j.value("on_failure", s.on_failure);
        s.on_damage = j.value("on_damage", s.on_damage);
        s.on_success = j.value("on_success", s.on_success);
        s.monthly_summary = j.value("monthly_summary", s.monthly_summary);
        s.ntfy_url = j.value("ntfy_url", std::string());
        s.ntfy_email = j.value("ntfy_email", std::string());
        s.telegram_chat = j.value("telegram_chat", std::string());
        s.webhook_url = j.value("webhook_url", std::string());
    } catch (const std::exception&) {
        // none yet (or unreadable): no alerts
    }
    return s;
}

void check_alerts(const AlertSettings& s) {
    for (const std::string* text : {&s.ntfy_url, &s.ntfy_email, &s.telegram_chat, &s.webhook_url})
        for (const char c : *text)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) throw Error("an alert setting contains control characters");
    for (const std::string* url : {&s.ntfy_url, &s.webhook_url})
        if (!url->empty()) check_url(*url);
}

void save_alerts(const AlertSettings& s, const std::string& dir) {
    check_alerts(s);
    const nlohmann::json j = {{"on_failure", s.on_failure},   {"on_damage", s.on_damage},
                              {"on_success", s.on_success},   {"monthly_summary", s.monthly_summary},
                              {"ntfy_url", s.ntfy_url},
                              {"ntfy_email", s.ntfy_email},   {"telegram_chat", s.telegram_chat},
                              {"webhook_url", s.webhook_url}};
    os::write_file_atomic(settings_path(dir), bytes_of(j.dump(2) + "\n"), 0600);
}

bool alert_secret_name(const std::string& which) {
    return which == "ntfy" || which == "telegram";
}

std::string alert_secret(const std::string& which, const std::string& dir) {
    if (!alert_secret_name(which)) throw Error("there is no alert channel \"" + which + "\"");
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
    Bytes sealed = os::protect_secret(bytes_of(secret), in_admin_store(path));
    os::make_private_dirs(os::parent_of(path));  // Linux: the 0700 folder is what keeps others out
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
            const std::string body = webhook_body(a, os::hostname(), os::format_time_local(os::now()));
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

std::string webhook_body(const Alert& a, const std::string& computer, const std::string& time) {
    const nlohmann::json j = {{"source", "vaultik"}, {"event", a.event},  {"level", level_name(a.level)},
                              {"job", a.job},        {"title", a.title},  {"message", a.message},
                              {"computer", computer}, {"time", time}};
    // A message may quote a file name that is not valid UTF-8 (Linux names are bytes): such bytes
    // become U+FFFD instead of dropping the whole alert.
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace vaultik::jobs
