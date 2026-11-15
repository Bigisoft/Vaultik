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

    jobs::CheckState state;
    std::uint64_t now = 1000 * kDay;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state.packs;
    o.time_budget = std::chrono::milliseconds(1);
    // Each pack takes a moment, so the budget runs out after the first one.
    o.on_progress = [](const std::string&) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); };

    std::size_t runs = 0;
    std::uint64_t read_total = 0;
    std::uint64_t left = 1;
    while (left > 0) {
        REQUIRE(runs < packs + 1);
        o.now = now;
        o.fresh_since = jobs::plan_full_check(state, true, now);
        CHECK(o.fresh_since == 1000 * kDay);  // the cycle started with the first one
        const VerifyResult r = run_verify(*repo, o);
        CHECK(r.errors == 0);
        CHECK(r.packs_read >= 1);  // every run gets somewhere
        read_total += r.packs_read;
        left = jobs::finish_full_check(state, *repo, o.fresh_since, now);
        ++runs;
        now += kDay;
    }
    CHECK(runs > 1);              // it did stop at its limit
    CHECK(read_total == packs);   // and no pack was read twice
    CHECK(state.packs.size() == packs);
    CHECK(state.cycle_started == 0);  // the cycle is complete

    // Remembered on disk, per repository and place.
    state.cycle_started = 5;
    jobs::save_check_state(u8(tmp.path), jobs::check_state_key(*repo), state, *repo);
    const jobs::CheckState back = jobs::load_check_state(u8(tmp.path), jobs::check_state_key(*repo));
    CHECK(back.packs == state.packs);
    CHECK(back.cycle_started == 5);
    state.cycle_started = 0;
    CHECK(jobs::packs_unread_since(back.packs, *repo, 1000 * kDay, now) == 0);
    CHECK(jobs::packs_unread_since(back.packs, *repo, now, now) == packs);
    CHECK(jobs::load_check_state(u8(tmp.path), "00ff").packs.empty());

    // The next cycle reads everything again, wherever the reads of this one fell.
    o.time_budget = std::chrono::milliseconds(0);
    o.on_progress = {};
    now += 30 * kDay;
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    CHECK(run_verify(*repo, o).packs_read == packs);
    CHECK(jobs::finish_full_check(state, *repo, o.fresh_since, now) == 0);
}

TEST_CASE("full checks: one the user starts reads every pack, a scheduled cycle covers each pack once") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 6);
    const std::size_t packs = repo->index().packs().size();
    jobs::CheckState state;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state.packs;

    // Everything was read yesterday; the user asks for a full check today: all of it again.
    o.now = 100 * kDay;
    o.fresh_since = jobs::plan_full_check(state, false, o.now);
    CHECK(o.fresh_since == 0);
    CHECK(run_verify(*repo, o).packs_read == packs);
    o.now = 101 * kDay;
    o.fresh_since = jobs::plan_full_check(state, false, o.now);
    CHECK(state.cycle_started == 0);  // no cycle for it
    CHECK(run_verify(*repo, o).packs_read == packs);
    CHECK(jobs::finish_full_check(state, *repo, o.now, o.now) == 0);

    // Scheduled cycles: a pack read late in one cycle is read again in the next one, so no pack
    // goes unread for longer than about one period.
    state = {};
    std::uint64_t now = 200 * kDay;
    int seen = 0;
    const auto stop_after = [&](int n) {
        seen = 0;
        o.should_cancel = [&seen, n] { return ++seen > n; };
    };
    // Cycle 1: half on day 200, the rest on day 225.
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    stop_after(3);
    CHECK_THROWS_AS(run_verify(*repo, o), Cancelled);
    CHECK(jobs::finish_full_check(state, *repo, o.fresh_since, now) == packs - 3);
    now = 225 * kDay;
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    CHECK(o.fresh_since == 200 * kDay);  // the same cycle
    o.should_cancel = {};
    CHECK(run_verify(*repo, o).packs_read == packs - 3);
    CHECK(jobs::finish_full_check(state, *repo, o.fresh_since, now) == 0);
    CHECK(state.cycle_started == 0);
    // Cycle 2 starts on day 230: every pack is read again, also those read on day 225.
    now = 230 * kDay;
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    CHECK(o.fresh_since == now);
    CHECK(run_verify(*repo, o).packs_read == packs);
    for (const auto& [pack, at] : state.packs) CHECK(at == now);
}

