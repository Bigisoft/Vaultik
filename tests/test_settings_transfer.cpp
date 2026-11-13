#include <doctest/doctest.h>

#include <filesystem>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/settings_transfer.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
using namespace vaultik::jobs;
namespace fs = std::filesystem;

namespace {

struct TempStore {
    fs::path path;
    TempStore() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-transfer-" + to_hex(rnd));
    }
    ~TempStore() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    std::string dir() const {
        const std::u8string s = path.u8string();
        return std::string(s.begin(), s.end());
    }
};

Job job(const std::string& name, const std::string& repo) {
    Job j;
    j.name = name;
    j.repository = repo;
    j.sources = {"C:/Users/me/Documents"};
    j.schedule.kind = Schedule::Kind::Daily;
    j.schedule.hour = 18;
    return j;
}

// A store with two jobs (one with a database dump), their secrets and alert settings.
void fill(const JobStore& store) {
    Job docs = job("Documents", "E:/Backups/docs");
    DatabaseDump db;
    db.id = "main";
    db.database = "app";
    db.user = "backup";
    docs.databases.push_back(db);
    store.save(docs);
    store.set_password(docs.id, "correct horse battery");
    store.set_database_secret(docs.id, "main", "db-secret");
    Job cloud = job("Photos", "s3:https://s3.eu-west-1.amazonaws.com/bucket/pc");
    cloud.cloud_key_id = "AKID";
    store.save(cloud);
    store.set_password(cloud.id, "photo password");
    store.set_cloud_secret(cloud.id, "cloud-secret-key");
    AlertSettings a;
    a.on_success = true;
    a.telegram_chat = "12345";
    save_alerts(a, store.dir());
    set_alert_secret("telegram", "bot-token", store.dir());
}

std::string text_of(const Bytes& b) {
    return std::string(b.begin(), b.end());
}

}  // namespace

TEST_CASE("settings export without secrets: no password or key in the file, imported paused") {
    TempStore from_dir, to_dir;
    const JobStore from(from_dir.dir());
    fill(from);
    nlohmann::json app = {{"close_to_tray", false}, {"remind_after_days", 3}};
    const SettingsBundle out = collect_settings(from, false, app);
    REQUIRE(out.jobs.size() == 2);
    const Bytes file = write_settings_file(out, "");
    const std::string text = text_of(file);
    for (const char* secret : {"correct horse battery", "photo password", "cloud-secret-key", "db-secret", "bot-token"})
        CHECK(text.find(secret) == std::string::npos);
    CHECK_FALSE(settings_file_encrypted(file));
    CHECK_THROWS_AS(write_settings_file(out, "a password"), Error);  // nothing to encrypt

    const SettingsBundle in = read_settings_file(file, "");
    CHECK_FALSE(in.with_secrets);
    CHECK(in.app.at("remind_after_days") == 3);
    REQUIRE(in.jobs.size() == 2);
    REQUIRE(in.alerts.has_value());
    CHECK(in.alerts->telegram_chat == "12345");

    const JobStore to(to_dir.dir());
    ImportChoice with_alerts;
    with_alerts.alerts = true;
    const ImportResult r = import_settings(to, in, with_alerts);
    REQUIRE(r.jobs.size() == 2);
    CHECK(r.alerts);
    for (const Job& j : to.list()) {
        CHECK(j.schedule.paused);  // nothing runs before the folders were checked
        CHECK(j.schedule.kind == Schedule::Kind::Daily);
        CHECK(j.schedule.hour == 18);
        CHECK(to.password(j.id).empty());  // asked for when first run or edited
        CHECK(to.cloud_secret(j.id).empty());
    }
    CHECK(load_alerts(to.dir()).on_success);
    CHECK(alert_secret("telegram", to.dir()).empty());
}

TEST_CASE("settings export with secrets: encrypted, a wrong password is refused, the secrets come back") {
    TempStore from_dir, to_dir;
    const JobStore from(from_dir.dir());
    fill(from);
    const SettingsBundle out = collect_settings(from, true);
    CHECK_THROWS_AS(write_settings_file(out, ""), Error);
    CHECK_THROWS_AS(write_settings_file(out, "short"), Error);
    const Bytes file = write_settings_file(out, "transfer password");
    const std::string text = text_of(file);
    // Nothing in clear: not the secrets, not even the job names or folders.
    for (const char* clear : {"correct horse battery", "cloud-secret-key", "db-secret", "bot-token", "Documents", "E:/Backups"})
        CHECK(text.find(clear) == std::string::npos);
    CHECK(settings_file_encrypted(file));
    CHECK_THROWS_AS(read_settings_file(file, ""), Error);
    CHECK_THROWS_AS(read_settings_file(file, "wrong password"), AuthError);
    // A changed byte of the sealed data is refused like a wrong password.
    nlohmann::json tampered = nlohmann::json::parse(text);
    std::string data = tampered["data"];
    data[data.size() / 2] = data[data.size() / 2] == 'A' ? 'B' : 'A';
    tampered["data"] = data;
    const std::string bad = tampered.dump();
    CHECK_THROWS_AS(read_settings_file(Bytes(bad.begin(), bad.end()), "transfer password"), AuthError);

    const SettingsBundle in = read_settings_file(file, "transfer password");
    CHECK(in.with_secrets);
    const JobStore to(to_dir.dir());
    ImportChoice with_alerts;
    with_alerts.alerts = true;
    const ImportResult r = import_settings(to, in, with_alerts);
    REQUIRE(r.jobs.size() == 2);
    const auto docs = to.find("documents");
    REQUIRE(docs.has_value());
    CHECK(docs->schedule.paused);
    CHECK(to.password(docs->id) == "correct horse battery");
    CHECK(to.database_secret(docs->id, "main") == "db-secret");
    const auto photos = to.find("photos");
    REQUIRE(photos.has_value());
    CHECK(to.cloud_secret(photos->id) == "cloud-secret-key");
    CHECK(alert_secret("telegram", to.dir()) == "bot-token");
}

