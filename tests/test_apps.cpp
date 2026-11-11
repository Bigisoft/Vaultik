#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "vaultik/ops/apps.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/copy.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/repair.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
namespace fs = std::filesystem;

namespace {

constexpr char kPassword[] = "test password";

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-test-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::unique_ptr<Repository> make_repo(const fs::path& dir) {
    LocalBackend backend(u8(dir), true);
    InitOptions options;
    options.kdf_iterations = 1000;
    Repository::init(backend, kPassword, options);
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

UninstallEntry entry(std::string key, std::string name, std::string version = "1.0", std::string publisher = "") {
    UninstallEntry e;
    e.key_name = std::move(key);
    e.display_name = std::move(name);
    e.display_version = std::move(version);
    e.publisher = std::move(publisher);
    return e;
}

InstalledApp app(std::string name, std::string id = "", std::string version = "") {
    InstalledApp a;
    a.name = std::move(name);
    a.winget_id = std::move(id);
    a.version = std::move(version);
    if (!a.winget_id.empty()) a.source = "winget";
    return a;
}

Id id_of(std::uint8_t b) {
    Id id{};
    id.fill(b);
    return id;
}

}  // namespace

TEST_CASE("uninstall entries: programs only, each once") {
    std::vector<UninstallEntry> in;
    in.push_back(entry("7-Zip", "7-Zip 24.08 (x64)", "24.08", "Igor Pavlov"));
    in.push_back(entry("{GUID-1}", ""));  // no name
    UninstallEntry system = entry("{GUID-2}", "Microsoft Edge Update Helper");
    system.system_component = true;
    in.push_back(system);
    UninstallEntry child = entry("{GUID-3}", "Office 16 Click-to-Run Localization Component");
    child.parent_key_name = "Office16";
    in.push_back(child);
    UninstallEntry update = entry("{GUID-4}", "Update for Something");
    update.release_type = "Security Update";
    in.push_back(update);
    in.push_back(entry("KB5034441", "Some fix"));
    in.push_back(entry("{GUID-5}", "Security Update for Microsoft Office (KB4011574) 32-Bit Edition"));
    in.push_back(entry("Git_is1", "  Git  ", "2.51.0", "The Git Development Community"));
    in.push_back(entry("Git_is1", "Git", "2.51.0", "The Git Development Community"));  // 32-bit view too
    in.push_back(entry("KBuilder", "KBuilder Studio"));  // not a KB number

    const std::vector<InstalledApp> out = filter_uninstall_entries(in);
    REQUIRE(out.size() == 3);
    CHECK(out[0].name == "7-Zip 24.08 (x64)");
    CHECK(out[0].version == "24.08");
    CHECK(out[0].publisher == "Igor Pavlov");
    CHECK(out[1].name == "Git");
    CHECK(out[2].name == "KBuilder Studio");
}

TEST_CASE("program names are compared without versions and brackets") {
    CHECK(normalize_app_name("7-Zip 24.08 (x64)") == "7zip");
    CHECK(normalize_app_name("7-Zip 26.02 (x64)") == "7zip");
    CHECK(normalize_app_name("Notepad++ (64-bit x64)") == "notepad");
    CHECK(normalize_app_name("Sandboxie-Plus v1.16.4") == "sandboxieplus");
    CHECK(normalize_app_name("PostgreSQL 18") == "postgresql18");
    CHECK(normalize_app_name("(x64)") == "x64");  // nothing else: the name itself
}

TEST_CASE("winget export files") {
    const std::string text = R"({
        "$schema" : "https://aka.ms/winget-packages.schema.2.0.json",
        "CreationDate" : "2026-11-11T09:27:45.465-00:00",
        "Sources" : [
            {
                "Packages" : [
                    {"PackageIdentifier" : "7zip.7zip", "Version" : "26.02"},
                    {"PackageIdentifier" : "Microsoft.SQLServer.2025.Developer", "Version" : "Unknown"},
                    {"PackageIdentifier" : "bad id & calc", "Version" : "1"},
                    {"Version" : "1"}
                ],
                "SourceDetails" : {"Argument" : "https://cdn.winget.microsoft.com/cache",
                                   "Identifier" : "Microsoft.Winget.Source_8wekyb3d8bbwe",
                                   "Name" : "winget", "Type" : "Microsoft.PreIndexed.Package"}
            },
            {
                "Packages" : [{"PackageIdentifier" : "9NBLGGH4NNS1", "Version" : "1.2"}],
                "SourceDetails" : {"Name" : "msstore"}
            }
        ],
        "WinGetVersion" : "1.12.350"
    })";
    const std::vector<WingetPackage> p = parse_winget_export(text);
    REQUIRE(p.size() == 3);
    CHECK(p[0].id == "7zip.7zip");
    CHECK(p[0].version == "26.02");
    CHECK(p[0].source == "winget");
    CHECK(p[1].id == "Microsoft.SQLServer.2025.Developer");
    CHECK(p[1].version.empty());
    CHECK(p[2].id == "9NBLGGH4NNS1");
    CHECK(p[2].source == "msstore");

    CHECK_THROWS_AS(parse_winget_export("not json"), Error);
    CHECK_THROWS_AS(parse_winget_export("{\"x\": 1}"), Error);
    CHECK(parse_winget_export(R"({"Sources": []})").empty());
}

