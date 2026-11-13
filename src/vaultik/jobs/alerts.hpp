#pragma once

// Alerts for when nobody is at the computer: a message to a phone or a service when a backup
// fails or finds damage. A backup runs on its own and ends, so run-job sends them itself. The
// settings live in <config dir>/alerts.json; the tokens are sealed like job passwords
// (secrets/alerts-ntfy.key, secrets/alerts-telegram.key).
//
// Channels: ntfy (a push notification app; ntfy.sh or an own server; it can also forward the
// message as an email), Telegram (a bot's message to a chat), and a webhook (a JSON POST to any
// URL, e.g. for Home Assistant, Slack or Discord gateways).

#include <string>
#include <vector>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

struct AlertSettings {
    bool on_failure = true;  // a backup failed, or could not read some files
    bool on_damage = true;   // a check or test restore found damage, or the backup drive is failing
    bool on_success = false; // every finished backup too
    bool monthly_summary = false;  // a short summary of the month's backups once a month
    std::string ntfy_url;    // "https://ntfy.sh/my-secret-topic"
    std::string ntfy_email;  // ntfy forwards the message to this address too (optional)
    std::string telegram_chat;  // the chat id the bot writes to
    std::string webhook_url;
    bool any() const { return !ntfy_url.empty() || !telegram_chat.empty() || !webhook_url.empty(); }
    bool operator==(const AlertSettings&) const = default;
};

AlertSettings load_alerts(const std::string& dir = config_dir());
void save_alerts(const AlertSettings& settings, const std::string& dir = config_dir());
// What save_alerts checks before it writes: no control characters, URLs it may send to. Throws Error.
void check_alerts(const AlertSettings& settings);
// Whether `which` is the name of an alert channel with a token ("ntfy", "telegram"). The two
// below throw Error for any other name.
bool alert_secret_name(const std::string& which);
// The sealed tokens: ntfy's access token (optional) and the Telegram bot token.
std::string alert_secret(const std::string& which, const std::string& dir = config_dir());
void set_alert_secret(const std::string& which, const std::string& secret, const std::string& dir = config_dir());

struct Alert {
    enum class Level { Info, Warning, Critical };
    Level level = Level::Warning;
    std::string event;  // "failed", "partial", "damage", "drive", "success", "test"
    std::string job;    // the job's name
    std::string title;  // one line: "Backup of Documents failed"
    std::string message;
};

// The JSON a webhook receives (invalid UTF-8 in the texts is replaced, never refused).
std::string webhook_body(const Alert& alert, const std::string& computer, const std::string& time);

// Sends the alert to every configured channel. Never throws: returns one line per channel that
// failed ("Telegram: HTTP 401 ..."), empty when all went through.
std::vector<std::string> send_alert(const AlertSettings& settings, const Alert& alert,
                                    const std::string& dir = config_dir());

}  // namespace vaultik::jobs
