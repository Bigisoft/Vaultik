#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

#include "vaultik/jobs/job.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/copy.hpp"
#include "vaultik/jobs/profile_items.hpp"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/diff.hpp"
#include "vaultik/ops/move_here.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/repair.hpp"
#include "vaultik/ops/restore_test.hpp"
#include "vaultik/repo/pack.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <windows.h>
#endif

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

// Relative path (UTF-8) -> content, for every regular file below `root` not ending in .tmp. The
// names are UTF-8, not the system code page, which cannot hold every name the tests use (Windows
// with code page 1252, as on GitHub's runners).
std::map<std::string, Bytes> snapshot_of_dir(const fs::path& root) {
    std::map<std::string, Bytes> files;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() == ".tmp") continue;
        const std::u8string name = fs::relative(e.path(), root).generic_u8string();
        files[std::string(name.begin(), name.end())] = read_file(e.path());
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
    const Bytes big = seeded_bytes(6u << 20, 11);

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
    append_file(src / "big.bin", seeded_bytes(1000, 12));
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
    // Times are kept in 100 ns units: on Linux, which keeps nanoseconds, the restored time is the
    // original rounded down to that.
    const auto time_lost = fs::last_write_time(src / "a.txt") - fs::last_write_time(restored / "a.txt");
    CHECK(time_lost >= std::chrono::nanoseconds(0));
    CHECK(time_lost < std::chrono::nanoseconds(100));

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
    write_file(src / "file.bin", seeded_bytes(3u << 20, 21));
    init_repo(repo_dir);
    {
        auto repo = open_repo(repo_dir);
        BackupOptions options;
        options.paths = {u8(src)};
        run_backup(*repo, options);
    }

    // Flip one byte inside the largest blob (file content, not a security descriptor).
    fs::path pack;
    std::uint64_t at = 0;
    {
        auto r = open_repo(repo_dir);
        std::uint32_t largest = 0;
        for (const std::string& name : r->backend().list("data"))
            for (const BlobEntry& e : read_pack_header(r->backend(), name, r->aead()))
                if (e.type == BlobType::Data && e.length > largest) {
                    largest = e.length;
                    pack = repo_dir / from_u8(name);
                    at = e.offset + e.length / 2;
                }
    }
    REQUIRE_FALSE(pack.empty());
    {
        std::fstream f(pack, std::ios::binary | std::ios::in | std::ios::out);
        f.seekg(static_cast<std::streamoff>(at));
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x40);
        f.seekp(static_cast<std::streamoff>(at));
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

TEST_CASE("a pack damaged by bad sectors is rebuilt from its repair data") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "file.bin", seeded_bytes(3u << 20, 31));
    write_file(src / "small.txt", text("small"));
    init_repo(repo_dir);
    {
        auto repo = open_repo(repo_dir);
        repo->set_parity_percent(5);
        BackupOptions options;
        options.paths = {u8(src)};
        run_backup(*repo, options);
    }
    // Every pack got repair data; the setting is the repository's.
    fs::path largest;
    Id largest_id{};
    {
        auto r = open_repo(repo_dir);
        CHECK(r->parity_percent() == 5);
        for (const std::string& name : r->backend().list("data")) {
            const Id id = id_from_hex(name.substr(name.find_last_of('/') + 1));
            CHECK(r->backend().exists(Repository::parity_name(id)));
            if (largest.empty() || fs::file_size(repo_dir / from_u8(name)) > fs::file_size(largest)) {
                largest = repo_dir / from_u8(name);
                largest_id = id;
            }
        }
    }
    REQUIRE_FALSE(largest.empty());
    const auto check_restore = [&] {
        auto repo = open_repo(repo_dir);
        VerifyOptions vo;
        vo.read_data = true;
        CHECK(run_verify(*repo, vo).errors == 0);
        RestoreOptions ro;
        ro.target = u8(tmp.path / "out");
        fs::remove_all(tmp.path / "out");
        const RestoreResult rr = run_restore(*repo, repo->load_snapshot(repo->list_snapshot_ids().at(0)), ro);
        CHECK(rr.errors == 0);
        CHECK(snapshot_of_dir(restored_location(tmp.path / "out", src)) == snapshot_of_dir(src));
    };

    // 20 KiB of zeros in the middle, as bad sectors leave it.
    {
        std::fstream f(largest, std::ios::binary | std::ios::in | std::ios::out);
        f.seekp(static_cast<std::streamoff>(fs::file_size(largest) / 2));
        const std::string zeros(20 * 1024, '\0');
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    {
        auto repo = open_repo(repo_dir);
        VerifyOptions vo;
        vo.read_data = true;
        const VerifyResult v = run_verify(*repo, vo);
        CHECK(v.errors > 0);
        CHECK(v.packs_repairable == 1);
        const RepairResult rr = run_repair(*repo, RepairOptions{});
        CHECK(rr.packs_rebuilt == 1);
        CHECK(rr.packs_unreadable == 0);
        CHECK(rr.blobs_damaged == 0);
        CHECK(rr.snapshots_repaired == 0);
    }
    check_restore();

    // Cut short by 30 KiB: the pack's own header (at its end) is gone too.
    fs::resize_file(largest, fs::file_size(largest) - 30 * 1024);
    {
        auto repo = open_repo(repo_dir);
        RepairOptions quick;
        quick.quick = true;
        const RepairResult rr = run_repair(*repo, quick);
        CHECK(rr.packs_rebuilt == 1);
        CHECK(rr.packs_unreadable == 0);
    }
    check_restore();

    // A health check gives a pack without repair data its repair data.
    {
        auto repo = open_repo(repo_dir);
        repo->backend().remove(Repository::parity_name(largest_id));
        VerifyOptions vo;
        vo.read_data = true;
        vo.add_parity = repo->parity_percent();
        const VerifyResult v = run_verify(*repo, vo);
        CHECK(v.errors == 0);
        CHECK(v.parity_added == 1);
        CHECK(repo->backend().exists(Repository::parity_name(largest_id)));
    }
}

