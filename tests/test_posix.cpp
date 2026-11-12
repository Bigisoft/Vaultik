// Linux parts of the OS layer: times, permissions, processes, volumes and private folders.

#include <doctest/doctest.h>

#ifndef _WIN32

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/mount.hpp"
#include "vaultik/platform/drive_health.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
namespace fs = std::filesystem;

namespace {

struct PosixTemp {
    std::string path;
    PosixTemp() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = (fs::temp_directory_path() / ("vaultik-posix-" + to_hex(rnd))).string();
        fs::create_directories(path);
    }
    ~PosixTemp() {
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, ec);
        for (const auto& e : fs::recursive_directory_iterator(path, ec))
            fs::permissions(e.path(), fs::perms::owner_all, fs::perm_options::add, ec);
        fs::remove_all(path, ec);
    }
};

std::string touch(const std::string& dir, const char* name) {
    const std::string p = os::join(dir, name);
    os::write_file_atomic(p, ByteSpan(reinterpret_cast<const std::uint8_t*>("x"), 1));
    return p;
}

}  // namespace

TEST_CASE("POSIX: modification times before 1970 and at the epoch survive set_metadata") {
    PosixTemp tmp;
    const std::string p = touch(tmp.path, "f");
    for (const std::uint64_t mtime : {os::from_unix(-86400LL * 400, 123456700), os::from_unix(0, 0),
                                      os::from_unix(1700000000, 999999900), os::from_unix(-1, 500000000)}) {
        os::FileInfo info;
        info.type = os::EntryType::File;
        info.mode = 0644;
        info.mtime = mtime;
        os::set_metadata(p, info);
        os::FileInfo back;
        os::ErrorCode err = 0;
        REQUIRE(os::stat(p, back, err));
        CHECK(back.mtime == mtime);
    }
}

TEST_CASE("POSIX: modes without owner read are restored, mode 0 only for Linux nodes") {
    PosixTemp tmp;
    const std::string p = touch(tmp.path, "f");
    os::FileInfo info;
    info.type = os::EntryType::File;
    info.mode = 0200;
    os::set_metadata(p, info);
    os::FileInfo back;
    os::ErrorCode err = 0;
    REQUIRE(os::stat(p, back, err));
    CHECK(back.mode == 0200);

    // Mode 0 with Windows attributes: made on Windows, no permissions to apply.
    info.mode = 0;
    info.attributes = 0x20;
    os::set_metadata(p, info);
    REQUIRE(os::stat(p, back, err));
    CHECK(back.mode == 0200);
    // No attributes and no times: a folder a backup put above its sources, never read; it keeps
    // its permissions.
    info.attributes = 0;
    os::set_metadata(p, info);
    REQUIRE(os::stat(p, back, err));
    CHECK(back.mode == 0200);
    // Mode 0 from Linux (attributes 0, with its times) is a real mode.
    info.mtime = os::now();
    os::set_metadata(p, info);
    REQUIRE(os::stat(p, back, err));
    CHECK(back.mode == 0);
}

TEST_CASE("POSIX: run_command captures output and reports programs that cannot start") {
    std::string out;
    CHECK(os::run_command({"/bin/sh", "-c", "echo hello; echo oops >&2; exit 3"}, &out) == 3);
    CHECK(out.find("hello") != std::string::npos);
    CHECK(out.find("oops") != std::string::npos);
    CHECK_THROWS_AS(os::run_command({"/nonexistent/vaultik-no-such-program"}), os::OsError);

    // Standard input is empty: a program waiting for it ends at once.
    out.clear();
    CHECK(os::run_command({"/bin/cat"}, &out) == 0);
    CHECK(out.empty());
}

TEST_CASE("POSIX: run_shell stops at the timeout or when cancelled, with what it started") {
    os::ShellOptions o;
    o.timeout = std::chrono::seconds(1);
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS_AS(os::run_shell("sleep 30 & sleep 30", o), Error);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));

    os::ShellOptions c;
    int polls = 0;
    c.cancelled = [&polls] { return ++polls > 2; };
    CHECK_THROWS_AS(os::run_shell("sleep 30", c), Error);
}

TEST_CASE("POSIX: run_shell environment and working folder") {
    PosixTemp tmp;
    std::string out;
    os::ShellOptions o;
    o.output = &out;
    o.clean_environment = true;
    o.extra_environment = {{"VAULTIK_TEST_VALUE", "a b"}};
    o.directory = tmp.path;
    CHECK(os::run_shell("printf '%s|%s|' \"$VAULTIK_TEST_VALUE\" \"$(pwd)\"; env | grep -c '^VAULTIK_PASSWORD=' || true", o) == 0);
    CHECK(out.starts_with("a b|" + tmp.path + "|"));
    CHECK(out.find("|0") != std::string::npos);  // a clean environment carries no password
}

