#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include "vaultik/jobs/job.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/copy.hpp"
#include "vaultik/ops/diff.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;
namespace fs = std::filesystem;

namespace {

constexpr char kPassword[] = "test password";

// UTF-8 string of a path, and back (std::filesystem uses the ANSI code page for char on Windows).
std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path from_u8(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
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

void append_file(const fs::path& p, const Bytes& data) {
    std::ofstream f(p, std::ios::binary | std::ios::app);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

Bytes text(std::string_view s) {
    return Bytes(s.begin(), s.end());
}

// Relative path -> content, for every regular file below `root` not ending in .tmp.
std::map<std::string, Bytes> snapshot_of_dir(const fs::path& root) {
    std::map<std::string, Bytes> files;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() == ".tmp") continue;
        files[fs::relative(e.path(), root).generic_string()] = read_file(e.path());
    }
    return files;
}

fs::path restored_location(const fs::path& target, const fs::path& source) {
    fs::path p = target;
    for (const std::string& c : os::path_components(u8(source))) p /= from_u8(c);
    return p;
}

std::string snapshot_path_of(const fs::path& source) {
    std::string s;
    for (const std::string& c : os::path_components(u8(source))) s += (s.empty() ? "" : "/") + c;
    return s;
}

void init_repo(const fs::path& dir) {
    LocalBackend backend(u8(dir), true);
    InitOptions options;
    options.kdf_iterations = 1000;  // keep the tests fast
    Repository::init(backend, kPassword, options);
}

std::unique_ptr<Repository> open_repo(const fs::path& dir, const char* password = kPassword) {
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), password);
}

}  // namespace

TEST_CASE("backup, incremental backup, restore and verify") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    const Bytes big = random_data(6u << 20, 11);

    write_file(src / "a.txt", text("hello"));
    write_file(src / "empty.bin", {});
    write_file(src / "big.bin", big);
    write_file(src / "sub" / "dup.bin", big);  // identical content: stored once
    write_file(src / "sub" / "deeper" / from_u8("тест.txt"), text("unicode name"));
    write_file(src / "skip.tmp", text("excluded"));

    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);

    BackupOptions options;
    options.paths = {u8(src)};
    options.excludes = {"*.tmp"};

    const BackupResult first = run_backup(*repo, options);
    const SnapshotStats& s1 = first.snapshot.stats;
    CHECK(s1.files == 5);
    CHECK(s1.errors == 0);
    CHECK(s1.bytes == 2 * big.size() + 5 + 12);
    CHECK(s1.new_bytes < big.size() + 1024);  // dup.bin added nothing
    CHECK_FALSE(first.snapshot.parent.has_value());

    // Change one file and add another.
    append_file(src / "big.bin", random_data(1000, 12));
    write_file(src / "new.txt", text("new file"));

    const BackupResult second = run_backup(*repo, options);
    const SnapshotStats& s2 = second.snapshot.stats;
    REQUIRE(second.snapshot.parent.has_value());
    CHECK(*second.snapshot.parent == first.snapshot_id);
    CHECK(s2.files == 6);
    CHECK(s2.new_bytes < (5u << 20));  // only the changed tail of big.bin plus new.txt

    // Restore with a freshly opened repository, as a later run would.
    repo.reset();
    repo = open_repo(repo_dir);
    CHECK(repo->list_snapshot_ids().size() == 2);

    const fs::path out = tmp.path / "out";
    RestoreOptions ro;
    ro.target = u8(out);
    const RestoreResult rr = run_restore(*repo, repo->load_snapshot(second.snapshot_id), ro);
    CHECK(rr.errors == 0);
    CHECK(rr.files == 6);

    const fs::path restored = restored_location(out, src);
    CHECK(snapshot_of_dir(restored) == snapshot_of_dir(src));
    CHECK_FALSE(fs::exists(restored / "skip.tmp"));
    CHECK(fs::last_write_time(restored / "a.txt") == fs::last_write_time(src / "a.txt"));

    // The first snapshot still restores the old content.
    const fs::path out_old = tmp.path / "out_old";
    RestoreOptions ro_old;
    ro_old.target = u8(out_old);
    ro_old.subpath = snapshot_path_of(src) + "/big.bin";
    CHECK(run_restore(*repo, repo->load_snapshot(first.snapshot_id), ro_old).errors == 0);
    CHECK(read_file(out_old / "big.bin") == big);

    // A second restore into the same place skips existing files unless asked to overwrite.
    const RestoreResult again = run_restore(*repo, repo->load_snapshot(second.snapshot_id), ro);
    CHECK(again.files == 0);
    CHECK(again.skipped >= 6);
    ro.overwrite = true;
    CHECK(run_restore(*repo, repo->load_snapshot(second.snapshot_id), ro).files == 6);

    // Subfolder restore.
    const fs::path out_sub = tmp.path / "out_sub";
    RestoreOptions ro_sub;
    ro_sub.target = u8(out_sub);
    ro_sub.subpath = snapshot_path_of(src) + "/sub";
    CHECK(run_restore(*repo, repo->load_snapshot(second.snapshot_id), ro_sub).errors == 0);
    CHECK(read_file(out_sub / "sub" / "deeper" / from_u8("тест.txt")) == text("unicode name"));

    VerifyOptions vo;
    vo.read_data = true;
    const VerifyResult v = run_verify(*repo, vo);
    CHECK(v.errors == 0);
    CHECK(v.snapshots == 2);
    CHECK(v.packs >= 1);
}