TEST_CASE("move to this PC finds an old account's folders") {
    TempDir tmp;
    const fs::path repo_dir = tmp.path / "repo";
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    // A backup made on another computer: C:\Users\anna with Documents (holding a file and
    // Outlook Files), Chrome's profile and Public, and D:\Data.
    const auto dir = [&](const std::string& name, std::vector<Node> children) {
        Tree t;
        t.nodes = std::move(children);
        t.sort();
        Node n;
        n.name = name;
        n.kind = NodeKind::Dir;
        const Bytes b = serialize_tree(t);
        repo->save_blob(BlobType::Tree, b, n.subtree);
        return n;
    };
    Node letter;
    letter.name = "letter.txt";
    letter.kind = NodeKind::File;
    const Bytes content = text("dear anna");
    letter.size = content.size();
    Id chunk{};
    repo->save_blob(BlobType::Data, content, chunk);
    letter.content = {chunk};
    const Node c = dir("C", {dir("Users", {dir("anna", {dir("Documents", {letter, dir("Outlook Files", {})}),
                                                       dir("AppData", {dir("Local", {dir("Google", {dir("Chrome", {
                                                           dir("User Data", {})})})})})}),
                                          dir("Public", {dir("Documents", {})})})});
    const Node d = dir("D", {dir("Data", {})});
    Tree root;
    root.nodes = {c, d};
    root.sort();
    Snapshot s;
    s.time = os::now();
    s.hostname = "old-pc";
    repo->save_blob(BlobType::Tree, serialize_tree(root), s.tree);
    repo->flush();
    repo->save_snapshot(s);

    const std::vector<MoveItem> plan = plan_move(*repo, s);
    CHECK(plan_accounts(plan) == std::vector<std::string>{"anna"});  // Public is nobody's own
    std::vector<std::string> ids;
    for (const MoveItem& m : plan) ids.push_back(m.id);
    // Outlook Files lies inside Documents and comes along with it.
    CHECK(ids == std::vector<std::string>{"documents", "chrome"});
    REQUIRE(plan.size() == 2);
    CHECK(plan[0].from == "C/Users/anna/Documents");
    CHECK(plan[0].to == os::user_folder(os::UserFolder::Documents));
    CHECK(plan[1].from == "C/Users/anna/AppData/Local/Google/Chrome/User Data");

    // Its content goes into the folder chosen, not a folder of the same name inside it.
    RestoreOptions ro;
    ro.target = u8(tmp.path / "here");
    ro.subpath = plan[0].from;
    ro.contents_only = true;
    const RestoreResult r = run_restore(*repo, s, ro);
    CHECK(r.errors == 0);
    CHECK(read_file(tmp.path / "here" / "letter.txt") == content);
    CHECK(fs::is_directory(tmp.path / "here" / "Outlook Files"));
}

