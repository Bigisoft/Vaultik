#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>

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

}  // namespace

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
    Bytes content = read_file(victim);
    content[20'000] ^= 0x01;
    write_file(victim, content);
    fs::last_write_time(victim, old);
    // An ordinary change: new content, new time.
    const fs::path edited = src / "dir1" / "f7.bin";
    write_file(edited, seeded_bytes(30'707, 77));
    const auto victim_time = fs::last_write_time(victim);

    r = check_unchanged_files(*repo, backup.snapshot, o);
    CHECK(r.changed == 1);
    CHECK(r.checked == 19);
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
    CHECK(r.changed == 2);
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
