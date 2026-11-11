// Windows extras: security descriptors, alternate data streams and junctions survive a backup and
// a restore.
#ifdef _WIN32

#include <doctest/doctest.h>

#include <windows.h>

#include <sddl.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

struct TempDir {
    fs::path path;
    TempDir() {
        std::uint8_t rnd[6];
        random_bytes(rnd);
        path = fs::temp_directory_path() / ("vaultik-extras-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_text(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << s;
}

std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

Bytes descriptor(const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    ULONG size = 0;
    REQUIRE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, &size));
    Bytes out(static_cast<std::uint8_t*>(sd), static_cast<std::uint8_t*>(sd) + size);
    LocalFree(sd);
    return out;
}

// The access list of a path in SDDL ("D:P(A;;FA;;;OW)..."), to compare two of them.
std::string dacl_of(const fs::path& p) {
    const Bytes sd = os::read_security(u8(p));
    REQUIRE_FALSE(sd.empty());
    LPWSTR text = nullptr;
    REQUIRE(ConvertSecurityDescriptorToStringSecurityDescriptorW(
        const_cast<std::uint8_t*>(sd.data()), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr));
    std::wstring w(text);
    LocalFree(text);
    std::string s;
    for (const wchar_t c : w) s += static_cast<char>(c);  // SDDL is ASCII
    return s;
}

}  // namespace

TEST_CASE("permissions, streams and junctions come back on Windows") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    fs::create_directories(src / "target");
    write_text(src / "a.txt", "main content");
    write_text(fs::path(u8(src / "a.txt") + ":notes"), "stream content");
    write_text(src / "target" / "inside.txt", "x");
    // Owner and everyone may read, only the owner may change; not inherited from the folder.
    REQUIRE(os::write_security(u8(src / "a.txt"), descriptor(L"D:P(A;;FA;;;OW)(A;;FR;;;WD)(A;;FA;;;SY)")) == 0);
    const std::string dacl = dacl_of(src / "a.txt");
    REQUIRE(os::write_security(u8(src / "target"), descriptor(L"D:P(A;OICI;FA;;;OW)(A;OICI;FA;;;SY)(A;OICI;FR;;;BU)")) == 0);
    const std::string folder_dacl = dacl_of(src / "target");
    REQUIRE(os::create_link(u8(src / "jn"), {0xA0000003, u8(src / "target"), false}, true) == 0);

    const auto streams = os::list_streams(u8(src / "a.txt"));
    REQUIRE(streams.size() == 1);
    CHECK(streams[0].name == "notes");
    os::ReparseLink read;
    REQUIRE(os::read_link(u8(src / "jn"), read));
    CHECK(read.target == u8(src / "target"));

    LocalBackend backend(u8(repo_dir), true);
    InitOptions io;
    io.kdf_iterations = 1000;
    Repository::init(backend, "pw", io);
    auto repo = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), "pw");
    BackupOptions bo;
    bo.paths = {u8(src)};
    const BackupResult b = run_backup(*repo, bo);

    // The extras survive a prune (their data counts as in use).
    PruneOptions po;
    run_prune(*repo, po);

    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    ro.permissions = true;
    const RestoreResult r = run_restore(*repo, b.snapshot, ro);
    CHECK(r.errors == 0);
    fs::path out = tmp.path / "out";
    for (const std::string& c : os::path_components(u8(src))) out /= fs::path(std::u8string(c.begin(), c.end()));
    CHECK(read_text(out / "a.txt") == "main content");
    CHECK(read_text(fs::path(u8(out / "a.txt") + ":notes")) == "stream content");
    CHECK(dacl_of(out / "a.txt") == dacl);
    CHECK(dacl_of(out / "target") == folder_dacl);
    os::ReparseLink back;
    REQUIRE(os::read_link(u8(out / "jn"), back));
    CHECK(back.tag == 0xA0000003u);
    CHECK(back.target == u8(src / "target"));

    // Without permissions the restored file gets the folder's usual ones.
    RestoreOptions plain;
    plain.target = u8(tmp.path / "out2");
    CHECK(run_restore(*repo, b.snapshot, plain).errors == 0);
}