TEST_CASE("folders the change journal calls unchanged are taken from the parent") {
    // With one reader and with several, which list folders ahead but never one the journal may
    // let the backup reuse.
    for (const unsigned readers : {0u, 4u}) {
        CAPTURE(readers);
        TempDir tmp;
        const fs::path src = tmp.path / "src";
        const fs::path repo_dir = tmp.path / "repo";
        write_file(src / "a" / "one.txt", text("one"));
        write_file(src / "b" / "two.txt", text("two"));
        write_file(src / "b" / "deeper" / "three.txt", text("three"));
        init_repo(repo_dir);
        auto repo = open_repo(repo_dir);
        BackupOptions options;
        options.paths = {u8(src)};
        options.read_threads = readers;
        const BackupResult first = run_backup(*repo, options);
        CHECK(first.dirs_reused == 0);
        const auto restored = [&](const Id& snapshot, const std::string& name) {
            const fs::path out = tmp.path / ("out-" + name);
            RestoreOptions ro;
            ro.target = u8(out);
            run_restore(*repo, repo->load_snapshot(snapshot), ro);
            return snapshot_of_dir(restored_location(out, src));
        };

        // Changed behind the journal's back: a journal that calls every folder unchanged keeps the
        // parent's listing, which shows the folders were not read.
        write_file(src / "b" / "two.txt", text("TWO, longer"));
        options.journal_parent = first.snapshot_id;
        options.dir_unchanged = [](const std::string&) { return true; };
        const BackupResult second = run_backup(*repo, options);
        CHECK(second.dirs_reused >= 3);
        CHECK(second.snapshot.stats.files == 3);
        CHECK(restored(second.snapshot_id, "2")["b/two.txt"] == text("two"));

        // A folder the journal names is read again, with the folders below it asked in turn.
        options.journal_parent = second.snapshot_id;
        options.dir_unchanged = [](const std::string& dir) { return !(dir.ends_with("\\b") || dir.ends_with("/b")); };
        const BackupResult third = run_backup(*repo, options);
        CHECK(third.dirs_reused >= 1);
        const auto files = restored(third.snapshot_id, "3");
        CHECK(files.at("b/two.txt") == text("TWO, longer"));
        CHECK(files.at("b/deeper/three.txt") == text("three"));
        CHECK(files.at("a/one.txt") == text("one"));

        // A journal read against another version than the parent answers nothing.
        options.journal_parent = first.snapshot_id;
        options.dir_unchanged = [](const std::string&) { return true; };
        CHECK(run_backup(*repo, options).dirs_reused == 0);
    }
}

TEST_CASE("the change journal is not trusted inside a folder that was renamed") {
    // Two folders swap names: the journal has records for the two folders and the folder they are
    // in, none for what is inside them, so proj\code looks unchanged. It is another folder now.
    for (const unsigned readers : {0u, 4u}) {
        CAPTURE(readers);
        TempDir tmp;
        const fs::path src = tmp.path / "top";
        const fs::path repo_dir = tmp.path / "repo";
        write_file(src / "proj" / "code" / "x.txt", text("old project"));
        // Another length: two files of one size written in the same instant can share their times,
        // and size and times are all a listing compares.
        write_file(src / "proj_new" / "code" / "x.txt", text("the new project"));
        init_repo(repo_dir);
        auto repo = open_repo(repo_dir);
        BackupOptions options;
        options.paths = {u8(src)};
        options.read_threads = readers;
        const BackupResult first = run_backup(*repo, options);

        fs::rename(src / "proj", src / "proj_old");
        fs::rename(src / "proj_new", src / "proj");
        const auto named = [](const std::string& dir, const std::string& name) {
            return dir.ends_with("\\" + name) || dir.ends_with("/" + name);
        };
        // The journal: records for both folders (renamed) and for the folder they are in.
        const auto moved = [named](const std::string& dir) { return named(dir, "proj") || named(dir, "proj_old"); };
        options.journal_parent = first.snapshot_id;
        options.dir_unchanged = [named, moved](const std::string& dir) { return !named(dir, "top") && !moved(dir); };
        options.dir_moved = moved;
        const BackupResult second = run_backup(*repo, options);

        const fs::path out = tmp.path / "out";
        RestoreOptions ro;
        ro.target = u8(out);
        run_restore(*repo, repo->load_snapshot(second.snapshot_id), ro);
        const auto files = snapshot_of_dir(restored_location(out, src));
        CHECK(files.at("proj/code/x.txt") == text("the new project"));
        CHECK(files.at("proj_old/code/x.txt") == text("old project"));
    }
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
    for (int i = 0; i < 40; ++i) write_file(src / ("photo" + std::to_string(i) + ".jpg"), seeded_bytes(40000, 100 + i));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    CHECK_FALSE(run_backup(*repo, options).alarm.raised());

    // Edited photos (new random content) and edited documents are ordinary changes.
    for (int i = 0; i < 40; ++i) write_file(src / ("photo" + std::to_string(i) + ".jpg"), seeded_bytes(40000, 200 + i));
    for (int i = 0; i < 10; ++i) write_file(src / ("doc" + std::to_string(i) + ".txt"), text(i + 50));
    BackupResult r = run_backup(*repo, options);
    CHECK(r.alarm.became_random == 0);
    CHECK_FALSE(r.alarm.raised());

    // Documents overwritten with random-looking data.
    for (int i = 0; i < 25; ++i) write_file(src / ("doc" + std::to_string(i) + ".txt"), seeded_bytes(40000, 300 + i));
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

    CHECK(byte_entropy(seeded_bytes(65536, 1)) > 7.9);
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
        write_file(tmp.path / "src" / "a.txt", seeded_bytes(100, 1));
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
    write_file(src / "one.bin", seeded_bytes(2u << 20, 31));
    write_file(src / "sub" / "two.txt", seeded_bytes(1000, 32));
    init_repo(a_dir);
    init_repo(b_dir);  // its own key: everything is stored again under it
    auto a = open_repo(a_dir);
    auto b = open_repo(b_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    options.tags = {"job:docs"};
    run_backup(*a, options);
    write_file(src / "sub" / "two.txt", seeded_bytes(1200, 33));
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
    write_file(src / "three.txt", seeded_bytes(500, 34));
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

TEST_CASE("a second copy does not trust a folder listing a prune of the target left without its data") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path a_dir = tmp.path / "a";
    const fs::path b_dir = tmp.path / "b";
    const Bytes c = seeded_bytes(300000, 51);
    // V1 stores f.txt's data; V2 moves it to another folder (its listing new, the data not) and
    // adds keep.txt; V3 only keeps keep.txt, so V2's pack stays when V1 and V2 are gone.
    write_file(src / "one" / "f.txt", c);
    init_repo(a_dir);
    init_repo(b_dir);
    auto a = open_repo(a_dir);
    auto b = open_repo(b_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    options.tags = {"job:docs"};
    const BackupResult v1 = run_backup(*a, options);
    fs::remove_all(src / "one");
    write_file(src / "two" / "f.txt", c);
    write_file(src / "keep.txt", seeded_bytes(300000, 52));
    const BackupResult v2 = run_backup(*a, options);
    fs::remove_all(src / "two");
    run_backup(*a, options);

    CopyOptions co;
    co.tags = {"job:docs"};
    co.map_file = u8(tmp.path / "copy.map");
    REQUIRE(copy_snapshots(*a, *b, co).copied == 3);

    // In the copy, V1 and V2 go and a prune drops the data of f.txt (V1's pack is mostly unused)
    // but keeps V2's pack (mostly keep.txt), with V2's folder listings in it.
    for (const Id& id : b->list_snapshot_ids()) {
        const Snapshot s = b->load_snapshot(id);
        if (s.copy_of && (*s.copy_of == v1.snapshot_id || *s.copy_of == v2.snapshot_id))
            b->backend().remove(Repository::snapshot_name(id));
    }
    const PruneResult pr = run_prune(*b, PruneOptions{});
    REQUIRE(pr.packs_deleted + pr.packs_repacked >= 1);
    b = open_repo(b_dir);

    // V1 is gone from the original too; copying again brings V2 back, with its data.
    a->backend().remove(Repository::snapshot_name(v1.snapshot_id));
    CHECK(copy_snapshots(*a, *b, co).copied == 1);
    b = open_repo(b_dir);
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*b, vo).errors == 0);
}