TEST_CASE("programs are matched to winget packages by whole names") {
    std::vector<InstalledApp> programs = {app("7-Zip 26.02 (x64)"), app("Microsoft OneDrive"), app("Git"),
                                          app("GitHub Desktop"), app("Notepad++ (64-bit x64)"),
                                          app("Some Old Tool")};
    const std::vector<WingetPackage> packages = {{"Git.Git", "2.51", "winget"},
                                                 {"7zip.7zip", "26.02", "winget"},
                                                 {"Microsoft.OneDrive", "26.1", "winget"},
                                                 {"Notepad++.Notepad++", "8.9", "winget"},
                                                 {"9NBLGGH4NNS1", "1.2", "msstore"}};
    const std::vector<InstalledApp> merged = merge_apps(programs, packages);
    REQUIRE(merged.size() == 7);
    CHECK(merged[0].winget_id == "7zip.7zip");
    CHECK(merged[1].winget_id == "Microsoft.OneDrive");
    CHECK(merged[2].winget_id == "Git.Git");
    CHECK(merged[3].winget_id.empty());  // "GitHub Desktop" is not Git
    CHECK(merged[4].winget_id == "Notepad++.Notepad++");
    CHECK(merged[5].winget_id.empty());
    CHECK(merged[6].name == "9NBLGGH4NNS1");  // a package matching no program is listed by its id
    CHECK(merged[6].winget_id == "9NBLGGH4NNS1");
    CHECK(merged[6].source == "msstore");
}

TEST_CASE("winget ids are checked against a strict set of characters") {
    CHECK(valid_winget_id("Git.Git"));
    CHECK(valid_winget_id("Notepad++.Notepad++"));
    CHECK(valid_winget_id("Microsoft.VCRedist.2015+.x64"));
    CHECK(valid_winget_id("9NBLGGH4NNS1"));
    CHECK(valid_winget_id("Python.Python.3_12-beta"));
    CHECK_FALSE(valid_winget_id(""));
    CHECK_FALSE(valid_winget_id("-silent"));
    CHECK_FALSE(valid_winget_id(".hidden"));
    CHECK_FALSE(valid_winget_id("Git.Git & calc"));
    CHECK_FALSE(valid_winget_id("Git.Git\" --override \"x"));
    CHECK_FALSE(valid_winget_id("a|b"));
    CHECK_FALSE(valid_winget_id("a;b"));
    CHECK_FALSE(valid_winget_id("..\\x"));
    CHECK_FALSE(valid_winget_id("Ünïcode"));
    CHECK_FALSE(valid_winget_id(std::string(129, 'a')));
    CHECK(valid_winget_id(std::string(128, 'a')));
    CHECK(valid_winget_source("winget"));
    CHECK(valid_winget_source("msstore"));
    CHECK_FALSE(valid_winget_source("https://evil.example"));
    CHECK_FALSE(valid_winget_source(""));
    CHECK_THROWS_AS(winget_install("Git.Git & calc", ""), Error);
    CHECK_THROWS_AS(winget_install("Git.Git", "mysource"), Error);
}

