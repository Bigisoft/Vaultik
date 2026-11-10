#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/lock.hpp"
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

Bytes random_data(std::size_t n, std::uint32_t seed) {
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

std::unique_ptr<Repository> make_repo(const fs::path& dir) {
    {
        LocalBackend backend(u8(dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, kPassword, options);
    }
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

std::unique_ptr<Repository> reopen(const fs::path& dir) {
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

std::uint64_t stored_pack_bytes(Repository& repo) {
    std::uint64_t total = 0;
    for (const std::string& name : repo.backend().list("data")) total += repo.backend().size(name);
    return total;
}

// Noon UTC on 2026-01-01 plus `days` and `hours`: the same local date in any time zone
// between UTC-11 and UTC+11.
SnapshotRecord snap_at(int days, int hours = 0, const std::string& tag = "job:a") {
    SnapshotRecord e;
    random_bytes(e.id);
    e.snapshot.time = os::from_unix(1767268800LL + days * 86400LL + hours * 3600LL, 0);
    e.snapshot.hostname = "pc";
    e.snapshot.paths = {"C:\\data"};
    e.snapshot.tags = {tag};
    return e;
}

std::size_t kept_count(const std::vector<ForgetDecision>& d) {
    return static_cast<std::size_t>(std::count_if(d.begin(), d.end(), [](const ForgetDecision& x) { return x.keep; }));
}

}  // namespace

TEST_CASE("retention rules keep the newest snapshot per period") {
    std::vector<SnapshotRecord> snaps;
    for (int day = 0; day < 10; ++day) snaps.push_back(snap_at(day));
    snaps.push_back(snap_at(9, -2));  // two more earlier on the last day
    snaps.push_back(snap_at(9, -4));

    RetentionPolicy daily;
    daily.daily = 3;
    auto d = plan_forget(snaps, daily);
    REQUIRE(d.size() == 12);
    CHECK(kept_count(d) == 3);
    CHECK(d[0].keep);  // newest first: the latest snapshot of day 9
    CHECK_FALSE(d[1].keep);  // an earlier one on the same day
    CHECK(d[0].reason == "daily");

    RetentionPolicy last;
    last.last = 2;
    d = plan_forget(snaps, last);
    CHECK(kept_count(d) == 2);
    CHECK(d[0].keep);
    CHECK(d[1].keep);

    RetentionPolicy both;
    both.last = 2;
    both.daily = 3;
    d = plan_forget(snaps, both);
    CHECK(kept_count(d) == 4);  // days 9, 8, 7 plus the second newest of day 9
    CHECK(d[0].reason == "last, daily");

    RetentionPolicy monthly;
    monthly.monthly = 12;
    CHECK(kept_count(plan_forget(snaps, monthly)) == 1);  // all in January 2026

    // Each job is its own group.
    snaps.push_back(snap_at(0, 0, "job:b"));
    CHECK(kept_count(plan_forget(snaps, daily)) == 4);

    CHECK(describe(both) == "last 2, 3 daily");
    CHECK(describe(RetentionPolicy{}) == "keep all");
}

TEST_CASE("repository locks") {
    TempDir tmp;
    auto repo = make_repo(tmp.path / "repo");

    {
        RepoLock a(*repo, RepoLock::Mode::Shared, "backup");
        RepoLock b(*repo, RepoLock::Mode::Shared, "restore");  // shared locks coexist
        CHECK(list_locks(*repo).size() == 2);
        CHECK_THROWS_AS(RepoLock(*repo, RepoLock::Mode::Exclusive, "prune"), LockedError);
    }
    CHECK(list_locks(*repo).empty());  // released by the destructors

    {
        RepoLock x(*repo, RepoLock::Mode::Exclusive, "prune");
        CHECK_THROWS_AS(RepoLock(*repo, RepoLock::Mode::Shared, "backup"), LockedError);
    }

    // A lock not refreshed for longer than the stale time, and one of a process that has ended,
    // are ignored and cleaned up.
    LockInfo old;
    old.hostname = "other-pc";
    old.command = "backup";
    old.time = os::now() - kLockStaleTicks - kTicksPerMinute;
    write_lock(*repo, "locks/" + id_hex(random_id()), old);
    LockInfo dead;
    dead.hostname = os::hostname();
    dead.command = "backup";
    dead.time = os::now();
    dead.pid = 0x7FFFFFF0u;
    write_lock(*repo, "locks/" + id_hex(random_id()), dead);
    CHECK(is_stale(old, os::now()));
    CHECK(is_stale(dead, os::now()));
    {
        RepoLock x(*repo, RepoLock::Mode::Exclusive, "prune");
        CHECK(list_locks(*repo).size() == 1);  // only its own
    }

    // unlock --all removes even a live lock of another computer.
    LockInfo live;
    live.hostname = "other-pc";
    live.command = "backup";
    live.time = os::now();
    write_lock(*repo, "locks/" + id_hex(random_id()), live);
    CHECK(remove_locks(*repo, false) == 0);
    CHECK(remove_locks(*repo, true) == 1);
}

TEST_CASE("forget and prune free the space of removed snapshots") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    const Bytes a = random_data(3u << 20, 1);
    const Bytes b = random_data(3u << 20, 2);
    const Bytes c = random_data(3u << 20, 3);

    write_file(src / "a.bin", a);
    write_file(src / "b.bin", b);
    auto repo = make_repo(repo_dir);
    BackupOptions bo;
    bo.paths = {u8(src)};
    run_backup(*repo, bo);

    fs::remove(src / "b.bin");
    write_file(src / "c.bin", c);
    const BackupResult second = run_backup(*repo, bo);
    const std::uint64_t before = stored_pack_bytes(*repo);

    // With every snapshot still there, prune has nothing to do.
    const PruneResult idle = run_prune(*repo, PruneOptions{});
    CHECK(idle.packs_deleted == 0);
    CHECK(idle.packs_repacked == 0);
    CHECK(stored_pack_bytes(*repo) == before);

    ForgetOptions fo;
    fo.policy.last = 1;
    const ForgetResult fr = run_forget(*repo, fo);
    CHECK(fr.removed == 1);
    CHECK(repo->list_snapshot_ids().size() == 1);

    PruneOptions dry;
    dry.dry_run = true;
    const PruneResult planned = run_prune(*repo, dry);
    CHECK(planned.packs_repacked == 1);  // the first pack: a.bin in use, b.bin not
    CHECK(stored_pack_bytes(*repo) == before);

    const PruneResult pr = run_prune(*repo, PruneOptions{});
    CHECK(pr.packs_repacked == 1);
    CHECK(pr.blobs_unused >= 3);  // the chunks of b.bin
    CHECK(stored_pack_bytes(*repo) < before - (2u << 20));

    repo = reopen(repo_dir);
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*repo, vo).errors == 0);

    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    CHECK(run_restore(*repo, repo->load_snapshot(second.snapshot_id), ro).errors == 0);
    fs::path restored = tmp.path / "out";
    for (const std::string& comp : os::path_components(u8(src))) restored /= fs::path(std::u8string(comp.begin(), comp.end()));
    CHECK(read_file(restored / "a.bin") == a);
    CHECK(read_file(restored / "c.bin") == c);
    CHECK_FALSE(fs::exists(restored / "b.bin"));

    // A second prune finds nothing more to do.
    const PruneResult again = run_prune(*repo, PruneOptions{});
    CHECK(again.packs_deleted == 0);
    CHECK(again.packs_repacked == 0);
}