TEST_CASE("comparing two versions") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "same.txt", seeded_bytes(100, 1));
    write_file(src / "edit.txt", seeded_bytes(100, 2));
    write_file(src / "gone.txt", seeded_bytes(100, 3));
    write_file(src / "old dir" / "x.txt", seeded_bytes(100, 4));
    write_file(src / "deep" / "inner" / "y.txt", seeded_bytes(100, 5));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    const BackupResult first = run_backup(*repo, options);

    write_file(src / "edit.txt", seeded_bytes(120, 6));
    fs::remove(src / "gone.txt");
    fs::remove_all(src / "old dir");
    write_file(src / "new.txt", seeded_bytes(100, 7));
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
    const Bytes one = seeded_bytes(100, 7);
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

TEST_CASE("a test restore brings files back and notices one that differs from the original") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    const fs::path work = tmp.path / "work";
    for (int i = 0; i < 30; ++i)
        write_file(src / ("dir" + std::to_string(i % 3)) / ("f" + std::to_string(i) + ".bin"),
                   seeded_bytes(1000 + i * 37, 100 + i));
    fs::create_directories(work);
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    const BackupResult backup = run_backup(*repo, options);

    RestoreTestOptions o;
    o.work_dir = u8(work);
    o.max_files = 10;
    o.seed = 7;
    RestoreTestResult r = test_restore(*repo, backup.snapshot, o);
    CHECK(r.candidates == 30);
    CHECK(r.files == 10);
    CHECK(r.compared == 10);
    CHECK(r.problems.empty());
    CHECK(fs::is_empty(work));  // the temporary folder is gone

    // Change one file's bytes but keep its size and time, as silent damage would.
    const fs::path victim = src / "dir1" / "f4.bin";
    const auto when = fs::last_write_time(victim);
    Bytes changed = read_file(victim);
    changed[10] ^= 0xFF;
    write_file(victim, changed);
    fs::last_write_time(victim, when);
    o.max_files = 100;
    r = test_restore(*repo, backup.snapshot, o);
    CHECK(r.files == 30);
    REQUIRE(r.problems.size() == 1);
    CHECK(r.problems[0].find("f4.bin") != std::string::npos);
    CHECK(r.problems[0].find("differs") != std::string::npos);

    // A size budget keeps the restore small.
    o.max_bytes = 3000;
    r = test_restore(*repo, backup.snapshot, o);
    CHECK(r.bytes <= 3000);
}