TEST_CASE("the programs missing on this computer") {
    const std::vector<InstalledApp> stored = {app("7-Zip 24.08 (x64)", "7zip.7zip", "24.08"),
                                              app("Git", "Git.Git"),
                                              app("Old Scanner Driver"),
                                              app("Photo Tool 3.1"),
                                              app("9NBLGGH4NNS1", "9NBLGGH4NNS1"),
                                              app("Notepad++", "Notepad++.Notepad++")};
    // Here: a newer 7-Zip (by name), Git under winget, the photo tool in another version.
    const std::vector<InstalledApp> here = {app("7-Zip 26.02 (x64)"), app("Git version control", "git.git"),
                                            app("Photo Tool 4.0")};
    std::vector<InstalledApp> missing = missing_apps(stored, here, true);
    REQUIRE(missing.size() == 3);
    CHECK(missing[0].name == "Old Scanner Driver");
    CHECK(missing[1].winget_id == "9NBLGGH4NNS1");
    CHECK(missing[2].winget_id == "Notepad++.Notepad++");

    // Without winget here, ids cannot be compared: Git only counts by name.
    missing = missing_apps(stored, here, false);
    CHECK(missing.size() == 4);
    CHECK(missing[0].name == "Git");
}

TEST_CASE("program lists as JSON") {
    std::vector<InstalledApp> apps = {app("Zeta"), app("alpha", "Alpha.Alpha", "1.0"), app("Zeta")};
    apps[0].publisher = "Zeta Corp \"quoted\"";
    apps[2].publisher = apps[0].publisher;
    const std::string json = apps_to_json(apps);
    // Sorted and without duplicates, so the same programs give the same blob.
    CHECK(json == apps_to_json({apps[2], apps[1]}));
    const std::vector<InstalledApp> back = apps_from_json(json);
    REQUIRE(back.size() == 2);
    CHECK(back[0].name == "alpha");
    CHECK(back[0].winget_id == "Alpha.Alpha");
    CHECK(back[0].source == "winget");
    CHECK(back[0].version == "1.0");
    CHECK(back[1].name == "Zeta");
    CHECK(back[1].publisher == "Zeta Corp \"quoted\"");
    CHECK(back[1].winget_id.empty());

    // Ids and sources that would not pass are dropped when read, not passed on.
    const std::vector<InstalledApp> tampered = apps_from_json(
        R"({"version":1,"apps":[{"name":"x","id":"x & calc","source":"evil"},{"id":"no.name"}]})");
    REQUIRE(tampered.size() == 1);
    CHECK(tampered[0].winget_id.empty());
    CHECK(tampered[0].source.empty());
    CHECK_THROWS_AS(apps_from_json(R"({"version":2,"apps":[]})"), Error);
    CHECK_THROWS_AS(apps_from_json("[1,2"), Error);
}

TEST_CASE("snapshots keep the program list id, and older ones read without it") {
    Snapshot s;
    s.time = 42;
    s.hostname = "host";
    s.tree = id_of(7);
    s.tags = {"daily"};
    s.copy_of = id_of(9);
    const Snapshot plain = parse_snapshot(serialize_snapshot(s));
    CHECK_FALSE(plain.apps.has_value());
    CHECK(plain.copy_of == s.copy_of);

    s.apps = id_of(5);
    const Bytes data = serialize_snapshot(s);
    const Snapshot back = parse_snapshot(data);
    REQUIRE(back.apps.has_value());
    CHECK(*back.apps == id_of(5));
    CHECK(back.tags == s.tags);  // the reserved tag is not shown as one
    CHECK(back.copy_of == s.copy_of);

    // A damaged id is ignored.
    ByteWriter w;
    w.u32(1);
    w.u64(1);
    w.str("h");
    w.str("u");
    w.varint(0);
    w.raw(id_of(1));
    w.u8(0);
    w.varint(1);
    w.str("vaultik:apps=zz");
    for (int i = 0; i < 7; ++i) w.varint(0);
    const Snapshot odd = parse_snapshot(w.take());
    CHECK_FALSE(odd.apps.has_value());
    CHECK(odd.tags.empty());
}

