#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "vaultik/jobs/growth.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/rot_check.hpp"
#include "vaultik/platform/os.hpp"
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

Bytes seeded_bytes(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    Bytes b(n);
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    return b;
}

void write_file(const fs::path& p, const Bytes& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

#ifdef _WIN32
// Sets a file's NTFS change time, which every write moves: the damage a test makes must not look
// written to.
void set_change_time(const fs::path& p, std::uint64_t t) {
    const HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    FILE_BASIC_INFO basic{};  // zero fields are left unchanged
    basic.ChangeTime.QuadPart = static_cast<LONGLONG>(t);
    CHECK(SetFileInformationByHandle(h, FileBasicInfo, &basic, sizeof basic));
    CloseHandle(h);
    CHECK(os::change_time(u8(p)) == t);
}
#endif

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::unique_ptr<Repository> new_repo(const fs::path& dir) {
    {
        LocalBackend backend(u8(dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, kPassword, options);
    }
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

std::string snapshot_path_of(const fs::path& source) {
    std::string s;
    for (const std::string& c : os::path_components(u8(source))) s += (s.empty() ? "" : "/") + c;
    return s;
}

// Snapshot paths below `source` as if `source` were "D:\work\src" (Windows) or "/work/src": the
// test's own temporary folder may itself be one a ready-made exclusion leaves out.
std::function<std::string(const std::string&)> fake_places(const fs::path& source) {
    const std::string prefix = snapshot_path_of(source);
    return [prefix](const std::string& p) -> std::string {
        if (p != prefix && !p.starts_with(prefix + "/")) return {};
#ifdef _WIN32
        std::string out = "D:\\work\\src" + p.substr(prefix.size());
        std::replace(out.begin(), out.end(), '/', '\\');
        return out;
#else
        return "/work/src" + p.substr(prefix.size());
#endif
    };
}

std::vector<std::pair<Id, Snapshot>> all_snapshots(Repository& repo) {
    std::vector<std::pair<Id, Snapshot>> out;
    for (const Id& id : repo.list_snapshot_ids()) out.emplace_back(id, repo.load_snapshot(id));
    return out;
}

}  // namespace

TEST_CASE("what grew names the folders that added new data and suggests exclusions") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    write_file(src / "docs" / "a.txt", seeded_bytes(200'000, 1));
    auto repo = new_repo(tmp.path / "repo");
    BackupOptions options;
    options.paths = {u8(src)};
    run_backup(*repo, options);
    const std::uint64_t between = os::now() + 1;

    // New data in the period: a download, a package folder, and a copy of a file stored before.
    write_file(src / "Downloads" / "movie.bin", seeded_bytes(3'000'000, 2));
    write_file(src / "proj" / "node_modules" / "pkg" / "lib.bin", seeded_bytes(2'000'000, 3));
    write_file(src / "proj" / "copy-of-a.txt", read_file(src / "docs" / "a.txt"));
    run_backup(*repo, options);

    jobs::GrowthOptions o;
    o.since = between;
    o.local_path = fake_places(src);
    jobs::GrowthReport r = jobs::what_grew(*repo, all_snapshots(*repo), o);
    CHECK(r.versions == 1);
    CHECK(r.files == 2);  // the copy brought nothing new
    CHECK(r.raw == 5'000'000);
    CHECK(r.stored >= 5'000'000);
    REQUIRE(r.folders.size() == 2);
    const std::string sep = os::kSeparator == '\\' ? "\\" : "/";
    const std::string root = sep == "\\" ? "D:\\work\\src" : "/work/src";
    CHECK(r.folders[0].path == root + sep + "Downloads");
    CHECK(r.folders[0].files == 1);
    REQUIRE(r.folders[0].suggestion);
    CHECK(r.folders[0].suggestion->preset.empty());
    CHECK(r.folders[0].suggestion->patterns == std::vector<std::string>{root + sep + "Downloads"});
    CHECK(r.folders[1].path == root + sep + "proj" + sep + "node_modules");
    REQUIRE(r.folders[1].suggestion);
    CHECK(r.folders[1].suggestion->preset == "dev");
    REQUIRE(!r.presets.empty());
    CHECK(r.presets[0].id == "dev");
    CHECK(r.presets[0].files == 1);

    // A job that uses the preset and skips Downloads already gets no suggestions for them.
    const auto& presets = jobs::exclusion_presets();
    const auto dev = std::find_if(presets.begin(), presets.end(), [](const jobs::ExclusionPreset& p) { return p.id == "dev"; });
    REQUIRE(dev != presets.end());
    o.excludes = dev->patterns;
    o.excludes.push_back(root + sep + "Downloads");
    r = jobs::what_grew(*repo, all_snapshots(*repo), o);
    REQUIRE(r.folders.size() == 2);
    CHECK(!r.folders[0].suggestion);
    CHECK(!r.folders[1].suggestion);
    CHECK(std::none_of(r.presets.begin(), r.presets.end(), [](const jobs::PresetGrowth& p) { return p.id == "dev"; }));

    // Only versions the job made count; from the start, everything is new.
    o.since = 0;
    o.select = [](const Snapshot&) { return true; };
    r = jobs::what_grew(*repo, all_snapshots(*repo), o);
    CHECK(r.versions == 2);
    CHECK(r.raw == 5'200'000);
    o.select = [](const Snapshot&) { return false; };
    r = jobs::what_grew(*repo, all_snapshots(*repo), o);
    CHECK(r.versions == 0);
    CHECK(r.files == 0);
    CHECK(r.folders.empty());
    CHECK(jobs::to_json(r).find("\"folders\":[]") != std::string::npos);
}

TEST_CASE("a health check notices a file whose content changed while its size and times did not") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const auto old = fs::file_time_type::clock::now() - std::chrono::hours(24 * 10);
    for (int i = 0; i < 20; ++i) {
        const fs::path f = src / ("dir" + std::to_string(i % 3)) / ("f" + std::to_string(i) + ".bin");
        write_file(f, seeded_bytes(30'000 + static_cast<std::size_t>(i) * 101, 50 + static_cast<std::uint32_t>(i)));
        fs::last_write_time(f, old);
    }
    write_file(src / "fresh.bin", seeded_bytes(1000, 9));  // written just now: never picked
    auto repo = new_repo(tmp.path / "repo");
    BackupOptions options;
    options.paths = {u8(src)};
    const BackupResult backup = run_backup(*repo, options);

    RotCheckOptions o;
    o.max_files = 100;
    o.seed = 3;
#ifndef _WIN32
    o.compare_ctime = false;  // setting the time back changes the ctime here
#endif
    RotCheckResult r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.candidates == 20);
    CHECK(r.checked == 20);
    CHECK(r.damaged.empty());
    CHECK(r.changed == 0);

    // Silent damage: one byte differs, the size and times are as they were.
    const fs::path victim = src / "dir2" / "f5.bin";
#ifdef _WIN32
    const std::uint64_t victim_change = os::change_time(u8(victim));
    REQUIRE(victim_change != 0);
    REQUIRE(victim_change <= backup.snapshot.time);
#endif
    Bytes content = read_file(victim);
    content[20'000] ^= 0x01;
    write_file(victim, content);
    fs::last_write_time(victim, old);
#ifdef _WIN32
    set_change_time(victim, victim_change);
    // Rewritten in place with its modification time set back, as VeraCrypt does with its
    // containers: the change time tells, so it is a change, not damage.
    const fs::path rewritten = src / "dir0" / "f3.bin";
    Bytes rewritten_content = read_file(rewritten);
    rewritten_content[100] ^= 0xFF;
    write_file(rewritten, rewritten_content);
    fs::last_write_time(rewritten, old);
    constexpr std::uint64_t kRewritten = 1;
#else
    constexpr std::uint64_t kRewritten = 0;
#endif
    // An ordinary change: new content, new time.
    const fs::path edited = src / "dir1" / "f7.bin";
    write_file(edited, seeded_bytes(30'707, 77));
    const auto victim_time = fs::last_write_time(victim);

    r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.changed == 1 + kRewritten);
    CHECK(r.checked == 19 - kRewritten);
    REQUIRE(r.damaged.size() == 1);
    CHECK(r.damaged[0].path == u8(victim));
    CHECK(r.damaged[0].size == content.size());
    CHECK(r.damaged[0].offset == 0);  // a file this small is one chunk
    // The check only read: the damaged file is as it was.
    CHECK(read_file(victim) == content);
    CHECK(fs::last_write_time(victim) == victim_time);

    // Limits: a byte budget smaller than every file reads nothing; no time reads nothing.
    o.max_bytes = 1000;
    r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.checked == 0);
    o.max_bytes = 1ull << 30;
    o.max_seconds = 0;
    r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.budget_reached);
    CHECK(r.checked == 0);
    o.max_seconds = 120;
    o.max_files = 5;
    r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.checked == 5);

    // A file that cannot be found (or read) is skipped, not reported.
    fs::remove(victim);
    o.max_files = 100;
    r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.damaged.empty());
    CHECK(r.changed == 2 + kRewritten);
}

TEST_CASE("a health check's silent damage findings are kept with the run and warn in the system log") {
    jobs::RunRecord r;
    r.kind = jobs::RunKind::Check;
    r.status = jobs::RunStatus::Success;
    r.rot_files = 2;
    r.rot = "C:\\a.jpg; C:\\b.docx";
    r.rot_version = 133'000'000'000'000'000ull;
    const jobs::RunRecord back = jobs::record_from_json(jobs::to_json(r));
    CHECK(back.rot_files == 2);
    CHECK(back.rot == r.rot);
    CHECK(back.rot_version == r.rot_version);
    const jobs::SystemLogEntry e = jobs::system_log_entry(r, "Documents", "documents");
    CHECK(e.id == 205);
    CHECK(e.level == 1);
    CHECK(e.text.find("a.jpg") == std::string::npos);  // never file names in the shared log
    CHECK(e.text.find("2 file(s) on this computer changed") != std::string::npos);

    jobs::Job job;
    job.name = "x";
    job.sources = {"C:\\x"};
    job.repository = "D:\\b";
    CHECK(job.health.rot_check);
    job.health.rot_check = false;
    CHECK(!jobs::job_from_json(jobs::to_json(job)).health.rot_check);
}