TEST_CASE("snapshot paths map back to their place on this computer") {
#ifdef _WIN32
    CHECK(original_location("C/Users/me/a.txt") == "C:\\Users\\me\\a.txt");
    CHECK(original_location("UNC/nas/share/x/y") == "\\\\nas\\share\\x\\y");
    CHECK(original_location("UNC/nas").empty());
#else
    CHECK(original_location("home/me/a.txt") == "/home/me/a.txt");
#endif
    CHECK(original_location("").empty());
}

TEST_CASE("repair rebuilds a damaged index from the packs") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "a.txt", text("alpha"));
    write_file(src / "b.bin", seeded_bytes(300000, 3));
    init_repo(repo_dir);
    {
        auto repo = open_repo(repo_dir);
        BackupOptions options;
        options.paths = {u8(src)};
        run_backup(*repo, options);
    }
    // Garble every index file: a normal open now fails.
    for (const auto& e : fs::recursive_directory_iterator(repo_dir / "index"))
        if (e.is_regular_file()) write_file(e.path(), seeded_bytes(200, 9));
    CHECK_THROWS(open_repo(repo_dir));

    auto bare = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), kPassword, false);
    const RepairResult r = run_repair(*bare, {});
    CHECK(r.blobs_damaged == 0);
    CHECK(r.snapshots == 1);
    CHECK(r.snapshots_repaired == 0);
    CHECK(r.index_files_replaced >= 1);
    bare.reset();

    auto repo = open_repo(repo_dir);
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*repo, vo).errors == 0);
    const fs::path out = tmp.path / "out";
    RestoreOptions ro;
    ro.target = u8(out);
    run_restore(*repo, repo->load_snapshot(repo->list_snapshot_ids().at(0)), ro);
    CHECK(read_file(restored_location(out, src / "a.txt")) == text("alpha"));
}

TEST_CASE("repair saves a version again without a file whose data is damaged") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "small.txt", text("still here"));
    write_file(src / "big.bin", seeded_bytes(400000, 4));
    init_repo(repo_dir);
    {
        auto repo = open_repo(repo_dir);
        BackupOptions options;
        options.paths = {u8(src)};
        run_backup(*repo, options);
    }
    // Flip a byte inside the big file's data in its pack.
    {
        auto repo = open_repo(repo_dir);
        bool done = false;
        for (const std::string& name : repo->backend().list("data")) {
            for (const BlobEntry& e : read_pack_header(repo->backend(), name, repo->aead())) {
                if (e.type != BlobType::Data || e.raw_len < 100000 || done) continue;
                Bytes pack = repo->backend().get(name);
                pack[e.offset + e.length / 2] ^= 0x55;
                repo->backend().put(name, pack);
                done = true;
            }
        }
        REQUIRE(done);
    }

    RepairOptions dry;
    dry.dry_run = true;
    {
        auto bare = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), kPassword, false);
        const RepairResult r = run_repair(*bare, dry);
        CHECK(r.blobs_damaged == 1);
        CHECK(r.snapshots_repaired == 1);
    }
    auto bare = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), kPassword, false);
    const RepairResult r = run_repair(*bare, {});
    CHECK(r.blobs_damaged == 1);
    CHECK(r.snapshots_repaired == 1);
    CHECK(r.files_lost == 1);
    CHECK(r.snapshots_removed == 0);
    bare.reset();

    auto repo = open_repo(repo_dir);
    const std::vector<Id> ids = repo->list_snapshot_ids();
    REQUIRE(ids.size() == 1);
    const Snapshot s = repo->load_snapshot(ids[0]);
    CHECK(s.note.find("repaired") != std::string::npos);
    const fs::path out = tmp.path / "out";
    RestoreOptions ro;
    ro.target = u8(out);
    CHECK(run_restore(*repo, s, ro).errors == 0);
    CHECK(read_file(restored_location(out, src / "small.txt")) == text("still here"));
    CHECK_FALSE(fs::exists(restored_location(out, src / "big.bin")));
}

TEST_CASE("a backup with worker threads and background uploads stores the same data") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    for (int i = 0; i < 12; ++i) write_file(src / ("f" + std::to_string(i) + ".bin"), seeded_bytes(900000 + i * 1000, 50 + i));
    write_file(src / "dup.bin", seeded_bytes(900000, 50));  // the same as f0: stored once
    write_file(src / "text.txt", text(std::string(200000, 'a')));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    repo->set_parallelism(3, 2);
    BackupOptions options;
    options.paths = {u8(src)};
    const BackupResult b = run_backup(*repo, options);
    CHECK(b.snapshot.stats.files == 14);
    repo->set_parallelism(0, 0);
    repo.reset();

    auto again = open_repo(repo_dir);
    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*again, vo).errors == 0);
    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    CHECK(run_restore(*again, b.snapshot, ro).errors == 0);
    CHECK(snapshot_of_dir(restored_location(tmp.path / "out", src)) == snapshot_of_dir(src));

    // A second backup with the pipeline finds everything already stored.
    again->set_parallelism(2, 1);
    const BackupResult c = run_backup(*again, options);
    CHECK(c.snapshot.stats.new_bytes == 0);
}