TEST_CASE("check state: a copy of the repository in another place starts from scratch") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 3);
    const std::size_t packs = repo->index().packs().size();
    jobs::CheckState state;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state.packs;
    o.now = 10 * kDay;
    run_verify(*repo, o);
    CHECK(state.packs.size() == packs);
    const std::string key = jobs::check_state_key(*repo);
    jobs::save_check_state(u8(tmp.path / "settings"), key, state, *repo);
    // The same repository under another spelling of its folder: the same state.
    CHECK(jobs::check_state_key(*repo, u8(tmp.path / "repo") + "/") == key);

    // Copied to another folder (as onto a new drive): its own state, so its first full check,
    // also a scheduled one, reads every pack there.
    fs::copy(tmp.path / "repo", tmp.path / "copy", fs::copy_options::recursive);
    repo.reset();
    auto copy = Repository::open(std::make_unique<LocalBackend>(u8(tmp.path / "copy")), kPassword);
    const std::string copy_key = jobs::check_state_key(*copy);
    CHECK(copy_key != key);
    jobs::CheckState there = jobs::load_check_state(u8(tmp.path / "settings"), copy_key);
    CHECK(there.packs.empty());
    o.pack_state = &there.packs;
    o.now = 11 * kDay;
    o.fresh_since = jobs::plan_full_check(there, true, o.now);
    CHECK(run_verify(*copy, o).packs_read == packs);
}

TEST_CASE("check state: a time from a clock that was later set back counts as never read") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 3);
    const std::size_t packs = repo->index().packs().size();
    jobs::CheckState state;
    for (const Id& p : repo->index().packs()) state.packs[p] = 900 * kDay;  // "read" in the future
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state.packs;
    o.now = 100 * kDay;
    o.fresh_since = 50 * kDay;
    CHECK(run_verify(*repo, o).packs_read == packs);
    for (const auto& [pack, at] : state.packs) CHECK(at == 100 * kDay);
    // Also for the sampled share: such a pack comes first, and is not left fresh for good.
    for (const Id& p : repo->index().packs()) state.packs[p] = 50 * kDay;
    state.packs[repo->index().packs().back()] = 900 * kDay;
    o.fresh_since = 0;
    o.read_share = 0.01;
    run_verify(*repo, o);
    CHECK(state.packs[repo->index().packs().back()] == 100 * kDay);
    CHECK(jobs::packs_unread_since(state.packs, *repo, 0, 99 * kDay) >= 1);  // 100 is after "now" 99
}

TEST_CASE("resumable check: a cancelled check keeps what it read, a weekly share covers everything over time") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 6);
    const std::size_t packs = repo->index().packs().size();

    jobs::PackTimes state;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state;
    o.fresh_since = 1;
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
    o.fresh_since = 0;
    o.read_share = 0.34;
    for (int week = 0; week < 3; ++week) {
        o.now = (20 + 7 * static_cast<std::uint64_t>(week)) * kDay;
        run_verify(*repo, o);
    }
    CHECK(state.size() == packs);
    CHECK(jobs::packs_unread_since(state, *repo, 11 * kDay, 41 * kDay) == 0);
}

TEST_CASE("check progress: the bytes read so far and in all, reaching the total") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 4);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> seen;
    VerifyOptions o;
    o.read_data = true;
    o.on_bytes = [&](std::uint64_t done, std::uint64_t total) { seen.emplace_back(done, total); };
    const VerifyResult r = run_verify(*repo, o);
    REQUIRE(seen.size() == r.packs_read + 1);  // once before, once per pack
    CHECK(seen.front().first == 0);
    const std::uint64_t total = seen.front().second;
    CHECK(total > 0);
    CHECK(total <= r.bytes_read);  // the stored blobs, without the pack headers
    for (std::size_t i = 1; i < seen.size(); ++i) {
        CHECK(seen[i].first > seen[i - 1].first);
        CHECK(seen[i].second == total);
    }
    CHECK(seen.back().first == total);

    // A sampled check counts only what it reads: the pack the seed picks, and now and then
    // another whose (random) id falls into the 1% share.
    seen.clear();
    o.read_share = 0.01;
    o.sample_seed = 3;
    const VerifyResult some = run_verify(*repo, o);
    CHECK(some.packs_read >= 1);
    CHECK(some.packs_read < r.packs_read);
    REQUIRE(seen.size() == some.packs_read + 1);
    CHECK(seen.back().first == seen.back().second);
    CHECK(seen.back().second < total);
}