TEST_CASE("restoring permissions and streams never writes through a link") {
    TempDir tmp;
    const fs::path victim = tmp.path / "victim.txt";
    write_text(victim, "someone else's file");
    const Bytes sd = descriptor(L"D:P(A;;FA;;;OW)");

    // A restored file swapped for a hard link to another file.
    const fs::path swapped = tmp.path / "restored.txt";
    REQUIRE(CreateHardLinkW(swapped.c_str(), victim.c_str(), nullptr));
    CHECK(os::write_security(u8(swapped), sd) != 0);
    os::ErrorCode err = 0;
    const os::FileId any{};
    CHECK_FALSE(os::File::create_stream(u8(swapped), "x", any, err).valid());
    CHECK(os::list_streams(u8(victim)).empty());

    // A restored folder swapped for a junction to another folder.
    fs::create_directories(tmp.path / "elsewhere");
    REQUIRE(os::create_link(u8(tmp.path / "jn"), {0xA0000003, u8(tmp.path / "elsewhere"), false}, true) == 0);
    CHECK(os::write_security(u8(tmp.path / "jn"), sd) == ERROR_STOPPED_ON_SYMLINK);

    // A plain file of its own is fine, as long as it is the one expected.
    const fs::path own = tmp.path / "own.txt";
    write_text(own, "mine");
    const std::optional<os::FileId> own_id = os::file_id(u8(own));
    REQUIRE(own_id);
    CHECK(os::write_security(u8(own), sd, &*own_id) == 0);
    {
        os::File s = os::File::create_stream(u8(own), "x", *own_id, err);
        CHECK(s.valid());
    }

    // The same name now naming another file (a folder on the way swapped for a link): refused.
    const fs::path other = tmp.path / "other.txt";
    write_text(other, "other");
    const std::optional<os::FileId> other_id = os::file_id(u8(other));
    REQUIRE(other_id);
    CHECK(os::write_security(u8(other), sd, &*own_id) == ERROR_STOPPED_ON_SYMLINK);
    CHECK_FALSE(os::File::create_stream(u8(other), "y", *own_id, err).valid());
    CHECK(os::list_streams(u8(other)).empty());
}

TEST_CASE("a held folder cannot be renamed away, and a junction is not held as a folder") {
    TempDir tmp;
    fs::create_directories(tmp.path / "dir" / "sub");
    os::ErrorCode err = 0;
    {
        os::File held = os::File::hold_directory(u8(tmp.path / "dir"), true, err);
        REQUIRE(held.valid());
        std::error_code ec;
        fs::rename(tmp.path / "dir", tmp.path / "moved", ec);
        CHECK(ec);  // refused while held
        CHECK(fs::exists(tmp.path / "dir" / "sub"));
    }
    std::error_code ec;
    fs::rename(tmp.path / "dir", tmp.path / "moved", ec);
    CHECK_FALSE(ec);  // free again once closed
    REQUIRE(os::create_link(u8(tmp.path / "jn"), {0xA0000003, u8(tmp.path / "moved"), false}, true) == 0);
    CHECK_FALSE(os::File::hold_directory(u8(tmp.path / "jn"), true, err).valid());
    CHECK(err == ERROR_STOPPED_ON_SYMLINK);
}