namespace {

// Writes packs until `allowed` of them were written, then fails every further pack upload.
class FailingPacks final : public Backend {
public:
    FailingPacks(const fs::path& dir, int allowed) : inner_(u8(dir)), allowed_(allowed) {}
    std::string location() const override { return inner_.location(); }
    bool exists(const std::string& name) override { return inner_.exists(name); }
    void put(const std::string& name, ByteSpan data) override {
        if (name.starts_with("data/") && allowed_.fetch_sub(1) <= 0) throw Error("simulated network failure");
        inner_.put(name, data);
    }
    Bytes get(const std::string& name) override { return inner_.get(name); }
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override {
        return inner_.get_range(name, offset, length);
    }
    std::uint64_t size(const std::string& name) override { return inner_.size(name); }
    std::vector<std::string> list(const std::string& dir) override { return inner_.list(dir); }
    void remove(const std::string& name) override { inner_.remove(name); }

private:
    LocalBackend inner_;
    std::atomic<int> allowed_;
};

}  // namespace

TEST_CASE("a failed background upload fails the backup and saves no version") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    // Incompressible data, enough for several 32 MiB packs.
    for (int i = 0; i < 6; ++i) write_file(src / ("f" + std::to_string(i) + ".bin"), seeded_bytes(20u << 20, 70 + i));
    init_repo(repo_dir);
    auto repo = Repository::open(std::make_unique<FailingPacks>(repo_dir, 1), kPassword);
    repo->set_parallelism(2, 1);
    BackupOptions options;
    options.paths = {u8(src)};
    CHECK_THROWS(run_backup(*repo, options));
    CHECK(repo->list_snapshot_ids().empty());
    CHECK_THROWS(repo->flush());  // still failed: nothing written later can count as stored
}

TEST_CASE("only when something changed: an unchanged backup saves no version") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "a.txt", text("one"));
    write_file(src / "sub" / "b.txt", text("two"));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions options;
    options.paths = {u8(src)};
    options.skip_unchanged = true;
    const BackupResult first = run_backup(*repo, options);  // no parent: always saved
    CHECK_FALSE(first.unchanged);
    const BackupResult second = run_backup(*repo, options);
    CHECK(second.unchanged);
    CHECK(repo->list_snapshot_ids().size() == 1);
    append_file(src / "sub" / "b.txt", text(" more"));
    const BackupResult third = run_backup(*repo, options);
    CHECK_FALSE(third.unchanged);
    CHECK(repo->list_snapshot_ids().size() == 2);
    options.skip_unchanged = false;  // without the option every run saves one
    CHECK_FALSE(run_backup(*repo, options).unchanged);
    CHECK(repo->list_snapshot_ids().size() == 3);
}

namespace {

// A tree of many files of mixed sizes: empty, tiny, one chunk, several chunks, and duplicates.
void make_mixed_tree(const fs::path& src) {
    std::mt19937 rng(77);
    for (int d = 0; d < 6; ++d) {
        const fs::path dir = src / ("dir" + std::to_string(d));
        for (int i = 0; i < 30; ++i) {
            std::size_t size = 0;
            switch (i % 6) {
                case 0: size = 0; break;
                case 1: size = 1 + rng() % 100; break;
                case 2: size = 4096 + rng() % 60000; break;
                case 3: size = 200 * 1024; break;  // below the minimum chunk size: one chunk
                case 4: size = rng() % 300000; break;
                default: size = (i == 5 && d % 2 == 0) ? (3u << 20) + rng() % (2u << 20) : 1000; break;
            }
            write_file(dir / ("f" + std::to_string(i) + ".bin"), seeded_bytes(size, rng()));
        }
        write_file(dir / "sub" / "deep" / "x.txt", text("folder " + std::to_string(d)));
    }
    write_file(src / "multi.bin", seeded_bytes(9u << 20, 5));
    write_file(src / "dir1" / "copy-of-multi.bin", seeded_bytes(9u << 20, 5));  // stored once
    write_file(src / "empty.bin", {});
    write_file(src / "gone.bin", text("read_path sends this one nowhere"));
    write_file(src / "locked.bin", seeded_bytes(70000, 6));
}

}  // namespace

TEST_CASE("several readers make the same snapshot as one") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    make_mixed_tree(src);

#ifdef _WIN32
    // Held open without sharing, so the backup cannot open it (the same for every reader count).
    // With administrator rights (as on GitHub's runners) the backup privilege opens it anyway.
    int locked_fd = -1;
    REQUIRE(_wsopen_s(&locked_fd, (src / "locked.bin").c_str(), _O_RDONLY | _O_BINARY, _SH_DENYRW, 0) == 0);
    struct CloseFd {
        int fd;
        ~CloseFd() { _close(fd); }
    } close_locked{locked_fd};
    const bool locked_unreadable = !os::process_elevated();
    const std::uint64_t expected_errors = locked_unreadable ? 2 : 1;