TEST_CASE("POSIX: run_command clean environment and extra variables") {
    ::setenv("VAULTIK_TEST_LEAK", "secret", 1);
    std::string out;
    os::CommandOptions o;
    o.output = &out;
    o.clean_environment = true;
    o.extra_environment = {{"VAULTIK_TEST_VALUE", "a b"}};
    CHECK(os::run_command({"/bin/sh", "-c", "printf '%s|%s|' \"$VAULTIK_TEST_VALUE\" \"${VAULTIK_TEST_LEAK:-none}\""}, o) == 0);
    CHECK(out == "a b|none|");

    // Without a clean environment the extra variables go on top of this process's.
    out.clear();
    o.clean_environment = false;
    CHECK(os::run_command({"/bin/sh", "-c", "printf '%s|%s|' \"$VAULTIK_TEST_VALUE\" \"${VAULTIK_TEST_LEAK:-none}\""}, o) == 0);
    CHECK(out == "a b|secret|");
    ::unsetenv("VAULTIK_TEST_LEAK");
}

TEST_CASE("POSIX: process ids past the pid_t range are not alive") {
    CHECK(os::process_alive(os::process_id()));
    CHECK_FALSE(os::process_alive(0xFFFFFFFFu));
}

TEST_CASE("POSIX: the home folder without HOME comes from the password database") {
    const std::string saved = os::get_env("HOME");
    ::unsetenv("HOME");
    const std::string home = os::user_folder(os::UserFolder::Profile);
    if (!saved.empty()) ::setenv("HOME", saved.c_str(), 1);
    CHECK_FALSE(home.empty());
}

TEST_CASE("POSIX: folders other accounts than root can change") {
    PosixTemp tmp;
    CHECK_FALSE(os::others_can_change("/"));  // root's, 0755
    CHECK(os::others_can_change("/tmp"));      // 1777: anyone can add a link there
    CHECK(os::others_can_change(os::join(tmp.path, "missing")));
    const std::string dir = os::join(tmp.path, "d");
    const std::string link = os::join(tmp.path, "l");
    REQUIRE(::mkdir(dir.c_str(), 0700) == 0);
    REQUIRE(::symlink("/", link.c_str()) == 0);
    CHECK(os::others_can_change(link));  // a link, even to a safe folder
    // A folder of the account running the test: safe only when that account is root itself.
    CHECK(os::others_can_change(dir) == !os::running_as_root());
    if (os::running_as_root()) {
        ::chmod(dir.c_str(), 0770);
        CHECK(os::others_can_change(dir));  // its group may write
    }
}