TEST_CASE("prune keeps data whose index file was lost") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "a.bin", random_data(1u << 20, 4));
    auto repo = make_repo(repo_dir);
    BackupOptions bo;
    bo.paths = {u8(src)};
    run_backup(*repo, bo);

    // Lose every index file: all packs now look like leftovers of cancelled backups.
    for (const std::string& name : repo->list_index_files()) repo->backend().remove(name);
    repo = reopen(repo_dir);
    std::vector<std::string> warnings;
    PruneOptions po;
    po.on_warning = [&](const std::string& w) { warnings.push_back(w); };
    const PruneResult pr = run_prune(*repo, po);
    CHECK(pr.packs_deleted == 0);
    CHECK_FALSE(warnings.empty());

    repo = reopen(repo_dir);
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*repo, vo).errors == 0);
}

TEST_CASE("the newest version always stays, even when every rule has passed it") {
    std::vector<SnapshotRecord> snaps;
    for (int day = 0; day < 5; ++day) snaps.push_back(snap_at(day));  // January 2026, long ago now
    RetentionPolicy recent;
    recent.within_days = 7;
    const auto d = plan_forget(snaps, recent);
    CHECK(kept_count(d) == 1);
    CHECK(d[0].keep);
    CHECK(d[0].reason == "newest");
}