TEST_CASE("wrong password is rejected") {
    TempDir tmp;
    init_repo(tmp.path / "repo");
    CHECK_THROWS_AS(open_repo(tmp.path / "repo", "not the password"), AuthError);
    CHECK_NOTHROW(open_repo(tmp.path / "repo"));
}

TEST_CASE("init refuses an existing repository") {
    TempDir tmp;
    init_repo(tmp.path / "repo");
    CHECK_THROWS_AS(init_repo(tmp.path / "repo"), Error);
}

TEST_CASE("restore never writes through links already in the target") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    const fs::path outside = tmp.path / "outside";
    write_file(src / "sub" / "inner.txt", text("inner"));
    write_file(src / "top.txt", text("top"));
    write_file(outside / "keep.txt", text("must stay"));

    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    const BackupResult backup = run_backup(*repo, options);

    // Prepare the target: the folder "sub" is a link to `outside`, and "top.txt" is a link to a
    // file there. Creating links needs Developer Mode or elevation on Windows.
    const fs::path out = tmp.path / "out";
    const fs::path restored = restored_location(out, src);
    fs::create_directories(restored);
    std::error_code ec;
    fs::create_directory_symlink(outside, restored / "sub", ec);
    if (!ec) fs::create_symlink(outside / "keep.txt", restored / "top.txt", ec);
    if (ec) {
        MESSAGE("skipped: cannot create symbolic links here (" << ec.message() << ")");
        return;
    }

    RestoreOptions ro;
    ro.target = u8(out);
    ro.overwrite = true;
    const RestoreResult rr = run_restore(*repo, backup.snapshot, ro);

    CHECK(rr.errors >= 1);                           // the linked folder is refused
    CHECK_FALSE(fs::exists(outside / "inner.txt"));  // nothing written through it
    CHECK(read_file(outside / "keep.txt") == text("must stay"));
    CHECK_FALSE(fs::is_symlink(restored / "top.txt"));  // the file link was replaced
    CHECK(read_file(restored / "top.txt") == text("top"));
}

