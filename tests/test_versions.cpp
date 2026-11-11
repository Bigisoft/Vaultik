#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

#include "vaultik/model/snapshot.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/history.hpp"
#include "vaultik/ops/prune.hpp"
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
        path = fs::temp_directory_path() / ("vaultik-versions-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, std::size_t size, std::uint32_t seed) {
    fs::create_directories(p.parent_path());
    std::mt19937 rng(seed);
    std::string data(size, '\0');
    for (char& c : data) c = static_cast<char>(rng());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
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

// The path of a local file inside a snapshot: "C/Users/..." for "C:\Users\...".
std::string snapshot_path(const fs::path& p) {
    std::string s = u8(fs::absolute(p));
    std::replace(s.begin(), s.end(), '\\', '/');
    if (s.size() > 1 && s[1] == ':') s.erase(1, 1);
    while (!s.empty() && s.front() == '/') s.erase(0, 1);
    return s;
}

SnapshotRecord snap_at(std::uint64_t time, const std::string& tag = "job:a") {
    SnapshotRecord e;
    random_bytes(e.id);
    e.snapshot.time = time;
    e.snapshot.hostname = "pc";
    e.snapshot.paths = {"C:\\data"};
    e.snapshot.tags = {tag};
    return e;
}

constexpr std::uint64_t kDay = 864000000000ull;  // FILETIME ticks

}  // namespace

TEST_CASE("pin, note and change counts survive as reserved tags") {
    Snapshot s;
    s.time = 123;
    s.tags = {"job:docs", "daily"};
    s.pinned = true;
    s.note = "before the Windows update";
    s.stats.changes_known = true;
    s.stats.files_new = 3;
    s.stats.files_changed = 2;
    s.stats.items_removed = 1;
    const Snapshot back = parse_snapshot(serialize_snapshot(s));
    CHECK(back.tags == std::vector<std::string>{"job:docs", "daily"});
    CHECK(back.pinned);
    CHECK(back.note == "before the Windows update");
    CHECK(back.stats.changes_known);
    CHECK(back.stats.files_new == 3);
    CHECK(back.stats.files_changed == 2);
    CHECK(back.stats.items_removed == 1);

    Snapshot plain;
    const Snapshot plain_back = parse_snapshot(serialize_snapshot(plain));
    CHECK_FALSE(plain_back.pinned);
    CHECK(plain_back.note.empty());
    CHECK_FALSE(plain_back.stats.changes_known);
}

TEST_CASE("retention keeps pinned and recent snapshots") {
    const std::uint64_t now = os::now();
    std::vector<SnapshotRecord> snaps;
    for (int d = 0; d < 60; d += 5) snaps.push_back(snap_at(now - static_cast<std::uint64_t>(d) * kDay - kDay / 2));
    snaps[10].snapshot.pinned = true;  // 50 days old

    RetentionPolicy p;
    p.within_days = 20;
    const auto d = plan_forget(snaps, p);
    std::size_t kept = 0;
    for (const ForgetDecision& x : d) {
        const std::uint64_t age_days = (now - x.entry.snapshot.time) / kDay;
        if (x.entry.snapshot.pinned) {
            CHECK(x.keep);
            CHECK(x.reason.find("pinned") != std::string::npos);
        } else {
            CHECK(x.keep == (age_days < 20));
        }
        kept += x.keep;
    }
    CHECK(kept == 5);  // 0, 5, 10, 15 days old and the pinned one

    RetentionPolicy size_only;
    size_only.max_bytes = 5ull << 30;
    for (const ForgetDecision& x : plan_forget(snaps, size_only)) CHECK(x.keep);
    CHECK_FALSE(size_only.empty());
    CHECK(size_only.no_rules());
}

TEST_CASE("name patterns") {
    CHECK(name_matches("budget", "Budget 2026.xlsx"));
    CHECK(name_matches("BUDGET*.xlsx", "budget 2026.xlsx"));
    CHECK_FALSE(name_matches("budget*.xlsx", "my budget.xlsx"));
    CHECK(name_matches("*.jp?g", "photo.jpeg"));
    CHECK(name_matches("?.txt", "a.txt"));
    // Case is ignored on Linux too, also outside ASCII.
    CHECK(name_matches("отчет", "Годишен ОТЧЕТ.docx"));
    CHECK(name_matches("ÉTÉ*", "été 2026.jpg"));
    CHECK(name_matches("łódź", "ŁÓDŹ.txt"));
    CHECK_FALSE(name_matches("?.txt", "ab.txt"));
}

