// Windows extras: security descriptors, alternate data streams and junctions survive a backup and
// a restore.
#ifdef _WIN32

#include <doctest/doctest.h>

#include <windows.h>

#include <sddl.h>

#include <filesystem>
#include <fstream>

#include "vaultik/crypto/crypto.hpp"
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

#endif  // _WIN32