TEST_CASE("damaged pack data is detected") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "file.bin", random_data(3u << 20, 21));
    init_repo(repo_dir);
    {
        auto repo = open_repo(repo_dir);
        BackupOptions options;
        options.paths = {u8(src)};
        run_backup(*repo, options);
    }

    // Flip one byte inside the first blob of the first pack.
    fs::path pack;
    for (const auto& e : fs::recursive_directory_iterator(repo_dir / "data"))
        if (e.is_regular_file()) pack = e.path();
    REQUIRE_FALSE(pack.empty());
    {
        std::fstream f(pack, std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(100);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x40);
        f.seekp(100);
        f.write(&c, 1);
    }

    auto repo = open_repo(repo_dir);
    VerifyOptions vo;
    vo.read_data = true;
    const VerifyResult all = run_verify(*repo, vo);
    CHECK(all.errors > 0);
    CHECK(all.packs_read == all.packs);

    // A sampled check reads only its share of the packs, but always at least one.
    vo.read_share = 1e-6;
    vo.sample_seed = 1;
    const VerifyResult some = run_verify(*repo, vo);
    CHECK(some.packs_read == 1);
    CHECK(some.packs_read <= some.packs);

    vo.should_cancel = [] { return true; };
    CHECK_THROWS_AS(run_verify(*repo, vo), Cancelled);

    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    const RestoreResult rr = run_restore(*repo, repo->load_snapshot(repo->list_snapshot_ids().at(0)), ro);
    CHECK(rr.errors > 0);  // the damaged file is reported, not silently restored
}

TEST_CASE("files that look encrypted by ransomware raise the alarm") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    // Documents (compressible text) and photos (random-looking already).
    const auto text = [](int seed) {
        std::string s;
        for (int i = 0; s.size() < 40000; ++i) s += "Line " + std::to_string(i * seed) + " of a quarterly report.\n";
        return Bytes(s.begin(), s.end());
    };
    for (int i = 0; i < 40; ++i) write_file(src / ("doc" + std::to_string(i) + ".txt"), text(i + 1));
    for (int i = 0; i < 40; ++i) write_file(src / ("photo" + std::to_string(i) + ".jpg"), random_data(40000, 100 + i));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    CHECK_FALSE(run_backup(*repo, options).alarm.raised());

    // Edited photos (new random content) and edited documents are ordinary changes.
    for (int i = 0; i < 40; ++i) write_file(src / ("photo" + std::to_string(i) + ".jpg"), random_data(40000, 200 + i));
    for (int i = 0; i < 10; ++i) write_file(src / ("doc" + std::to_string(i) + ".txt"), text(i + 50));
    BackupResult r = run_backup(*repo, options);
    CHECK(r.alarm.became_random == 0);
    CHECK_FALSE(r.alarm.raised());

    // Documents overwritten with random-looking data.
    for (int i = 0; i < 25; ++i) write_file(src / ("doc" + std::to_string(i) + ".txt"), random_data(40000, 300 + i));
    r = run_backup(*repo, options);
    CHECK(r.alarm.became_random == 25);
    CHECK(r.alarm.parent_files == 80);
    CHECK(r.alarm.raised());
    CHECK_FALSE(r.alarm.examples.empty());

    // Files replaced by the same name with an extra extension.
    for (int i = 25; i < 40; ++i) {
        const fs::path from = src / ("doc" + std::to_string(i) + ".txt");
        fs::rename(from, fs::path(from).concat(".locked"));
    }
    for (int i = 0; i < 10; ++i) {
        const fs::path from = src / ("photo" + std::to_string(i) + ".jpg");
        fs::rename(from, fs::path(from).concat(".locked"));
    }
    r = run_backup(*repo, options);
    CHECK(r.alarm.renamed == 25);
    CHECK(r.alarm.raised());

    CHECK(byte_entropy(random_data(65536, 1)) > 7.9);
    CHECK(byte_entropy(text(1)) < 5.0);
}