TEST_CASE("versions: change counts, history, search, pins and a size limit") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    write_file(src / "a.txt", 5000, 1);
    write_file(src / "same.bin", 300000, 2);
    write_file(src / "gone.txt", 100, 3);
    write_file(src / "sub" / "deep.dat", 2000, 4);
    auto repo = make_repo(tmp.path / "repo");

    BackupOptions o;
    o.paths = {u8(src)};
    o.tags = {"job:t"};
    const BackupResult first = run_backup(*repo, o);
    CHECK(first.snapshot.stats.changes_known);
    CHECK(first.snapshot.stats.files_new == 4);

    write_file(src / "a.txt", 7000, 11);  // changed
    write_file(src / "b.txt", 900, 12);   // new
    fs::remove(src / "gone.txt");         // removed
    const BackupResult second = run_backup(*repo, o);
    CHECK(second.snapshot.stats.files_new == 1);
    CHECK(second.snapshot.stats.files_changed == 1);
    CHECK(second.snapshot.stats.items_removed == 1);

    const std::vector<SnapshotRecord> snaps = load_snapshot_records(*repo);
    REQUIRE(snaps.size() == 2);

    const auto changed = item_history(*repo, snaps, snapshot_path(src / "a.txt"));
    REQUIRE(changed.size() == 2);
    CHECK(changed[0].changed);
    CHECK(changed[1].changed);
    CHECK(changed[0].node.size == 7000);

    const auto same = item_history(*repo, snaps, snapshot_path(src / "same.bin"));
    REQUIRE(same.size() == 2);
    CHECK_FALSE(same[0].changed);  // unchanged in the newer snapshot
    CHECK(same[1].changed);

    const auto gone = item_history(*repo, snaps, snapshot_path(src / "gone.txt"));
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].snapshot == 1);  // only in the older snapshot

    bool truncated = true;
    const auto found = find_items(*repo, snaps, "*.txt", 100, &truncated);
    CHECK_FALSE(truncated);
    REQUIRE(found.size() == 3);  // a.txt, b.txt, gone.txt
    for (const FoundItem& f : found) {
        if (f.path.ends_with("/a.txt")) CHECK(f.snapshots.size() == 2);
        if (f.path.ends_with("/b.txt")) CHECK(f.snapshots == std::vector<std::size_t>{0});
        if (f.path.ends_with("/gone.txt")) CHECK(f.snapshots == std::vector<std::size_t>{1});
    }
    CHECK(find_items(*repo, snaps, "deep", 100).size() == 1);

    // Pin the older one: removing it by ID is refused, and a size limit leaves it alone.
    Snapshot older = snaps[1].snapshot;
    older.pinned = true;
    older.note = "keep me";
    repo->update_snapshot(snaps[1].id, older);
    const auto reloaded = load_snapshot_records(*repo);
    CHECK(reloaded[1].snapshot.pinned);
    CHECK(reloaded[1].snapshot.note == "keep me");
    ForgetOptions fo;
    fo.ids = {snaps[1].id};
    CHECK_THROWS_AS(run_forget(*repo, fo), Error);

    SizePlan plan = plan_size_limit(*repo, reloaded, "job:t", 1);
    CHECK(plan.remove.empty());  // pinned and newest stay
    CHECK_FALSE(plan.fits);

    older.pinned = false;
    repo->update_snapshot(snaps[1].id, older);
    const auto unpinned = load_snapshot_records(*repo);
    plan = plan_size_limit(*repo, unpinned, "job:t", 1);
    REQUIRE(plan.remove.size() == 1);
    CHECK(plan.remove[0].id == snaps[1].id);  // the oldest goes, the newest stays
    CHECK(plan.size_after < plan.size_before);

    plan = plan_size_limit(*repo, unpinned, "job:t", plan.size_before);
    CHECK(plan.remove.empty());
    CHECK(plan.fits);

    // A removed snapshot is never brought back by an update.
    CHECK(remove_snapshots(*repo, {unpinned[1]}) == 1);
    CHECK_THROWS_AS(repo->update_snapshot(snaps[1].id, older), Error);
}
