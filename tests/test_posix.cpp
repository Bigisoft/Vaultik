// Linux parts of the OS layer: times, permissions, processes, volumes and private folders.

#include <doctest/doctest.h>

#ifndef _WIN32

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
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
    // Mode 0 from Linux (attributes 0) is a real mode.
    info.attributes = 0;
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

#endif