TEST_CASE("POSIX: secrets are sealed with a key kept in this account's settings") {
    PosixTemp tmp;
    const std::string saved = os::get_env("VAULTIK_CONFIG_DIR");
    ::setenv("VAULTIK_CONFIG_DIR", tmp.path.c_str(), 1);
    const std::string plain = "correct horse battery staple";
    const ByteSpan in(reinterpret_cast<const std::uint8_t*>(plain.data()), plain.size());
    const Bytes sealed = os::protect_secret(in);
    // The plain text is not in what lands on the disk.
    CHECK(std::search(sealed.begin(), sealed.end(), plain.begin(), plain.end()) == sealed.end());
    const Bytes back = os::unprotect_secret(sealed);
    CHECK(std::string(back.begin(), back.end()) == plain);

    const std::string key = os::join(os::join(tmp.path, "secrets"), "local.key");
    struct stat st{};
    REQUIRE(::stat(key.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);
    CHECK(st.st_size == 32);

    Bytes tampered = sealed;
    tampered.back() ^= 1;
    CHECK_THROWS_AS(os::unprotect_secret(tampered), Error);
    CHECK_THROWS_AS(os::unprotect_secret(in), Error);  // plain text is not taken as a sealed secret

    // On another computer (no key there) it cannot be opened, and a new key does not open it.
    REQUIRE(::unlink(key.c_str()) == 0);
    CHECK_THROWS_AS(os::unprotect_secret(sealed), Error);
    (void)os::protect_secret(in);
    CHECK_THROWS_AS(os::unprotect_secret(sealed), Error);

    if (saved.empty()) ::unsetenv("VAULTIK_CONFIG_DIR");
    else ::setenv("VAULTIK_CONFIG_DIR", saved.c_str(), 1);
}

TEST_CASE("POSIX: private folders are 0700") {
    PosixTemp tmp;
    const std::string dir = os::join(os::join(tmp.path, "a"), "secrets");
    os::make_private_dirs(dir);
    struct stat st{};
    REQUIRE(::stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
    ::chmod(dir.c_str(), 0755);  // a folder left open by an older version is tightened
    os::make_private_dirs(dir);
    REQUIRE(::stat(dir.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0700);
}

TEST_CASE("POSIX: volume names") {
    // Whatever the test machine has: a folder on a disk with a UUID gives a volume whose root
    // holds it; volume_root finds that root again.
    const os::VolumeLocation loc = os::volume_of(fs::current_path().string());
    if (!loc.volume.empty()) {
        CHECK(loc.volume.starts_with("uuid:"));
        CHECK(os::volume_root(loc.volume) == loc.root);
    }
    CHECK(os::volume_root("uuid:vaultik-no-such-volume").empty());
    CHECK(os::volume_root("uuid:../../etc").empty());
    CHECK(os::volume_root("\\\\?\\Volume{0}\\").empty());
}

TEST_CASE("POSIX: a version shown as a folder with FUSE") {
    if (!SnapshotMount::available()) {
        MESSAGE("FUSE is not available here: " << SnapshotMount::how_to_enable());
        return;
    }
    PosixTemp tmp;
    const std::string src = os::join(tmp.path, "src");
    fs::create_directories(src + "/sub/deeper");
    std::mt19937 rng(7);
    std::string big(5u << 20, '\0');  // several chunks
    for (char& c : big) c = static_cast<char>(rng());
    const auto put = [](const std::string& path, const std::string& data) {
        std::ofstream f(path, std::ios::binary);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    };
    put(src + "/big.bin", big);
    put(src + "/sub/note.txt", "hello from the backup\n");
    put(src + "/sub/deeper/empty", "");
    put(src + "/private.txt", "secret");
    ::chmod((src + "/private.txt").c_str(), 0600);
    ::chmod((src + "/sub").c_str(), 0750);
    REQUIRE(::symlink("sub/note.txt", (src + "/link").c_str()) == 0);
    struct timespec times[2] = {{1600000000, 0}, {1600000000, 0}};
    ::utimensat(AT_FDCWD, (src + "/sub/note.txt").c_str(), times, 0);

    const std::string repo_dir = os::join(tmp.path, "repo");
    {
        LocalBackend backend(repo_dir, true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, "pw", options);
    }
    auto repo = Repository::open(std::make_unique<LocalBackend>(repo_dir), "pw");
    BackupOptions backup;
    backup.paths = {src};
    const BackupResult result = run_backup(*repo, backup);

    const std::string folder = os::join(tmp.path, "view");
    std::optional<SnapshotMount> mount;
    try {
        mount.emplace(*repo, result.snapshot, folder);
    } catch (const Error& e) {
        MESSAGE("could not mount here (" << e.what() << "); skipped");
        return;
    }
    CHECK(mount->active());
    // The version keeps the full path of the source below the folder.
    std::string inside = folder;
    for (const std::string& part : os::path_components(src)) inside += "/" + part;
    const auto read = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    CHECK(read(inside + "/big.bin") == big);
    CHECK(read(inside + "/sub/note.txt") == "hello from the backup\n");
    CHECK(read(inside + "/sub/deeper/empty").empty());
    CHECK(read(inside + "/link") == "hello from the backup\n");

    struct stat st{};
    REQUIRE(::lstat((inside + "/big.bin").c_str(), &st) == 0);
    CHECK(static_cast<std::size_t>(st.st_size) == big.size());
    REQUIRE(::lstat((inside + "/private.txt").c_str(), &st) == 0);
    CHECK((st.st_mode & 07777) == 0600);
    CHECK(st.st_uid == ::getuid());
    REQUIRE(::lstat((inside + "/sub").c_str(), &st) == 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK((st.st_mode & 07777) == 0750);
    REQUIRE(::lstat((inside + "/sub/note.txt").c_str(), &st) == 0);
    CHECK(st.st_mtim.tv_sec == 1600000000);
    REQUIRE(::lstat((inside + "/link").c_str(), &st) == 0);
    CHECK(S_ISLNK(st.st_mode));
    char target[64] = {};
    CHECK(::readlink((inside + "/link").c_str(), target, sizeof target - 1) == 12);
    CHECK(std::string(target) == "sub/note.txt");

    // Read-only: nothing can be changed, added or removed.
    CHECK(::open((inside + "/sub/note.txt").c_str(), O_WRONLY) < 0);
    CHECK(errno == EROFS);
    CHECK(::open((inside + "/new.txt").c_str(), O_WRONLY | O_CREAT, 0644) < 0);
    CHECK(::unlink((inside + "/big.bin").c_str()) != 0);
    CHECK(::lstat((inside + "/missing").c_str(), &st) != 0);

    mount.reset();
    CHECK_FALSE(fs::exists(folder));  // unmounted and removed
}

TEST_CASE("POSIX: drive health of a folder never fails") {
    // Whatever the test machine has (a virtual disk in WSL and CI): an answer or nothing, and an
    // unknown health always says why.
    for (const os::DriveHealth& d : os::drive_health(fs::current_path().string())) {
        CHECK(d.device.starts_with("/dev/"));
        if (!d.health_percent() && d.status == os::DriveHealth::Status::Unknown) CHECK_FALSE(d.unknown_reason.empty());
    }
    CHECK(os::drive_health("/proc").empty());  // no device
    for (const os::DriveHealth& d : os::all_drives()) CHECK_FALSE(d.device.empty());
}

#endif