TEST_CASE("compression level: kept with the repository, used for new data only, read back by any level") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 1);
    CHECK(repo->compression_level() == 3);  // as made
    const std::size_t packs_before = repo->index().packs().size();

    repo->set_compression_level(12);
    CHECK(repo->compression_level() == 12);
    CHECK_THROWS(repo->set_compression_level(0));
    CHECK_THROWS(repo->set_compression_level(20));
    CHECK(fs::exists(tmp.path / "repo" / "settings" / "compression"));

    // New data at the new level; everything still reads back.
    const fs::path src = tmp.path / "src";
    {
        std::ofstream f(src / "text.txt", std::ios::binary);
        for (int i = 0; i < 20000; ++i) f << "line " << i % 97 << " of some text that compresses well\n";
    }
    BackupOptions o;
    o.paths = {u8(src)};
    run_backup(*repo, o);
    CHECK(repo->index().packs().size() > packs_before);

    // Reopened: the level stays, and the data reads back whole.
    repo = Repository::open(std::make_unique<LocalBackend>(u8(tmp.path / "repo")), kPassword);
    CHECK(repo->compression_level() == 12);
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*repo, vo).errors == 0);

    // Back to the made-with level: the setting goes away, as for a repository that never had one.
    repo->set_compression_level(3);
    CHECK_FALSE(fs::exists(tmp.path / "repo" / "settings" / "compression"));
    repo = Repository::open(std::make_unique<LocalBackend>(u8(tmp.path / "repo")), kPassword);
    CHECK(repo->compression_level() == 3);
}

TEST_CASE("full check: a pack that stays damaged does not keep the round open for good") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 4);
    const std::size_t packs = repo->index().packs().size();
    REQUIRE(packs >= 4);
    // One pack is damaged on the drive (a byte flipped in the middle), and stays so. Flipped with
    // the repository closed (Windows writes no file another handle has open) and checked to take.
    const Id bad = repo->index().packs().front();
    const fs::path file = tmp.path / "repo" / fs::u8path(Repository::pack_name(bad));
    REQUIRE(fs::exists(file));
    const auto flip = [&] {
        repo.reset();
        const auto at = static_cast<std::streamoff>(fs::file_size(file) / 2);
        char before = 0;
        {
            std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);
            REQUIRE(f.is_open());
            f.seekg(at);
            f.get(before);
            f.seekp(at);
            f.put(static_cast<char>(before ^ 0x5A));
            REQUIRE(f.good());
        }
        std::ifstream in(file, std::ios::binary);
        in.seekg(at);
        char after = 0;
        in.get(after);
        REQUIRE(after == static_cast<char>(before ^ 0x5A));
        in.close();
        repo = Repository::open(std::make_unique<LocalBackend>(u8(tmp.path / "repo")), kPassword);
    };
    flip();
    jobs::CheckState state;
    VerifyOptions o;
    o.read_data = true;
    o.pack_state = &state.packs;
    o.damaged_state = &state.damaged;
    o.archived_state = &state.archived;

    // The round's first check reads every pack and finds the damage: the round is complete
    // nonetheless (the damage is alerted), so the next round starts afresh.
    std::uint64_t now = 100 * kDay;
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    VerifyResult r = run_verify(*repo, o);
    CHECK(r.errors > 0);
    CHECK(r.packs_read == packs);
    CHECK(state.damaged.contains(bad));
    CHECK(jobs::finish_full_check(state, *repo, o.fresh_since, now) == 0);
    CHECK(state.cycle_started == 0);
    // Remembered on disk too (older versions read only the rest).
    jobs::save_check_state(u8(tmp.path), "0a0b", state, *repo);
    CHECK(jobs::load_check_state(u8(tmp.path), "0a0b").damaged == state.damaged);

    // A month later: every pack is read again, not just the damaged one and new ones.
    now = 131 * kDay;
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    CHECK(o.fresh_since == now);
    r = run_verify(*repo, o);
    CHECK(r.packs_read == packs);
    CHECK(r.errors > 0);
    CHECK(jobs::finish_full_check(state, *repo, o.fresh_since, now) == 0);

    // Repaired (here: the pack put back as it was): read intact, no longer damaged.
    flip();
    now = 162 * kDay;
    o.now = now;
    o.fresh_since = jobs::plan_full_check(state, true, now);
    r = run_verify(*repo, o);
    CHECK(r.errors == 0);
    CHECK_FALSE(state.damaged.contains(bad));
}

TEST_CASE("check state: the file of an earlier version, named by the repository id alone, is removed") {
    TempDir tmp;
    auto repo = repo_with_packs(tmp.path, 1);
    const auto& id = repo->config().repo_id;
    const fs::path old = tmp.path / "checks" / (to_hex(ByteSpan(id.data(), id.size())) + ".json");
    fs::create_directories(old.parent_path());
    std::ofstream(old) << R"({"packs":{}})";
    jobs::CheckState state;
    const std::string key = jobs::check_state_key(*repo);
    jobs::save_check_state(u8(tmp.path), key, state, *repo);
    CHECK_FALSE(fs::exists(old));
    CHECK(fs::exists(tmp.path / "checks" / (key + ".json")));
}
