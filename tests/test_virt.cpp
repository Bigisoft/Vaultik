#include <doctest/doctest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/job.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/virt.hpp"
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
        path = fs::temp_directory_path() / ("vaultik-virt-test-" + to_hex(rnd));
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

Bytes read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A stream from memory, read in small uneven pieces like a pipe gives them.
class MemoryStream : public StreamSource {
public:
    MemoryStream(Bytes data, bool fail) : data_(std::move(data)), fail_(fail) {}
    std::size_t read(std::uint8_t* dst, std::size_t max) override {
        const std::size_t n = std::min({max, data_.size() - pos_, std::size_t{70000}});
        std::memcpy(dst, data_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    void finish() override {
        if (fail_) throw Error("the exporting program failed");
    }

private:
    Bytes data_;
    std::size_t pos_ = 0;
    bool fail_;
};

ExtraSource memory_source(std::vector<std::string> place, const Bytes& data, bool fail = false) {
    ExtraSource x;
    x.place = std::move(place);
    x.open = [data, fail] { return std::make_unique<MemoryStream>(data, fail); };
    return x;
}

}  // namespace

TEST_CASE("WSL distribution names and version paths") {
    CHECK(valid_distro_name("Ubuntu"));
    CHECK(valid_distro_name("rancher-desktop-data"));
    CHECK(valid_distro_name("Ubuntu-24.04"));
    CHECK(valid_distro_name("my_distro"));
    CHECK_FALSE(valid_distro_name(""));
    CHECK_FALSE(valid_distro_name("two words"));
    CHECK_FALSE(valid_distro_name("a/b"));
    CHECK_FALSE(valid_distro_name("x\"y"));
    CHECK_FALSE(valid_distro_name("--help"
                                  "\n"));
    CHECK_FALSE(valid_distro_name(std::string(65, 'a')));

    CHECK(wsl_place("Ubuntu") == std::vector<std::string>{"WSL", "Ubuntu.tar"});
    CHECK(wsl_snapshot_path("Ubuntu") == "WSL/Ubuntu.tar");
    CHECK(distro_of_path("WSL/Ubuntu.tar") == "Ubuntu");
    CHECK(distro_of_path("/WSL\\acu-linux.tar") == "acu-linux");
    CHECK(distro_of_path("WSL/Ubuntu.tar/x") == "");
    CHECK(distro_of_path("C/WSL/Ubuntu.tar") == "");
    CHECK(distro_of_path("WSL/Ubuntu.vhdx") == "");
    CHECK(distro_of_path("WSL") == "");
}

TEST_CASE("wsl.exe output is read as UTF-16 or UTF-8") {
    // "Ubuntu\r\nDebian\r\n" in UTF-16LE with a byte order mark.
    std::string utf16 = "\xFF\xFE";
    for (const char c : std::string("Ubuntu\r\nDebian\r\n")) {
        utf16.push_back(c);
        utf16.push_back('\0');
    }
    CHECK(console_text(utf16) == "Ubuntu\nDebian\n");
    CHECK(console_text("Ubuntu\r\nDebian\r\n") == "Ubuntu\nDebian\n");
    CHECK(console_text("\xEF\xBB\xBFok") == "ok");
    // A non-ASCII character in UTF-16 (U+0436) comes out as UTF-8.
    const std::string zhe("\x36\x04\x0A\x00", 4);
    CHECK(console_text(zhe) == "\xD0\xB6\n");

    const std::vector<std::string> names =
        parse_distro_names("Ubuntu\n  acu-linux  \nThere are no running distributions.\n\nDebian\n");
    CHECK(names == std::vector<std::string>{"Ubuntu", "acu-linux", "Debian"});
    CHECK(parse_distro_names("Windows Subsystem for Linux has no installed distributions.").empty());
}

TEST_CASE("which chosen WSL distributions a backup exports") {
    const std::vector<WslDistro> installed = {{"Ubuntu", "D:\\WSL\\Ubuntu", "D:\\WSL\\Ubuntu\\ext4.vhdx", 2},
                                              {"Debian", "C:\\x", "C:\\x\\ext4.vhdx", 2},
                                              {"old", "C:\\old", "", 1}};
    SUBCASE("stopped ones are exported, missing ones warned about") {
        const WslPlan p = plan_wsl({"ubuntu", "Gone", "Debian", "Ubuntu"}, installed, {}, WslWhenRunning::Skip);
        CHECK(p.export_names == std::vector<std::string>{"Ubuntu", "Debian"});  // installed spelling, once
        CHECK(p.stop_first.empty());
        CHECK(p.not_installed == std::vector<std::string>{"Gone"});
        CHECK(p.running_skipped.empty());
    }
    SUBCASE("a running one is skipped by default") {
        const WslPlan p = plan_wsl({"Ubuntu", "Debian"}, installed, {"UBUNTU"}, WslWhenRunning::Skip);
        CHECK(p.export_names == std::vector<std::string>{"Debian"});
        REQUIRE(p.running_skipped.size() == 1);
        CHECK(p.running_skipped[0].first == "Ubuntu");
        CHECK(p.running_skipped[0].second.find("running") != std::string::npos);
    }
    SUBCASE("exported while running, never stopped") {
        const WslPlan p = plan_wsl({"Ubuntu"}, installed, {"Ubuntu"}, WslWhenRunning::Export);
        CHECK(p.export_names == std::vector<std::string>{"Ubuntu"});
        CHECK(p.stop_first.empty());
        CHECK(p.running_skipped.empty());
    }
    SUBCASE("stopped first only when the job says so, and only the running ones") {
        const WslPlan p = plan_wsl({"Ubuntu", "Debian"}, installed, {"Ubuntu"}, WslWhenRunning::Stop);
        CHECK(p.export_names == std::vector<std::string>{"Ubuntu", "Debian"});
        CHECK(p.stop_first == std::vector<std::string>{"Ubuntu"});
    }
    CHECK(parse_wsl_when_running("stop") == WslWhenRunning::Stop);
    CHECK(std::string(to_string(WslWhenRunning::Export)) == "export");
    CHECK_THROWS_AS(parse_wsl_when_running("kill"), Error);
}

TEST_CASE("disk files of backed-up machines are excluded from the file backup") {
    const std::vector<std::string> ex = disk_excludes(
        {"\\\\?\\C:\\Users\\me\\AppData\\Local\\rancher-desktop\\distro\\ext4.vhdx", "D:\\WSL\\Ubuntu\\ext4.vhdx",
         "D:\\WSL\\Ubuntu\\ext4.vhdx", "\\\\?\\UNC\\server\\share\\vm.vhdx", "C:\\odd*name.vhdx", ""});
    CHECK(ex == std::vector<std::string>{"C:\\Users\\me\\AppData\\Local\\rancher-desktop\\distro\\ext4.vhdx",
                                         "D:\\WSL\\Ubuntu\\ext4.vhdx", "\\\\server\\share\\vm.vhdx"});
#ifdef _WIN32
    CHECK(path_excluded(ex, "D:\\WSL\\Ubuntu\\ext4.vhdx"));
    CHECK(path_excluded(ex, "d:\\wsl\\ubuntu\\EXT4.VHDX"));  // any case, as Windows compares names
    CHECK(path_excluded(ex, "C:\\Users\\me\\AppData\\Local\\rancher-desktop\\distro\\ext4.vhdx"));
    CHECK_FALSE(path_excluded(ex, "D:\\WSL\\Ubuntu\\other.vhdx"));
    CHECK_FALSE(path_excluded(ex, "D:\\WSL\\Debian\\ext4.vhdx"));
    CHECK_FALSE(path_excluded(ex, "D:\\WSL\\Ubuntu"));
#endif
}

TEST_CASE("Hyper-V machine names, version paths and Get-VM's list") {
    CHECK(valid_vm_name("Windows 11 dev"));
    CHECK(valid_vm_name("Test (1)"));
    CHECK_FALSE(valid_vm_name(""));
    CHECK_FALSE(valid_vm_name("*"));
    CHECK_FALSE(valid_vm_name("vm?"));
    CHECK_FALSE(valid_vm_name("vm[1]"));
    CHECK_FALSE(valid_vm_name("a`b"));
    CHECK_FALSE(valid_vm_name(" padded"));
    CHECK_FALSE(valid_vm_name("line\nbreak"));
    CHECK_FALSE(valid_vm_name(std::string(101, 'v')));

    CHECK(vm_file_name("Windows 11 dev") == "Windows 11 dev");
    CHECK(vm_file_name("a/b:c") == "a_b_c");
    CHECK(vm_file_name("dots.") == "dots_");
    CHECK(hyperv_place("a/b") == std::vector<std::string>{"Hyper-V", "a_b"});
    CHECK(hyperv_snapshot_path("Lab") == "Hyper-V/Lab");
    CHECK(machine_of_path("Hyper-V/Lab") == "Lab");
    CHECK(machine_of_path("/Hyper-V/Lab/") == "Lab");
    CHECK(machine_of_path("Hyper-V/Lab/Virtual Hard Disks") == "");
    CHECK(machine_of_path("WSL/Lab") == "");

    const auto many = parse_hyperv_list(
        R"([{"Name":"Lab","State":"Running","Disks":["D:\\VMs\\lab.vhdx","D:\\VMs\\data.vhdx"]},)"
        R"({"Name":"Empty","State":"Off","Disks":[]},{"Name":"","State":"Off"}])");
    REQUIRE(many.size() == 2);
    CHECK(many[0].name == "Lab");
    CHECK(many[0].state == "Running");
    CHECK(many[0].disks == std::vector<std::string>{"D:\\VMs\\lab.vhdx", "D:\\VMs\\data.vhdx"});
    CHECK(many[1].disks.empty());
    // One machine: ConvertTo-Json gives an object, and a single disk as a string.
    const auto one = parse_hyperv_list(R"({"Name":"Solo","State":"Off","Disks":"C:\\solo.vhdx"})");
    REQUIRE(one.size() == 1);
    CHECK(one[0].disks == std::vector<std::string>{"C:\\solo.vhdx"});
    CHECK(parse_hyperv_list("").empty());
    CHECK(parse_hyperv_list("[]").empty());
    CHECK_THROWS_AS(parse_hyperv_list("Get-VM : You do not have the required permission"), Error);
}

TEST_CASE("an exported machine's configuration is found") {
    TempDir tmp;
    CHECK_THROWS_AS(find_vmcx(u8(tmp.path)), Error);
    fs::create_directories(tmp.path / "Virtual Machines");
    { std::ofstream(tmp.path / "Virtual Machines" / "1234.vmcx") << "x"; }
    { std::ofstream(tmp.path / "Virtual Machines" / "1234.vmgs") << "x"; }
    CHECK(find_vmcx(u8(tmp.path)) == os::join(os::join(u8(tmp.path), "Virtual Machines"), "1234.vmcx"));
    { std::ofstream(tmp.path / "Virtual Machines" / "5678.VMCX") << "x"; }
    CHECK_THROWS_AS(find_vmcx(u8(tmp.path)), Error);
}

TEST_CASE("job fields for WSL and Hyper-V") {
    jobs::Job job;
    job.id = "vms";
    job.name = "VMs";
    job.repository = "D:\\Backups";
    job.wsl_distros = {"Ubuntu", "acu-linux"};
    job.wsl_when_running = "stop";
    CHECK_NOTHROW(jobs::validate(job));  // no folders needed when it backs up distributions

    const jobs::Job back = jobs::job_from_json(jobs::to_json(job));
    CHECK(back.wsl_distros == job.wsl_distros);
    CHECK(back.wsl_when_running == "stop");
    CHECK(back.hyperv_vms.empty());

    SUBCASE("older jobs have none") {
        nlohmann::json j = nlohmann::json::parse(jobs::to_json(job));
        REQUIRE(j.erase("wsl") == 1);
        REQUIRE(j.erase("hyperv") == 1);
        j["sources"] = {"C:\\Data"};  // (such a job needed a folder)
        const jobs::Job old = jobs::job_from_json(j.dump());
        CHECK(old.wsl_distros.empty());
        CHECK(old.wsl_when_running == "skip");
    }
    SUBCASE("bad values are refused") {
        jobs::Job j = job;
        j.wsl_distros = {"bad name"};
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j = job;
        j.wsl_when_running = "kill";
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j = job;
        j.run_as_admin = true;  // the system account has no WSL distributions
        CHECK_THROWS_AS(jobs::validate(j), Error);
    }
    SUBCASE("Hyper-V needs administrator rights") {
        jobs::Job j = job;
        j.wsl_distros.clear();
        j.hyperv_vms = {"Lab VM"};
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j.run_as_admin = true;
        CHECK_NOTHROW(jobs::validate(j));
        j.hyperv_staging = "E:\\Staging";
        CHECK_NOTHROW(jobs::validate(j));
        const jobs::Job b = jobs::job_from_json(jobs::to_json(j));
        CHECK(b.hyperv_vms == j.hyperv_vms);
        CHECK(b.hyperv_staging == "E:\\Staging");
        j.hyperv_staging = "\\\\server\\share";
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j.hyperv_staging = "relative\\folder";
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j.hyperv_staging.clear();
        j.hyperv_vms = {"*"};  // would match every machine
        CHECK_THROWS_AS(jobs::validate(j), Error);
    }
    SUBCASE("a job with nothing at all is still refused") {
        jobs::Job j = job;
        j.wsl_distros.clear();
        CHECK_THROWS_AS(jobs::validate(j), Error);
    }
}

TEST_CASE("streams and folders are backed up at their own places") {
    TempDir tmp;
    const fs::path repo_dir = tmp.path / "repo";
    {
        LocalBackend backend(u8(repo_dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, "pw", options);
    }
    auto repo = Repository::open(std::make_unique<LocalBackend>(u8(repo_dir)), "pw");

    const Bytes tar = random_data(3u << 20, 5);
    const fs::path staged = tmp.path / "staging" / "Lab";
    fs::create_directories(staged / "Virtual Hard Disks");
    { std::ofstream(staged / "Virtual Hard Disks" / "lab.vhdx", std::ios::binary) << "disk"; }

    BackupOptions o;  // no folders at all: only the extras
    o.extras.push_back(memory_source(wsl_place("Ubuntu"), tar));
    ExtraSource folder;
    folder.place = hyperv_place("Lab");
    folder.folder = u8(staged);
    o.extras.push_back(folder);
    o.extras.push_back(memory_source(wsl_place("Broken"), random_data(5000, 6), true));
    std::vector<std::string> warnings;
    o.on_warning = [&](const std::string& w) { warnings.push_back(w); };
    o.skip_unchanged = true;
    o.tags = {"job:vms"};
    // As run-job sets it: the staging area is kept out of the job's folders, but not out of the
    // exported folder stored from it.
    o.excludes = {u8(tmp.path / "staging") + std::string(1, vaultik::os::kSeparator) + "*"};

    const BackupResult first = run_backup(*repo, o);
    CHECK(first.snapshot.paths.empty());
    CHECK(first.snapshot.stats.files == 2);  // the tar and the disk; the broken export is left out
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("WSL/Broken.tar") != std::string::npos);
    CHECK(first.missing_sources == std::vector<std::string>{"WSL/Broken.tar"});

    Node node;
    REQUIRE(resolve_snapshot_path(*repo, first.snapshot, "WSL/Ubuntu.tar", node));
    CHECK(node.kind == NodeKind::File);
    CHECK(node.size == tar.size());
    CHECK_FALSE(resolve_snapshot_path(*repo, first.snapshot, "WSL/Broken.tar", node));
    REQUIRE(resolve_snapshot_path(*repo, first.snapshot, "Hyper-V/Lab/Virtual Hard Disks/lab.vhdx", node));
    CHECK(node.size == 4);

    // Restored as plain files.
    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    ro.subpath = "WSL/Ubuntu.tar";
    CHECK(run_restore(*repo, first.snapshot, ro).errors == 0);
    CHECK(read_all(tmp.path / "out" / "Ubuntu.tar") == tar);

    // The same export again: nothing new is stored, and the run counts as unchanged.
    o.extras.pop_back();
    const BackupResult second = run_backup(*repo, o);
    CHECK(second.unchanged);
    CHECK(second.snapshot.stats.new_bytes == 0);

    // A changed export is stored again, mostly deduplicated against the last one.
    Bytes changed = tar;
    for (std::size_t i = 0; i < 1000; ++i) changed[1'500'000 + i] ^= 0xFF;
    o.extras[0] = memory_source(wsl_place("Ubuntu"), changed);
    const BackupResult third = run_backup(*repo, o);
    CHECK_FALSE(third.unchanged);
    CHECK(third.snapshot.stats.new_bytes < (2u << 20));
    REQUIRE(resolve_snapshot_path(*repo, third.snapshot, "WSL/Ubuntu.tar", node));
    CHECK(node.size == changed.size());

    // A machine that could not be exported leaves no empty folder behind.
    BackupOptions failing;
    failing.extras.push_back(memory_source(wsl_place("Ubuntu"), tar));
    failing.extras.push_back(memory_source(hyperv_place("Gone"), {}, true));
    failing.on_warning = [](const std::string&) {};
    const BackupResult fourth = run_backup(*repo, failing);
    CHECK_FALSE(resolve_snapshot_path(*repo, fourth.snapshot, "Hyper-V", node));
    CHECK(fourth.missing_sources == std::vector<std::string>{"Hyper-V/Gone"});
}

#ifdef _WIN32
TEST_CASE("a program's output and input as streams") {
    TempDir tmp;
    std::string text;
    for (int i = 0; i < 20000; ++i) text += "line " + std::to_string(i) + " of the streamed output\r\n";
    { std::ofstream(tmp.path / "in.txt", std::ios::binary) << text; }

    SUBCASE("reading a program's output") {
        os::PipedProcess p({"cmd.exe", "/d", "/c", "type", u8(tmp.path / "in.txt")},
                           os::PipedProcess::Direction::FromProgram, {{"VAULTIK_TEST_VAR", "1"}});
        std::string got;
        std::uint8_t buf[100000];
        while (const std::size_t n = p.read(buf, sizeof buf)) got.append(reinterpret_cast<const char*>(buf), n);
        CHECK(p.wait() == 0);
        CHECK(got == text);
    }
    SUBCASE("writing a program's input") {
        os::PipedProcess p({"findstr.exe", "/v", "/c:never-in-the-text"}, os::PipedProcess::Direction::ToProgram);
        const std::string small = text.substr(0, text.rfind("\r\n", 30000) + 2);  // whole lines
        p.write(ByteSpan(reinterpret_cast<const std::uint8_t*>(small.data()), small.size()));
        CHECK(p.wait() == 0);
        CHECK(p.messages() == small);  // what it printed (its output is kept as messages)
    }
    SUBCASE("exit codes, messages and stopping") {
        os::PipedProcess fails({"cmd.exe", "/d", "/c", "echo oops 1>&2 & exit 3"}, os::PipedProcess::Direction::FromProgram);
        std::uint8_t buf[16];
        CHECK(fails.read(buf, sizeof buf) == 0);
        CHECK(fails.wait() == 3);
        CHECK(fails.messages().find("oops") != std::string::npos);

        const auto start = std::chrono::steady_clock::now();
        os::PipedProcess slow({"ping.exe", "-n", "30", "127.0.0.1"}, os::PipedProcess::Direction::FromProgram);
        slow.stop();
        CHECK(slow.wait() != 0);
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    }
}
#endif

TEST_CASE("Hyper-V exports left by a run that ended are recognized") {
    using std::chrono::hours;
    const auto alive = [](std::uint32_t pid) { return pid == 4242; };
    // The run's process is gone, still runs, or is this one.
    CHECK(hyperv_run_left_over("run-1234-0a1b2c3d4e5f", hours(0), 99, alive));
    CHECK_FALSE(hyperv_run_left_over("run-4242-0a1b2c3d4e5f", hours(2), 99, alive));
    CHECK_FALSE(hyperv_run_left_over("run-99-0a1b2c3d4e5f", hours(0), 99, alive));
    // A process id taken over by another program: after a week anyway.
    CHECK(hyperv_run_left_over("run-4242-0a1b2c3d4e5f", hours(24 * 7), 99, alive));
    // Before the process id was in the name: after a day.
    CHECK_FALSE(hyperv_run_left_over("run-0a1b2c3d4e5f", hours(23), 99, alive));
    CHECK(hyperv_run_left_over("run-0a1b2c3d4e5f", hours(24), 99, alive));
    // Anything else in the folder is never touched.
    CHECK_FALSE(hyperv_run_left_over("Exports", hours(1000), 99, alive));
    CHECK_FALSE(hyperv_run_left_over("run-x1-abc", hours(1000), 99, alive));
    CHECK_FALSE(hyperv_run_left_over("run-99999999999-abc", hours(1), 99, alive));
}