TEST_CASE("repository files are replaced while another program has them open") {
    TempDir tmp;
    LocalBackend backend(u8(tmp.path / "repo"), true);
    const auto bytes = [](const char* s) { return ByteSpan(reinterpret_cast<const std::uint8_t*>(s), std::strlen(s)); };
    backend.put("snapshots/ab", bytes("first"));
    // Another process reading the file keeps it open (shared for reading and deleting, as Vaultik does).
    os::ErrorCode err = 0;
    os::File reader = os::File::open_random(u8(tmp.path / "repo" / "snapshots" / "ab"), err);
    REQUIRE(reader.valid());
    CHECK_NOTHROW(backend.put("snapshots/ab", bytes("second")));
    const Bytes now = backend.get("snapshots/ab");
    CHECK(std::string(now.begin(), now.end()) == "second");
}

TEST_CASE("a new repository is never created over existing backup data") {
    TempDir tmp;
    init_repo(tmp.path / "repo");
    {
        auto repo = open_repo(tmp.path / "repo");
        BackupOptions options;
        write_file(tmp.path / "src" / "a.txt", random_data(100, 1));
        options.paths = {u8(tmp.path / "src")};
        run_backup(*repo, options);
    }
    fs::remove(tmp.path / "repo" / "config");  // the config lost, the data still there
    LocalBackend backend(u8(tmp.path / "repo"));
    CHECK_THROWS_AS(Repository::init(backend, "another password"), Error);
}

TEST_CASE("a second copy of a backup") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path a_dir = tmp.path / "a";
    const fs::path b_dir = tmp.path / "b";
    write_file(src / "one.bin", random_data(2u << 20, 31));
    write_file(src / "sub" / "two.txt", random_data(1000, 32));
    init_repo(a_dir);
    init_repo(b_dir);  // its own key: everything is stored again under it
    auto a = open_repo(a_dir);
    auto b = open_repo(b_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    options.tags = {"job:docs"};
    run_backup(*a, options);
    write_file(src / "sub" / "two.txt", random_data(1200, 33));
    const BackupResult second = run_backup(*a, options);

    CopyOptions co;
    co.tags = {"job:docs"};
    co.map_file = u8(tmp.path / "copy.map");
    CopyResult r = copy_snapshots(*a, *b, co);
    CHECK(r.copied == 2);
    CHECK(r.present == 0);
    const std::vector<Id> ids = b->list_snapshot_ids();
    REQUIRE(ids.size() == 2);

    // The newest copy restores the same files, knows its original and its parent.
    Snapshot newest;
    for (const Id& id : ids) {
        const Snapshot s = b->load_snapshot(id);
        if (s.copy_of && *s.copy_of == second.snapshot_id) newest = s;
    }
    REQUIRE(newest.copy_of.has_value());
    CHECK(newest.parent.has_value());
    CHECK(newest.tags == std::vector<std::string>{"job:docs"});
    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    run_restore(*b, newest, ro);
    CHECK(snapshot_of_dir(restored_location(tmp.path / "out", src)) == snapshot_of_dir(src));
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*b, vo).errors == 0);

    // Copying again finds both; a new backup copies only what is new.
    r = copy_snapshots(*a, *b, co);
    CHECK(r.copied == 0);
    CHECK(r.present == 2);

    // A pin added to the original later reaches the copy.
    Snapshot pinned = a->load_snapshot(second.snapshot_id);
    pinned.pinned = true;
    pinned.note = "kept";
    a->update_snapshot(second.snapshot_id, pinned);
    r = copy_snapshots(*a, *b, co);
    CHECK(r.updated == 1);
    bool copy_pinned = false;
    for (const Id& id : b->list_snapshot_ids()) {
        const Snapshot s = b->load_snapshot(id);
        if (s.copy_of && *s.copy_of == second.snapshot_id) copy_pinned = s.pinned && s.note == "kept";
    }
    CHECK(copy_pinned);
    CHECK(copy_snapshots(*a, *b, co).updated == 0);
    write_file(src / "three.txt", random_data(500, 34));
    run_backup(*a, options);
    r = copy_snapshots(*a, *b, co);
    CHECK(r.copied == 1);
    // Only three.txt's chunk and the listings of the folders above it (up to the drive), not the
    // 2 MB of one.bin again.
    CHECK(r.bytes < (64u << 10));

    // Other jobs' snapshots are left out.
    options.tags = {"job:other"};
    run_backup(*a, options);
    CHECK(copy_snapshots(*a, *b, co).copied == 0);
}