TEST_CASE("a backup keeps the program list; prune, copy, verify and repair know it") {
    TempDir tmp;
    write_text(tmp.path / "src" / "a.txt", "alpha");
    auto repo = make_repo(tmp.path / "repo");
    const std::vector<InstalledApp> apps = {app("Git", "Git.Git", "2.51"), app("Old Tool")};

    BackupOptions options;
    options.paths = {u8(tmp.path / "src")};
    options.tags = {"job:x"};
    const BackupResult without = run_backup(*repo, options);
    CHECK_FALSE(without.snapshot.apps.has_value());
    CHECK_FALSE(load_app_list(*repo, without.snapshot).has_value());

    options.app_list = [&] { return std::optional<std::string>(apps_to_json(apps)); };
    const BackupResult first = run_backup(*repo, options);
    write_text(tmp.path / "src" / "b.txt", "beta");
    const BackupResult second = run_backup(*repo, options);
    REQUIRE(first.snapshot.apps.has_value());
    CHECK(second.snapshot.apps == first.snapshot.apps);  // the same list is one blob
    const auto loaded = load_app_list(*repo, repo->load_snapshot(second.snapshot_id));
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->size() == 2);
    CHECK((*loaded)[0].winget_id == "Git.Git");

    // A run soon after keeps the last version's list without listing again.
    options.app_list = nullptr;
    options.reuse_app_list = true;
    write_text(tmp.path / "src" / "c.txt", "gamma");
    const BackupResult reused = run_backup(*repo, options);
    CHECK(reused.snapshot.apps == first.snapshot.apps);
    options.reuse_app_list = false;

    // A failing list is a warning, not a failed backup.
    std::vector<std::string> warnings;
    options.on_warning = [&](const std::string& w) { warnings.push_back(w); };
    options.app_list = []() -> std::optional<std::string> { throw Error("no registry"); };
    const BackupResult failed_list = run_backup(*repo, options);
    CHECK_FALSE(failed_list.snapshot.apps.has_value());
    CHECK(warnings.size() == 1);
    options.on_warning = nullptr;

    // Prune keeps it while a snapshot names it.
    run_prune(*repo, PruneOptions{});
    CHECK(load_app_list(*repo, repo->load_snapshot(second.snapshot_id)).has_value());
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*repo, vo).errors == 0);

    // A second copy has it too, under its own key.
    auto copy_repo = make_repo(tmp.path / "copy");
    CopyOptions co;
    co.tags = {"job:x"};
    copy_snapshots(*repo, *copy_repo, co);
    bool copied = false;
    for (const Id& id : copy_repo->list_snapshot_ids()) {
        const Snapshot s = copy_repo->load_snapshot(id);
        if (s.copy_of == second.snapshot_id) {
            REQUIRE(s.apps.has_value());
            CHECK(*s.apps != *second.snapshot.apps);
            const auto list = load_app_list(*copy_repo, s);
            REQUIRE(list.has_value());
            CHECK(list->size() == 2);
            copied = true;
        }
    }
    CHECK(copied);

    // A snapshot naming a list that is not there: verify reports it, repair drops the field.
    Snapshot lost = repo->load_snapshot(second.snapshot_id);
    lost.apps = id_of(3);
    const Id lost_id = repo->save_snapshot(lost);
    CHECK_THROWS_AS(load_app_list(*repo, lost), Error);
    CHECK(run_verify(*repo, VerifyOptions{}).errors == 1);
    repo.reset();
    {
        auto bare = Repository::open(std::make_unique<LocalBackend>(u8(tmp.path / "repo")), kPassword, false);
        const RepairResult r = run_repair(*bare, {});
        CHECK(r.snapshots_repaired == 1);
        CHECK(r.files_lost == 0);
    }
    repo = Repository::open(std::make_unique<LocalBackend>(u8(tmp.path / "repo")), kPassword);
    CHECK(run_verify(*repo, VerifyOptions{}).errors == 0);
    bool saved_again = false;
    for (const Id& id : repo->list_snapshot_ids()) {
        CHECK(id != lost_id);
        const Snapshot s = repo->load_snapshot(id);
        if (s.note.find("program list was lost") != std::string::npos) {
            CHECK_FALSE(s.apps.has_value());
            saved_again = true;
        }
    }
    CHECK(saved_again);
}