#else
    const std::uint64_t expected_errors = 1;
#endif

    const std::string gone = u8(src / "gone.bin");
    const auto make_options = [&](unsigned readers) {
        BackupOptions o;
        o.paths = {u8(src)};
        o.read_threads = readers;
        o.use_parent = false;
        // A file that disappears between the listing and its reading.
        o.read_path = [gone](const std::string& p) { return p == gone ? p + ".missing" : p; };
        return o;
    };

    init_repo(tmp.path / "one");
    init_repo(tmp.path / "four");
    auto one = open_repo(tmp.path / "one");
    auto four = open_repo(tmp.path / "four");
    four->set_parallelism(2, 0);

    std::vector<std::string> warnings_one, warnings_four;
    BackupOptions o1 = make_options(1);
    o1.on_warning = [&](const std::string& w) { warnings_one.push_back(w); };
    BackupOptions o4 = make_options(4);
    o4.on_warning = [&](const std::string& w) { warnings_four.push_back(w); };
    const BackupResult r1 = run_backup(*one, o1);
    const BackupResult r4 = run_backup(*four, o4);

    const SnapshotStats& a = r1.snapshot.stats;
    const SnapshotStats& b = r4.snapshot.stats;
    CHECK(a.errors == expected_errors);
    CHECK(a.files == b.files);
    CHECK(a.dirs == b.dirs);
    CHECK(a.bytes == b.bytes);
    CHECK(a.new_bytes == b.new_bytes);
    CHECK(a.errors == b.errors);
    CHECK(a.skipped == b.skipped);
    CHECK(a.files_new == b.files_new);
    CHECK(a.files_changed == b.files_changed);
    std::sort(warnings_one.begin(), warnings_one.end());
    std::sort(warnings_four.begin(), warnings_four.end());
    CHECK(warnings_one == warnings_four);
    CHECK(a.new_bytes < a.bytes);  // the copy of multi.bin added nothing

    // In the same repository (same keys) four readers make the very same root tree. Compared with
    // a second run, not the first: NTFS updates a folder's modification time lazily, so the
    // first listing after the files were written can still see older folder times.
    const BackupResult same = run_backup(*one, make_options(1));
    const BackupResult again = run_backup(*one, make_options(4));
    CHECK(again.snapshot.tree == same.snapshot.tree);
    CHECK(again.snapshot.stats.new_bytes == 0);
    CHECK(again.snapshot.stats.bytes == a.bytes);

    // With a parent: a changed, an added and a removed file.
    append_file(src / "dir2" / "f5.bin", seeded_bytes(5000, 9));
    write_file(src / "dir3" / "new.bin", seeded_bytes(300000, 10));
    fs::remove(src / "dir4" / "f3.bin");
    BackupOptions p1 = make_options(1);
    p1.use_parent = true;
    BackupOptions p4 = make_options(4);
    p4.use_parent = true;
    const BackupResult i1 = run_backup(*one, p1);
    const BackupResult i4 = run_backup(*four, p4);
    CHECK(i1.snapshot.stats.files_new == 1);
    CHECK(i1.snapshot.stats.files_changed == 1);
    CHECK(i1.snapshot.stats.files_new == i4.snapshot.stats.files_new);
    CHECK(i1.snapshot.stats.files_changed == i4.snapshot.stats.files_changed);
    CHECK(i1.snapshot.stats.items_removed == i4.snapshot.stats.items_removed);
    CHECK(i1.snapshot.stats.new_bytes == i4.snapshot.stats.new_bytes);
    CHECK(i1.snapshot.stats.bytes == i4.snapshot.stats.bytes);

    // What four readers stored restores to the source (less the files that could not be read).
    const fs::path out = tmp.path / "out";
    RestoreOptions ro;
    ro.target = u8(out);
    CHECK(run_restore(*four, four->load_snapshot(i4.snapshot_id), ro).errors == 0);
    std::map<std::string, Bytes> expected = snapshot_of_dir(src);
    expected.erase("gone.bin");
#ifdef _WIN32
    // Reading the source here cannot open the locked file either: what the backup should have
    // stored is its content, or nothing when the backup could not read it.
    if (locked_unreadable) expected.erase("locked.bin");
    else expected["locked.bin"] = seeded_bytes(70000, 6);
#endif
    CHECK(snapshot_of_dir(restored_location(out, src)) == expected);

    VerifyOptions vo;
    vo.read_data = true;
    CHECK(run_verify(*four, vo).errors == 0);
}