TEST_CASE("commands of administrator jobs: only safe ones, checked") {
    using vaultik::jobs::check_admin_command;
    char windows[MAX_PATH] = {};
    GetSystemWindowsDirectoryA(windows, MAX_PATH);
    const std::string cmd = std::string(windows) + "\\System32\\cmd.exe";
    CHECK_NOTHROW(check_admin_command(cmd + " /d /v:off /c echo hello", "the command"));
    CHECK_NOTHROW(check_admin_command("\"" + cmd + "\" /d /v:off /c ver", "the command"));
    CHECK_THROWS(check_admin_command("%TOOLS%\\dump.cmd", "the command"));
    CHECK_THROWS(check_admin_command("C:^\\Users\\x.cmd", "the command"));
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c ..\\..\\x.cmd", "the command"));
    CHECK_THROWS(check_admin_command("\\\\server\\share\\x.cmd", "the command"));
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c C:Users\\me\\x.cmd", "the command"));  // relative to the drive
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c \\Users\\me\\x.cmd", "the command"));  // relative to its root
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c /Users/me/x.cmd", "the command"));
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c /x.cmd", "the command"));      // a file at the drive's top
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c /Users", "the command"));      // a folder there
    CHECK_NOTHROW(check_admin_command(cmd + " /d /v:off /c ver & echo done", "the command"));
    TempDir tmp;  // in the user's temp folder: anyone running as the user can change it
    CHECK_THROWS(check_admin_command(cmd + " /d /v:off /c " + u8(tmp.path / "dump.cmd"), "the command"));
    CHECK_THROWS(check_admin_command("\"" + u8(tmp.path / "not yet there.cmd") + "\"", "the command"));
    // What a command starts with: a program or batch file only, nothing that starts others unseen.
    CHECK_NOTHROW(check_admin_command("vssadmin list shadows", "the command"));
    CHECK_NOTHROW(check_admin_command("net stop spooler & net start spooler", "the command"));
    CHECK_NOTHROW(check_admin_command("ver > nul 2>&1", "the command"));
    const std::string script = std::string(windows) + "\\System32\\WindowsPowerShell\\v1.0\\Modules";
    CHECK_NOTHROW(check_admin_command("powershell -NoProfile -ExecutionPolicy Bypass -File " + script + "\\x.ps1",
                                      "the command"));
    CHECK_THROWS(check_admin_command("powershell -NoProfile -Command Get-Date", "the command"));  // unread
    CHECK_THROWS(check_admin_command("powershell -NoProfile -File x.ps1", "the command"));  // not a full path
    CHECK_THROWS(check_admin_command("pwsh.exe -nop -File " + script + "\\x.ps1", "the command"));
    CHECK_THROWS(check_admin_command("powershell -File " + script + "\\x.ps1", "the command"));  // the user's profile
    CHECK_THROWS(check_admin_command("plink nas", "the command"));
    CHECK_THROWS(check_admin_command("putty -load nas", "the command"));
    CHECK_THROWS(check_admin_command("curl -o x https://example.com", "the command"));  // reads the user's .curlrc
    CHECK_THROWS(check_admin_command("powershell_ise", "the command"));
    CHECK_THROWS(check_admin_command("POWERS~1 -Command x", "the command"));
    CHECK_THROWS(check_admin_command("whoami", "the command"));  // not one of the listed tools by name alone
    CHECK_THROWS(check_admin_command("python -c x", "the command"));
    CHECK_NOTHROW(check_admin_command(std::string(windows) + "\\System32\\whoami.exe", "the command"));
    CHECK_THROWS(check_admin_command("powershell -NoProfile bypass -File " + script + "\\x.ps1", "the command"));
    CHECK_THROWS(check_admin_command("powershell -NoProfile -ep;evil -File " + script + "\\x.ps1", "the command"));
    // -File must be PowerShell's own, not a later command's: here it would read commands from the pipe.
    CHECK_THROWS(check_admin_command("echo x | powershell -NoProfile & bypass -File " + script + "\\x.ps1",
                                     "the command"));
    CHECK_THROWS(check_admin_command("echo x | powershell -NoProfile & ver -File " + script + "\\x.ps1",
                                     "the command"));
    CHECK_THROWS(check_admin_command("git-upload-pack x", "the command"));
    CHECK_THROWS(check_admin_command("ver & run.vbs", "the command"));  // opened by the user's file types
    CHECK_THROWS(check_admin_command("ver | (task.js)", "the command"));
    CHECK_THROWS(check_admin_command("call x.ps1", "the command"));
    CHECK_THROWS(check_admin_command("start x.exe", "the command"));
    CHECK_THROWS(check_admin_command("if 1==1 x.vbs", "the command"));
    CHECK_THROWS(check_admin_command("for %i in (1) do x", "the command"));
    CHECK_THROWS(check_admin_command("cmd /c ver", "the command"));  // the user's AutoRun command
    CHECK_THROWS(check_admin_command("cmd /d /v:off /c x.vbs", "the command"));
    CHECK_THROWS(check_admin_command("cmd /d /v:off /v:on /c ver", "the command"));
    CHECK_THROWS(check_admin_command("forfiles /c x", "the command"));
    CHECK_THROWS(check_admin_command("set PATHEXT=.MSC & compmgmt", "the command"));
    CHECK_THROWS(check_admin_command("ssh nas stop-db", "the command"));
    CHECK_THROWS(check_admin_command("ver !x!", "the command"));
    CHECK_THROWS(check_admin_command("cmd /d /v:off /c \"ver & x.vbs\"", "the command"));
    CHECK_THROWS(check_admin_command("> out.txt ver", "the command"));
    CHECK_THROWS(check_admin_command("ver\nx.vbs", "the command"));
}

