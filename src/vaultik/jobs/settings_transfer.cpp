#include "vaultik/jobs/settings_transfer.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/base64.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/version.hpp"

namespace vaultik::jobs {

using json = nlohmann::json;

namespace {

constexpr const char* kFormat = "vaultik-settings";
constexpr std::uint32_t kIterations = 600'000;  // PBKDF2-HMAC-SHA256, OWASP's figure for 2023
constexpr std::uint32_t kMaxIterations = 20'000'000;

// Bound to the sealed data, so a header cannot be moved onto other data.
const std::string kAad = "vaultik-settings v1";

void wipe_string(std::string& s) {
    secure_wipe(s.data(), s.size());
    s.clear();
}

constexpr std::size_t kMaxFileSize = 64u << 20;
constexpr int kMaxDepth = 64;  // far deeper than a settings file goes

// Refuses JSON nested deeper than kMaxDepth before it is parsed (a file made to be nested a
// million times deep would run the parser, or the copies made of it, out of stack).
void check_depth(const std::uint8_t* p, std::size_t n) {
    int depth = 0;
    bool in_string = false;
    for (std::size_t i = 0; i < n; ++i) {
        const char c = static_cast<char>(p[i]);
        if (in_string) {
            if (c == '\\') ++i;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{' || c == '[') {
            if (++depth > kMaxDepth) throw Error("the settings file is damaged (nested too deep)");
        } else if (c == '}' || c == ']') {
            --depth;
        }
    }
}

// Overwrites the secrets held in a content tree (before it is freed).
void wipe_secrets(json& c) {
    const auto wipe = [](json& v) {
        if (v.is_string()) {
            std::string& s = v.get_ref<std::string&>();
            secure_wipe(s.data(), s.size());
        }
    };
    if (c.is_object() && c.contains("secrets") && c["secrets"].is_object())
        for (auto& [id, s] : c["secrets"].items()) {
            if (!s.is_object()) continue;
            for (const char* k : {"password", "cloud", "copy"})
                if (s.contains(k)) wipe(s[k]);
            if (s.contains("databases") && s["databases"].is_object())
                for (auto& [db, pw] : s["databases"].items()) wipe(pw);
        }
    if (c.is_object() && c.contains("alert_secrets") && c["alert_secrets"].is_object())
        for (auto& [which, token] : c["alert_secrets"].items()) wipe(token);
}

json alerts_json(const AlertSettings& a) {
    return {{"on_failure", a.on_failure},         {"on_damage", a.on_damage},     {"on_success", a.on_success},
            {"monthly_summary", a.monthly_summary}, {"ntfy_url", a.ntfy_url},     {"ntfy_email", a.ntfy_email},
            {"telegram_chat", a.telegram_chat},   {"webhook_url", a.webhook_url}};
}

AlertSettings alerts_from(const json& j) {
    AlertSettings a;
    a.on_failure = j.value("on_failure", a.on_failure);
    a.on_damage = j.value("on_damage", a.on_damage);
    a.on_success = j.value("on_success", a.on_success);
    a.monthly_summary = j.value("monthly_summary", a.monthly_summary);
    a.ntfy_url = j.value("ntfy_url", std::string());
    a.ntfy_email = j.value("ntfy_email", std::string());
    a.telegram_chat = j.value("telegram_chat", std::string());
    a.webhook_url = j.value("webhook_url", std::string());
    return a;
}

json content_json(const SettingsBundle& b) {
    json c = {{"computer", b.computer}, {"created", b.created}, {"app_version", b.app_version}, {"app", b.app}};
    if (b.alerts) c["alerts"] = alerts_json(*b.alerts);
    json jobs = json::array();
    for (const Job& j : b.jobs) jobs.push_back(json::parse(to_json(j)));
    c["jobs"] = std::move(jobs);
    if (b.with_secrets) {
        json secrets = json::object();
        for (const auto& [id, s] : b.secrets) {
            json dbs = json::object();
            for (const auto& [db, pw] : s.databases) dbs[db] = pw;
            secrets[id] = {{"password", s.password}, {"cloud", s.cloud}, {"copy", s.copy}, {"databases", dbs}};
        }
        c["secrets"] = std::move(secrets);
        json alert = json::object();
        for (const auto& [which, token] : b.alert_secrets) alert[which] = token;
        c["alert_secrets"] = std::move(alert);
    }
    return c;
}

SettingsBundle bundle_from(const json& c, bool with_secrets) {
    SettingsBundle b;
    b.computer = c.value("computer", std::string());
    b.created = c.value("created", std::uint64_t{0});
    b.app_version = c.value("app_version", std::string());
    if (c.contains("app") && c.at("app").is_object()) b.app = c.at("app");
    if (c.contains("alerts") && c.at("alerts").is_object()) b.alerts = alerts_from(c.at("alerts"));
    if (c.contains("jobs")) {
        for (const json& j : c.at("jobs")) b.jobs.push_back(job_from_json(j.dump()));
    }
    b.with_secrets = with_secrets;
    if (with_secrets && c.contains("secrets")) {
        for (const auto& [id, s] : c.at("secrets").items()) {
            JobSecrets js;
            js.password = s.value("password", std::string());
            js.cloud = s.value("cloud", std::string());
            js.copy = s.value("copy", std::string());
            if (s.contains("databases"))
                for (const auto& [db, pw] : s.at("databases").items()) js.databases[db] = pw.get<std::string>();
            b.secrets[id] = std::move(js);
        }
    }
    if (with_secrets && c.contains("alert_secrets"))
        for (const auto& [which, token] : c.at("alert_secrets").items()) {
            // Only the known channels: the name becomes part of a file name when it is imported.
            if (!alert_secret_name(which)) throw Error("the settings file is damaged (an unknown alert channel)");
            b.alert_secrets[which] = token.get<std::string>();
        }
    return b;
}

json parse_header(ByteSpan file) {
    if (file.size() > kMaxFileSize) throw Error("this is not a Vaultik settings file (it is far too large)");
    check_depth(file.data(), file.size());
    json j;
    try {
        j = json::parse(file.begin(), file.end());
    } catch (const std::exception&) {
        throw Error("this is not a Vaultik settings file");
    }
    if (!j.is_object() || j.value("format", std::string()) != kFormat) throw Error("this is not a Vaultik settings file");
    const int version = j.value("version", 0);
    if (version > kSettingsFormatVersion)
        throw Error("this settings file was made by a newer Vaultik (format " + std::to_string(version) +
                    "); update Vaultik to import it");
    if (version < 1) throw Error("this settings file has an older format that this Vaultik cannot read");
    return j;
}

Key32 file_key(const std::string& password, ByteSpan salt, std::uint32_t iterations) {
    return pbkdf2_sha256(password, salt, iterations);
}

}  // namespace

void JobSecrets::wipe() {
    wipe_string(password);
    wipe_string(cloud);
    wipe_string(copy);
    for (auto& [id, pw] : databases) wipe_string(pw);
}

SettingsBundle::~SettingsBundle() {
    for (auto& [id, s] : secrets) s.wipe();
    for (auto& [which, token] : alert_secrets) wipe_string(token);
}

SettingsBundle collect_settings(const JobStore& store, bool with_secrets, const json& app) {
    SettingsBundle b;
    b.computer = os::hostname();
    b.created = os::now();
    b.app_version = VAULTIK_VERSION;
    b.app = app.is_object() ? app : json::object();
    b.alerts = load_alerts(store.dir());
    b.jobs = store.list();
    b.with_secrets = with_secrets;
    if (!with_secrets) return b;
    for (const Job& j : b.jobs) {
        JobSecrets s;
        s.password = store.password(j.id);
        s.cloud = store.cloud_secret(j.id);
        s.copy = store.copy_secret(j.id);
        for (const DatabaseDump& d : j.databases) {
            std::string pw = store.database_secret(j.id, d.id);
            if (!pw.empty()) s.databases[d.id] = std::move(pw);
        }
        b.secrets[j.id] = std::move(s);
    }
    for (const char* which : {"ntfy", "telegram"}) {
        std::string token = alert_secret(which, store.dir());
        if (!token.empty()) b.alert_secrets[which] = std::move(token);
    }
    return b;
}

std::string default_settings_file_name(const std::string& computer, std::uint64_t when) {
    std::string name;
    for (const char c : computer) {
        const auto u = static_cast<unsigned char>(c);
        name.push_back(std::isalnum(u) || c == '-' || c == '_' ? c : '-');
    }
    if (name.empty()) name = "computer";
    const std::time_t t = static_cast<std::time_t>(when >= os::kUnixEpochTicks ? (when - os::kUnixEpochTicks) / 10'000'000 : 0);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char date[16];
    std::strftime(date, sizeof date, "%Y-%m-%d", &tm);
    return "Vaultik-settings-" + name + "-" + date + kSettingsExtension;
}

Bytes write_settings_file(const SettingsBundle& bundle, const std::string& password) {
    json file = {{"format", kFormat}, {"version", kSettingsFormatVersion}};
    std::string content;
    {
        json tree = content_json(bundle);
        content = tree.dump(2);
        wipe_secrets(tree);  // the tree held them too
    }
    if (!bundle.with_secrets) {
        if (!password.empty()) throw Error("a settings file without secrets is not encrypted; give no password");
        file["encrypted"] = false;
        file["content"] = json::parse(content);
        const std::string text = file.dump(2) + "\n";
        return Bytes(text.begin(), text.end());
    }
    if (password.size() < 8) {
        wipe_string(content);
        throw Error("a settings file with passwords and keys needs a password of at least 8 characters to encrypt it");
    }
    std::uint8_t salt[32];
    random_bytes(salt);
    Key32 key = file_key(password, salt, kIterations);
    Bytes sealed;
    {
        const AesGcm aead(key);
        sealed = aead.seal(ByteSpan(reinterpret_cast<const std::uint8_t*>(content.data()), content.size()),
                           ByteSpan(reinterpret_cast<const std::uint8_t*>(kAad.data()), kAad.size()));
    }
    secure_wipe(key.data(), key.size());
    wipe_string(content);
    file["encrypted"] = true;
    file["kdf"] = "pbkdf2-sha256";
    file["iterations"] = kIterations;
    file["salt"] = to_hex(salt);
    file["data"] = base64_encode(sealed);
    const std::string text = file.dump(2) + "\n";
    return Bytes(text.begin(), text.end());
}

bool settings_file_encrypted(ByteSpan file) {
    return parse_header(file).value("encrypted", false);
}

SettingsBundle read_settings_file(ByteSpan file, const std::string& password) {
    const json j = parse_header(file);
    if (!j.value("encrypted", false)) {
        if (!j.contains("content") || !j.at("content").is_object()) throw Error("the settings file is damaged (no content)");
        return bundle_from(j.at("content"), false);
    }
    if (password.empty()) throw Error("this settings file is encrypted; its password is needed");
    if (j.value("kdf", std::string()) != "pbkdf2-sha256") throw Error("the settings file uses a key derivation this Vaultik does not know");
    const std::uint32_t iterations = j.value("iterations", 0u);
    if (iterations < 100'000 || iterations > kMaxIterations) throw Error("the settings file is damaged (key derivation)");
    const Bytes salt = from_hex(j.value("salt", std::string()));
    if (salt.size() != 32) throw Error("the settings file is damaged (salt)");
    const Bytes sealed = base64_decode(j.value("data", std::string()));
    Key32 key = file_key(password, salt, iterations);
    Bytes plain;
    try {
        const AesGcm aead(key);
        plain = aead.open(sealed, ByteSpan(reinterpret_cast<const std::uint8_t*>(kAad.data()), kAad.size()));
    } catch (const AuthError&) {
        secure_wipe(key.data(), key.size());
        throw AuthError("wrong password for this settings file (or the file is damaged)");
    }
    secure_wipe(key.data(), key.size());
    json content;
    try {
        check_depth(plain.data(), plain.size());
        content = json::parse(plain.begin(), plain.end());
    } catch (const std::exception&) {
        secure_wipe(plain.data(), plain.size());
        throw Error("the settings file is damaged (content)");
    }
    secure_wipe(plain.data(), plain.size());
    try {
        SettingsBundle b = bundle_from(content, true);
        wipe_secrets(content);  // the parsed copy held the secrets too
        return b;
    } catch (...) {
        wipe_secrets(content);
        throw;
    }
}

bool runs_commands(const Job& job) {
    return !job.before_command.empty() || !job.after_command.empty() || job.run_as_admin;
}

std::vector<std::string> strip_commands(Job& job) {
    std::vector<std::string> out;
    if (!job.before_command.empty()) out.push_back("the command before the backup (" + job.before_command + ")");
    if (!job.after_command.empty()) out.push_back("the command after the backup (" + job.after_command + ")");
    if (job.run_as_admin) out.push_back("running with administrator rights");
    job.before_command.clear();
    job.after_command.clear();
    job.run_as_admin = false;
    return out;
}

ImportResult import_settings(const JobStore& store, const SettingsBundle& bundle, const ImportChoice& choice) {
    for (const std::string& id : choice.jobs)
        if (std::none_of(bundle.jobs.begin(), bundle.jobs.end(), [&](const Job& j) { return j.id == id; }))
            throw Error("the settings file has no job \"" + id + "\"");
    if (choice.alerts && bundle.with_secrets)
        for (const auto& [which, token] : bundle.alert_secrets)
            if (!alert_secret_name(which)) throw Error("the settings file is damaged (an unknown alert channel)");

    // First every chosen job as it will be saved, each checked: nothing is written (no job, no
    // secret removed) when one of them is not right.
    struct Planned {
        Job job;
        ImportedJob done;
        bool clears_secrets = false;  // the replaced job's secrets go
    };
    std::vector<Planned> plan;
    std::vector<Job> existing = store.list();
    for (const Job& from : bundle.jobs) {
        if (!choice.jobs.empty() && std::find(choice.jobs.begin(), choice.jobs.end(), from.id) == choice.jobs.end())
            continue;
        if (from.id.empty()) throw Error("the settings file is damaged (a job without an id)");
        Planned p;
        Job& job = p.job;
        job = from;
        // Nothing runs on its own before the user checked the job's folders on this computer.
        job.schedule.paused = true;
        // Commands to run and administrator rights only when asked for: they were not seen yet.
        if (!choice.keep_commands) p.done.stripped = strip_commands(job);
        p.done.from_id = from.id;
        const auto clash = std::find_if(existing.begin(), existing.end(), [&](const Job& j) { return j.id == from.id; });
        if (clash != existing.end()) {
            if (choice.replace) {
                p.done.replaced = true;
                // The old job's secrets belong to its destination: kept when it is the same one
                // and the file brings none, removed otherwise.
                const bool same_place = clash->repository == job.repository;
                const bool brings = bundle.with_secrets && bundle.secrets.count(from.id) != 0;
                p.clears_secrets = !same_place || brings;
            } else {
                std::vector<std::string> taken;
                for (const Job& j : existing) taken.push_back(j.id);
                for (const Planned& q : plan) taken.push_back(q.job.id);
                job.id = make_id(job.name, taken);
                if (job.id == from.id) job.id = make_id(job.name + "-imported", taken);
                p.done.renamed = true;
                // Two jobs of one name would look the same in the list.
                const auto same_name = [&](const std::string& n) {
                    return std::any_of(existing.begin(), existing.end(), [&](const Job& j) { return j.name == n; }) ||
                           std::any_of(plan.begin(), plan.end(), [&](const Planned& q) { return q.job.name == n; });
                };
                if (same_name(job.name)) {
                    std::string name = job.name + " (imported)";
                    for (int n = 2; same_name(name); ++n) name = job.name + " (imported " + std::to_string(n) + ")";
                    job.name = name;
                }
            }
        }
        validate(job);
        if (std::any_of(plan.begin(), plan.end(), [&](const Planned& q) { return q.job.id == job.id; }))
            throw Error("the settings file holds the job \"" + job.id + "\" twice");
        if (clash == existing.end() || !choice.replace) existing.push_back(job);
        plan.push_back(std::move(p));
    }
    if (choice.alerts && bundle.alerts) check_alerts(*bundle.alerts);  // as save_alerts does, before anything is written

    // Then saved, and only then their secrets changed.
    ImportResult r;
    for (Planned& p : plan) store.save(p.job);
    for (Planned& p : plan) {
        Job& job = p.job;
        if (p.clears_secrets) {
            store.set_password(job.id, "");
            store.set_cloud_secret(job.id, "");
            store.set_copy_secret(job.id, "");
        }
        if (bundle.with_secrets) {
            const auto s = bundle.secrets.find(p.done.from_id);
            if (s != bundle.secrets.end()) {
                if (!s->second.password.empty()) store.set_password(job.id, s->second.password);
                store.set_cloud_secret(job.id, s->second.cloud);
                store.set_copy_secret(job.id, s->second.copy);
                for (const auto& [db, pw] : s->second.databases) {
                    if (std::any_of(job.databases.begin(), job.databases.end(), [&](const DatabaseDump& d) { return d.id == db; }))
                        store.set_database_secret(job.id, db, pw);
                }
                p.done.secrets = !s->second.password.empty();
            }
        }
        p.done.id = job.id;
        p.done.name = job.name;
        r.jobs.push_back(std::move(p.done));
    }
    if (choice.alerts && bundle.alerts) {
        save_alerts(*bundle.alerts, store.dir());
        if (bundle.with_secrets)
            for (const auto& [which, token] : bundle.alert_secrets) set_alert_secret(which, token, store.dir());
        r.alerts = true;
    }
    return r;
}

}  // namespace vaultik::jobs