TEST_CASE("settings import: choosing jobs, and a job whose id is taken is renamed or replaces it") {
    TempStore from_dir, to_dir;
    const JobStore from(from_dir.dir());
    fill(from);
    const Bytes file = write_settings_file(collect_settings(from, true), "transfer password");
    const SettingsBundle in = read_settings_file(file, "transfer password");

    const JobStore to(to_dir.dir());
    Job mine = job("Documents", "F:/Elsewhere");
    to.save(mine);
    to.set_password(mine.id, "my own password");
    REQUIRE(mine.id == "documents");

    ImportChoice only;
    only.jobs = {"photos"};
    only.alerts = false;
    ImportResult r = import_settings(to, in, only);
    REQUIRE(r.jobs.size() == 1);
    CHECK(r.jobs[0].id == "photos");
    CHECK_FALSE(r.alerts);
    CHECK(to.list().size() == 2);

    only.jobs = {"nothing-like-it"};
    CHECK_THROWS_AS(import_settings(to, in, only), Error);

    // Renamed: both stay, under different ids and names.
    ImportChoice rename;
    rename.jobs = {"documents"};
    r = import_settings(to, in, rename);
    REQUIRE(r.jobs.size() == 1);
    CHECK(r.jobs[0].renamed);
    CHECK(r.jobs[0].id != "documents");
    CHECK(r.jobs[0].name == "Documents (imported)");
    CHECK(to.find("documents")->repository == "F:/Elsewhere");
    CHECK(to.password("documents") == "my own password");
    CHECK(to.password(r.jobs[0].id) == "correct horse battery");
    CHECK(to.list().size() == 3);

    // Replaced: the one id now holds the imported job and its secrets.
    ImportChoice replace;
    replace.jobs = {"documents"};
    replace.replace = true;
    r = import_settings(to, in, replace);
    REQUIRE(r.jobs.size() == 1);
    CHECK(r.jobs[0].replaced);
    CHECK(r.jobs[0].id == "documents");
    CHECK(to.find("documents")->repository == "E:/Backups/docs");
    CHECK(to.find("documents")->schedule.paused);
    CHECK(to.password("documents") == "correct horse battery");
    CHECK(to.list().size() == 3);
}

TEST_CASE("settings files of another format version are refused") {
    TempStore from_dir;
    const JobStore from(from_dir.dir());
    fill(from);
    const Bytes file = write_settings_file(collect_settings(from, false), "");
    nlohmann::json j = nlohmann::json::parse(text_of(file));
    CHECK(j.at("version") == kSettingsFormatVersion);
    j["version"] = kSettingsFormatVersion + 1;
    std::string newer = j.dump();
    CHECK_THROWS_WITH_AS(read_settings_file(Bytes(newer.begin(), newer.end()), ""), doctest::Contains("newer Vaultik"), Error);
    CHECK_THROWS_AS(settings_file_encrypted(Bytes(newer.begin(), newer.end())), Error);
    j["version"] = 0;
    const std::string older = j.dump();
    CHECK_THROWS_WITH_AS(read_settings_file(Bytes(older.begin(), older.end()), ""), doctest::Contains("older format"), Error);
    const std::string other = R"({"format":"something-else","version":1})";
    CHECK_THROWS_AS(read_settings_file(Bytes(other.begin(), other.end()), ""), Error);
    const std::string junk = "not json at all";
    CHECK_THROWS_AS(read_settings_file(Bytes(junk.begin(), junk.end()), ""), Error);

    CHECK(default_settings_file_name("My PC/1", os::now()).starts_with("Vaultik-settings-My-PC-1-20"));
    CHECK(default_settings_file_name("pc", os::now()).ends_with(".vaultik-settings"));
}