TEST_CASE("a command runs with a time limit, and programs it leaves running are not waited for") {
    os::ShellOptions o;
    o.timeout = std::chrono::seconds(2);
    const auto start = std::chrono::steady_clock::now();
    CHECK_THROWS(os::run_shell("ping -n 30 127.0.0.1 > nul", o));  // stopped after 2 s
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(15));

    std::string out;
    os::ShellOptions o2;
    o2.output = &out;
    const auto start2 = std::chrono::steady_clock::now();
    CHECK(os::run_shell("start /b ping -n 20 127.0.0.1 & echo started", o2) == 0);
    CHECK(std::chrono::steady_clock::now() - start2 < std::chrono::seconds(10));
    CHECK(out.find("started") != std::string::npos);

    // The fixed environment of administrator runs: the user's variables are not there.
    std::string env;
    os::ShellOptions o3;
    o3.output = &env;
    o3.clean_environment = true;
    o3.extra_environment = {{"VAULTIK_RESULT", "success"}};
    CHECK(os::run_shell("echo [%USERPROFILE%] [%VAULTIK_RESULT%]", o3) == 0);
    CHECK(env.find("[%USERPROFILE%]") != std::string::npos);
    CHECK(env.find("[success]") != std::string::npos);
}

TEST_CASE("a file changed through its other name (a hard link) is read again") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    fs::create_directories(src / "b");
    fs::create_directories(src / "c");
    write_text(src / "b" / "linked.txt", "linked");
    fs::create_hard_link(src / "b" / "linked.txt", src / "c" / "linked-too.txt");

    LocalBackend backend(u8(repo_dir), true);
    InitOptions io;
    io.kdf_iterations = 1000;
    Repository::init(backend, "pw", io);
    auto repo = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), "pw");
    BackupOptions bo;
    bo.paths = {u8(src)};
    run_backup(*repo, bo);

    // Through the second name: the folder listing of b still shows the old size and times.
    write_text(src / "c" / "linked-too.txt", "LINKED, changed through the other name");
    const BackupResult second = run_backup(*repo, bo);

    const auto restored = [&](const BackupResult& b, const char* out_name) {
        RestoreOptions ro;
        ro.target = u8(tmp.path / out_name);
        CHECK(run_restore(*repo, b.snapshot, ro).errors == 0);
        fs::path out = tmp.path / out_name;
        for (const std::string& c : os::path_components(u8(src))) out /= fs::path(std::u8string(c.begin(), c.end()));
        return out;
    };
    const fs::path out = restored(second, "out");
    CHECK(read_text(out / "b" / "linked.txt") == "LINKED, changed through the other name");
    CHECK(read_text(out / "c" / "linked-too.txt") == "LINKED, changed through the other name");

    // A file that got its second name after the last backup is only caught when every file is
    // checked.
    write_text(src / "b" / "plain.txt", "plain");
    run_backup(*repo, bo);
    fs::create_hard_link(src / "b" / "plain.txt", src / "c" / "plain-too.txt");
    write_text(src / "c" / "plain-too.txt", "PLAIN, changed through a new name");
    bo.check_every_file = true;
    const BackupResult checked = run_backup(*repo, bo);
    CHECK(read_text(restored(checked, "out2") / "b" / "plain.txt") == "PLAIN, changed through a new name");
}