TEST_CASE("comparing two versions") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "same.txt", random_data(100, 1));
    write_file(src / "edit.txt", random_data(100, 2));
    write_file(src / "gone.txt", random_data(100, 3));
    write_file(src / "old dir" / "x.txt", random_data(100, 4));
    write_file(src / "deep" / "inner" / "y.txt", random_data(100, 5));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    const BackupResult first = run_backup(*repo, options);

    write_file(src / "edit.txt", random_data(120, 6));
    fs::remove(src / "gone.txt");
    fs::remove_all(src / "old dir");
    write_file(src / "new.txt", random_data(100, 7));
    const BackupResult second = run_backup(*repo, options);

    const DiffResult d = diff_snapshots(*repo, first.snapshot, second.snapshot);
    CHECK(d.added == 1);
    CHECK(d.removed == 2);  // gone.txt and the folder "old dir" (one change for the folder)
    CHECK(d.changed == 1);
    REQUIRE(d.changes.size() == 4);
    bool saw_edit = false;
    for (const Change& c : d.changes) {
        if (c.kind == Change::Kind::Changed) {
            saw_edit = c.path.ends_with("/edit.txt") && c.old_size == 100 && c.new_size == 120;
        }
        if (c.path.ends_with("/old dir")) CHECK(c.node_kind == NodeKind::Dir);
    }
    CHECK(saw_edit);
    CHECK(diff_snapshots(*repo, second.snapshot, second.snapshot).changes.empty());
    CHECK(diff_snapshots(*repo, first.snapshot, second.snapshot, 2).truncated);
}

TEST_CASE("ready-made exclusions skip what they say") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    const Bytes one = random_data(100, 7);
    write_file(src / "keep.txt", one);                                      // kept
    write_file(src / "report.tmp", one);                                    // temp
    write_file(src / "~$report.docx", one);                                 // temp (Office lock file)
    write_file(src / "AppData" / "Local" / "Temp" / "t.txt", one);          // temp
    write_file(src / "AppData" / "Local" / "Google" / "Chrome" / "User Data" / "Default" / "Cache" / "c", one);
    write_file(src / "project" / "node_modules" / "lib" / "a.js", one);     // dev
    write_file(src / "project" / "main.js", one);                           // kept
    write_file(src / "Thumbs.db", one);                                     // clutter
    write_file(src / "$Recycle.Bin" / "x", one);                            // recycle
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    for (const jobs::ExclusionPreset& p : jobs::exclusion_presets())
        options.excludes.insert(options.excludes.end(), p.patterns.begin(), p.patterns.end());
    const BackupResult r = run_backup(*repo, options);
    CHECK(r.snapshot.stats.files == 2);  // keep.txt and project/main.js

    // The estimate counts the same files the backup reads.
    const SourceEstimate e = estimate_sources(options.paths, options.excludes);
    CHECK(e.files == 2);
    CHECK(e.bytes == 200);
    CHECK(e.unreadable == 0);
    CHECK(estimate_sources({u8(src)}, {}).files == 9);  // without the exclusions
    CHECK_THROWS_AS(estimate_sources({u8(src)}, {}, [] { return true; }), Cancelled);
}

TEST_CASE("overlapping backup paths are rejected") {
    TempDir tmp;
    write_file(tmp.path / "a" / "b" / "f.txt", text("x"));
    init_repo(tmp.path / "repo");
    auto repo = open_repo(tmp.path / "repo");
    BackupOptions options;
    options.paths = {u8(tmp.path / "a"), u8(tmp.path / "a" / "b")};
    CHECK_THROWS_AS(run_backup(*repo, options), Error);
}