TEST_CASE("a backup with several readers stops promptly when cancelled") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    for (int i = 0; i < 12; ++i)
        write_file(src / ("d" + std::to_string(i % 3)) / ("big" + std::to_string(i) + ".bin"),
                   seeded_bytes(3u << 20, static_cast<std::uint32_t>(100 + i)));
    init_repo(tmp.path / "repo");
    auto repo = open_repo(tmp.path / "repo");
    repo->set_parallelism(2, 0);

    BackupOptions o;
    o.paths = {u8(src)};
    o.read_threads = 4;
    // While listing, part way into a folder's files, and in a later folder.
    for (const int after : {4, 12, 30}) {
        CAPTURE(after);
        int polls = 0;
        o.should_cancel = [&polls, after] { return ++polls > after; };
        const auto start = std::chrono::steady_clock::now();
        CHECK_THROWS_AS(run_backup(*repo, o), Cancelled);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(60));
        CHECK(repo->list_snapshot_ids().empty());
    }

    // The repository is still usable: a full run afterwards works.
    o.should_cancel = {};
    const BackupResult r = run_backup(*repo, o);
    CHECK(r.snapshot.stats.files == 12);
    CHECK(r.snapshot.stats.errors == 0);
}

#ifdef _WIN32
TEST_CASE("a file only in the cloud is counted, not read, unless the job downloads it") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "here.txt", text("on this disk"));
    write_file(src / "online.txt", text("only in the cloud"));
    // Marked the way a sync program marks a file it keeps online (offline: reading recalls it).
    const std::wstring online = (src / "online.txt").wstring();
    REQUIRE(SetFileAttributesW(online.c_str(), GetFileAttributesW(online.c_str()) | FILE_ATTRIBUTE_OFFLINE));
    os::FileInfo info;
    os::ErrorCode err = 0;
    REQUIRE(os::stat(u8(src / "online.txt"), info, err));
    REQUIRE(os::is_cloud_only(info));

    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions o;
    o.paths = {u8(src)};
    const BackupResult first = run_backup(*repo, o);
    CHECK(first.snapshot.stats.cloud_only == 1);
    CHECK(first.snapshot.stats.cloud_only_bytes == 17);
    CHECK(first.snapshot.stats.skipped == 0);
    CHECK(first.snapshot.stats.files == 1);
    CHECK(repo->load_snapshot(first.snapshot_id).stats.cloud_only == 1);

    o.download_cloud_files = true;
    const BackupResult second = run_backup(*repo, o);
    CHECK(second.snapshot.stats.cloud_only == 0);
    CHECK(second.snapshot.stats.files == 2);
    CHECK(second.snapshot.stats.bytes == 12 + 17);
}
#endif

TEST_CASE("restore: an interrupted restore goes on, leaving identical files alone; its size is known first") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    write_file(src / "a.bin", seeded_bytes(300000, 41));
    write_file(src / "b.bin", seeded_bytes(200000, 42));
    write_file(src / "sub" / "c.txt", text("small file"));
    init_repo(repo_dir);
    auto repo = open_repo(repo_dir);
    BackupOptions bo;
    bo.paths = {u8(src)};
    const BackupResult b = run_backup(*repo, bo);
    const Snapshot snap = repo->load_snapshot(b.snapshot_id);

    RestoreOptions o;
    o.target = u8(tmp.path / "out");
    RestoreEstimate e = estimate_restore(*repo, snap, o);
    CHECK(e.files == 3);
    CHECK(e.bytes == 300000 + 200000 + 10);
    CHECK(e.needed == e.bytes);

    RestoreResult r = run_restore(*repo, snap, o);
    CHECK(r.files == 3);
    const fs::path out = restored_location(tmp.path / "out", src);
    REQUIRE(fs::exists(out / "a.bin"));
    CHECK(estimate_restore(*repo, snap, o).needed == 0);  // all there already

    // As if the restore stopped during b.bin: half of it written, with a time of now.
    {
        std::ofstream f(out / "b.bin", std::ios::binary | std::ios::trunc);
        const Bytes half = seeded_bytes(100000, 42);
        f.write(reinterpret_cast<const char*>(half.data()), static_cast<std::streamsize>(half.size()));
    }
    e = estimate_restore(*repo, snap, o);
    CHECK(e.needed == 200000);

    // Run again: identical files are left alone, the half one is restored again.
    o.overwrite = true;
    o.skip_identical = true;
    r = run_restore(*repo, snap, o);
    CHECK(r.identical == 2);
    CHECK(r.files == 1);
    CHECK(r.bytes == 200000);
    CHECK(read_file(out / "b.bin") == seeded_bytes(200000, 42));
    CHECK(r.errors == 0);

    // Same size and time but other content: not identical, restored again.
    {
        Bytes changed = seeded_bytes(300000, 41);
        changed[1000] ^= 1;
        os::FileInfo info;
        os::ErrorCode err = 0;
        REQUIRE(os::stat(u8(out / "a.bin"), info, err));
        std::ofstream(out / "a.bin", std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(changed.data()), static_cast<std::streamsize>(changed.size()));
        os::set_metadata(u8(out / "a.bin"), info);
    }
    r = run_restore(*repo, snap, o);
    CHECK(r.identical == 2);
    CHECK(r.files == 1);
    CHECK(read_file(out / "a.bin") == seeded_bytes(300000, 41));
}