TEST_CASE("a program run with the user's own rights runs, writes and returns its exit code") {
    TempDir tmp;
    wchar_t system[MAX_PATH];
    REQUIRE(GetSystemDirectoryW(system, MAX_PATH) > 0);
    const fs::path cmd = fs::path(system) / L"cmd.exe";
    const fs::path out = tmp.path / "made.txt";
    const fs::path groups = tmp.path / "groups.txt";
    const std::string line = "cmd.exe /d /c \"echo made> \"" + u8(out) + "\" & whoami /groups > \"" + u8(groups) +
                             "\" & exit 7\"";
    CHECK(os::run_with_user_rights(u8(cmd), line) == 7);
    // What it made belongs to the account (it could not have been made with Administrators as owner).
    CHECK(read_text(out).starts_with("made"));
    // Never at high integrity, whatever this test runs with.
    CHECK(read_text(groups).find("High Mandatory Level") == std::string::npos);
    CHECK_THROWS(os::run_with_user_rights(u8(tmp.path / "missing.exe"), "missing.exe"));
}

TEST_CASE("an elevated run of a job from the user's settings neither starts on its own nor runs commands") {
    jobs::Job job;
    job.name = "Docs";
    CHECK(jobs::elevated_run_refusal(job, false).empty());
    CHECK(jobs::elevated_run_refusal(job, true).find("automatically") != std::string::npos);
    job.before_command = "net user x /add";
    CHECK(jobs::elevated_run_refusal(job, false).find("commands") != std::string::npos);
    job.before_command.clear();
    job.after_command = "C:\\Windows\\System32\\cmd.exe /c exit 0";
    CHECK(!jobs::elevated_run_refusal(job, false).empty());
}

TEST_CASE("the approval of a job with administrator rights shows everything that gives it its power") {
    jobs::Job job;
    job.name = "Disks";
    job.repository = "s3:https://s3.example.com/bucket/vaultik";
    job.cloud_lock_days = 30;
    job.cloud_lock_compliance = true;
    job.image_disks = {"0", "system"};
    job.second_copy.repository = "sftp://backup@nas.example.com/vaultik";
    job.second_copy.cloud_region = "SHA256:abcdef";
    job.second_copy.cloud_lock_days = 7;
    job.retention.last = 1;
    const jobs::AlertSettings alerts;
    std::string q = jobs::approval_question(job, false, true, alerts);
    CHECK(q.find("the whole disk 0 (every partition)") != std::string::npos);
    CHECK(q.find("the whole disk Windows runs from") != std::string::npos);
    CHECK(q.find("locked for 30 days (compliance mode") != std::string::npos);
    CHECK(q.find("locked for 7 days (governance mode)") != std::string::npos);
    CHECK(q.find("trusting the server key SHA256:abcdef") != std::string::npos);
    CHECK(q.find("Removes old versions, keeping") != std::string::npos);

    jobs::Job vm;
    vm.name = "Machines";
    vm.repository = "D:\\Backups";
    vm.hyperv_vms = {"Web"};
    q = jobs::approval_question(vm, false, true, alerts);
    CHECK(q.find("Hyper-V virtual machine Web") != std::string::npos);
    CHECK(q.find("exported first into Vaultik's folder in ProgramData") != std::string::npos);
    CHECK(q.find("Keeps every version.") != std::string::npos);
    vm.hyperv_staging = "E:\\Exports";
    CHECK(jobs::approval_question(vm, false, true, alerts).find("exported first into a new folder in E:\\Exports") != std::string::npos);

    // What does not fit on screen is refused rather than cut.
    job.image_disks.assign(13, "1");
    CHECK_THROWS(jobs::approval_question(job, false, true, alerts));
    job.image_disks.assign(12, "gpt-01234567-89ab-cdef-0123-456789abcdef");
    CHECK_THROWS(jobs::approval_question(job, false, true, alerts));
}

#endif  // _WIN32