TEST_CASE("settings import: alert settings only when asked for, and only the known alert channels") {
    TempStore from_dir, to_dir;
    const JobStore from(from_dir.dir());
    fill(from);
    const SettingsBundle in = read_settings_file(write_settings_file(collect_settings(from, true), "transfer password"),
                                                 "transfer password");
    const JobStore to(to_dir.dir());
    ImportResult r = import_settings(to, in, {});
    CHECK_FALSE(r.alerts);
    CHECK_FALSE(load_alerts(to.dir()).any());
    CHECK(alert_secret("telegram", to.dir()).empty());

    // A file naming an alert channel that is a path ("x/../<job id>") is refused as damaged; it
    // would overwrite a job's saved password otherwise.
    SettingsBundle crafted;
    crafted.with_secrets = true;
    crafted.alerts = AlertSettings{};
    crafted.alert_secrets["x/../documents"] = "evil";
    ImportChoice alerts;
    alerts.alerts = true;
    CHECK_THROWS_AS(import_settings(to, crafted, alerts), Error);
    // The same through a file.
    SettingsBundle saved = collect_settings(from, true);
    saved.alert_secrets["../../secret"] = "evil";
    const Bytes file = write_settings_file(saved, "transfer password");
    CHECK_THROWS_AS(read_settings_file(file, "transfer password"), Error);
    // And the store itself takes no other name.
    CHECK_THROWS_AS(set_alert_secret("x/../documents", "evil", to.dir()), Error);
    CHECK_THROWS_AS(set_alert_secret("", "", to.dir()), Error);
    CHECK_THROWS_AS(alert_secret("../telegram", to.dir()), Error);
    CHECK_NOTHROW(set_alert_secret("ntfy", "token", to.dir()));
    CHECK(alert_secret("ntfy", to.dir()) == "token");
}

TEST_CASE("settings import: every job is checked before anything is written or a secret removed") {
    TempStore from_dir, to_dir;
    const JobStore from(from_dir.dir());
    fill(from);
    SettingsBundle in = read_settings_file(write_settings_file(collect_settings(from, true), "transfer password"),
                                           "transfer password");
    const JobStore to(to_dir.dir());
    Job mine = job("Documents", "F:/Elsewhere");
    to.save(mine);
    to.set_password(mine.id, "my own password");
    REQUIRE(in.jobs.size() == 2);
    // The second job in the file is not a valid job: the first (which replaces "documents" and
    // would remove its password) must not be imported either.
    std::sort(in.jobs.begin(), in.jobs.end(), [](const Job& a, const Job& b) { return a.id < b.id; });
    REQUIRE(in.jobs[0].id == "documents");
    in.jobs[1].repository.clear();
    ImportChoice replace;
    replace.replace = true;
    CHECK_THROWS_AS(import_settings(to, in, replace), Error);
    CHECK(to.list().size() == 1);
    CHECK(to.find("documents")->repository == "F:/Elsewhere");
    CHECK(to.password("documents") == "my own password");
}

TEST_CASE("settings import: commands and administrator rights only when asked for") {
    TempStore from_dir, to_dir;
    const JobStore from(from_dir.dir());
    Job j = job("Documents", "E:/Backups/docs");
    j.before_command = "net stop something";
    j.after_command = "shutdown /s";
    j.run_as_admin = true;
    from.save(j);
    const SettingsBundle in = read_settings_file(write_settings_file(collect_settings(from, false), ""), "");
    REQUIRE(in.jobs.size() == 1);
    CHECK(runs_commands(in.jobs[0]));

    const JobStore to(to_dir.dir());
    ImportResult r = import_settings(to, in, {});
    REQUIRE(r.jobs.size() == 1);
    CHECK(r.jobs[0].stripped.size() == 3);
    auto got = to.find(r.jobs[0].id);
    REQUIRE(got.has_value());
    CHECK(got->before_command.empty());
    CHECK(got->after_command.empty());
    CHECK_FALSE(got->run_as_admin);
    CHECK_FALSE(runs_commands(*got));

    ImportChoice keep;
    keep.keep_commands = true;
    keep.replace = true;
    r = import_settings(to, in, keep);
    REQUIRE(r.jobs.size() == 1);
    CHECK(r.jobs[0].stripped.empty());
    got = to.find(r.jobs[0].id);
    REQUIRE(got.has_value());
    CHECK(got->before_command == "net stop something");
    CHECK(got->after_command == "shutdown /s");
    CHECK(got->run_as_admin);
}

TEST_CASE("settings files nested too deep are refused before they are parsed") {
    std::string deep = R"({"format":"vaultik-settings","version":1,"encrypted":false,"content":{"jobs":[)";
    for (int i = 0; i < 100000; ++i) deep += "[";
    for (int i = 0; i < 100000; ++i) deep += "]";
    deep += "]}}";
    CHECK_THROWS_AS(read_settings_file(Bytes(deep.begin(), deep.end()), ""), Error);
    // Brackets inside a string do not count.
    std::string text = R"({"format":"vaultik-settings","version":1,"encrypted":false,"content":{"computer":")";
    for (int i = 0; i < 1000; ++i) text += "[{";
    text += R"(\"","jobs":[]}})";
    CHECK(read_settings_file(Bytes(text.begin(), text.end()), "").jobs.empty());
}
