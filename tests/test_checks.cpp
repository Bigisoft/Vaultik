// Health checks over time: resumable full checks, coverage of the weekly share, what jobs sharing
// a repository share.

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

#include "vaultik/jobs/check_state.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
namespace fs = std::filesystem;

namespace {

constexpr char kPassword[] = "test password";
constexpr std::uint64_t kDay = 864'000'000'000ull;

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-checks-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_random(const fs::path& p, std::size_t n, std::uint32_t seed) {
    fs::create_directories(p.parent_path());
    std::mt19937 rng(seed);
    std::string b(n, '\0');
    for (auto& x : b) x = static_cast<char>(rng());
    std::ofstream(p, std::ios::binary) << b;
}

// A repository with several packs: one backup per new file.
std::unique_ptr<Repository> repo_with_packs(const fs::path& dir, int backups) {
    const fs::path src = dir / "src";
    const fs::path repo_dir = dir / "repo";
    {
        LocalBackend backend(u8(repo_dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, kPassword, options);
    }
    auto repo = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), kPassword);
    BackupOptions o;
    o.paths = {u8(src)};
    for (int i = 0; i < backups; ++i) {
        write_random(src / ("f" + std::to_string(i) + ".bin"), 200u << 10, static_cast<std::uint32_t>(i + 1));
        run_backup(*repo, o);
    }
    return repo;
}

}  // namespace

TEST_CASE("resumable full check: a time limit stops it, the next one goes on, all packs get read") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 5);
    const std::size_t packs = repo->index().packs().size();
    REQUIRE(packs >= 5);

    jobs::PackTimes state;
    std::uint64_t now = 1000 * kDay;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state;
    o.fresh_ticks = 30 * kDay;
    o.time_budget = std::chrono::milliseconds(1);
    // Each pack takes a moment, so the budget runs out after the first one.
    o.on_progress = [](const std::string&) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); };

    std::size_t runs = 0;
    std::uint64_t read_total = 0;
    while (jobs::packs_due(state, *repo, now, 30 * kDay) > 0) {
        REQUIRE(runs < packs + 1);
        o.now = now;
        const VerifyResult r = run_verify(*repo, o);
        CHECK(r.errors == 0);
        CHECK(r.packs_read >= 1);  // every run gets somewhere
        read_total += r.packs_read;
        ++runs;
        now += kDay;
    }
    CHECK(runs > 1);              // it did stop at its limit
    CHECK(read_total == packs);   // and no pack was read twice
    CHECK(state.size() == packs);

    // Remembered on disk, per repository; the next month's check reads everything again.
    jobs::save_check_state(u8(tmp.path), jobs::check_state_key(*repo), state, *repo);
    const jobs::PackTimes back = jobs::load_check_state(u8(tmp.path), jobs::check_state_key(*repo));
    CHECK(back == state);
    CHECK(jobs::packs_due(back, *repo, now, 30 * kDay) == 0);
    CHECK(jobs::packs_due(back, *repo, now + 31 * kDay, 30 * kDay) == packs);
    CHECK(jobs::load_check_state(u8(tmp.path), "00ff").empty());

    // Without a time limit a full check reads only what is due.
    o.time_budget = std::chrono::milliseconds(0);
    o.on_progress = {};
    o.now = now;
    CHECK(run_verify(*repo, o).packs_read == 0);
    o.now = now + 31 * kDay;
    CHECK(run_verify(*repo, o).packs_read == packs);
}

TEST_CASE("resumable check: a cancelled check keeps what it read, a weekly share covers everything over time") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 6);
    const std::size_t packs = repo->index().packs().size();

    jobs::PackTimes state;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state;
    o.fresh_ticks = 30 * kDay;
    o.now = 10 * kDay;
    int seen = 0;
    o.should_cancel = [&] { return ++seen > 3; };
    CHECK_THROWS_AS(run_verify(*repo, o), Cancelled);
    CHECK(state.size() == 3);  // read before it was stopped
    o.should_cancel = {};
    const VerifyResult rest = run_verify(*repo, o);
    CHECK(rest.packs_read == packs - 3);

    // The weekly share: the packs read longest ago, so a third each week covers all in three.
    state.clear();
    o.fresh_ticks = 0;
    o.read_share = 0.34;
    for (int week = 0; week < 3; ++week) {
        o.now = (20 + 7 * static_cast<std::uint64_t>(week)) * kDay;
        run_verify(*repo, o);
    }
    CHECK(state.size() == packs);
    CHECK(jobs::packs_due(state, *repo, 41 * kDay, 30 * kDay) == 0);
}
