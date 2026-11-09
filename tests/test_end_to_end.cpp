#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include "vaultik/ops/backup.hpp"
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

TEST_CASE("overlapping backup paths are rejected") {
    TempDir tmp;
    write_file(tmp.path / "a" / "b" / "f.txt", text("x"));
    init_repo(tmp.path / "repo");
    auto repo = open_repo(tmp.path / "repo");
    BackupOptions options;
    options.paths = {u8(tmp.path / "a"), u8(tmp.path / "a" / "b")};
    CHECK_THROWS_AS(run_backup(*repo, options), Error);
}
