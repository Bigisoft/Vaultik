// Runs cut short by a failing drive: a write that fails, one cut short, a rename that does not
// happen, a delete or a read that fails. Whatever the run was doing, the repository must stay
// consistent (every version reads back), and the next run must work and leave it whole.

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <random>

#include "faulty_backend.hpp"
#include "vaultik/jobs/check_state.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/util/hex.hpp"

using namespace vaultik;
using test::FaultyBackend;
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
        path = fs::temp_directory_path() / ("vaultik-faults-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_random(const fs::path& p, std::size_t n, std::uint32_t seed) {
    fs::create_directories(p.parent_path());
    std::mt19937 rng(seed);
    std::string b(n, '\0');
    for (auto& x : b) x = static_cast<char>(rng());
    std::ofstream(p, std::ios::binary) << b;
}

std::unique_ptr<Repository> open_plain(const fs::path& dir) {
    return Repository::open(std::make_unique<LocalBackend>(u8(dir)), kPassword);
}

std::unique_ptr<Repository> open_faulty(const fs::path& dir, FaultyBackend::Fault fault, int at, FaultyBackend** out) {
    auto backend = std::make_unique<FaultyBackend>(std::make_unique<LocalBackend>(u8(dir)), fault, at);
    *out = backend.get();
    return Repository::open(std::move(backend), kPassword);
}

// Every version reads back whole, with every stored chunk.
void check_whole(const fs::path& dir, const char* what) {
    INFO(what);
    auto repo = open_plain(dir);
    VerifyOptions vo;
    vo.read_data = true;
    const VerifyResult v = run_verify(*repo, vo);
    CHECK(v.errors == 0);
    CHECK(v.snapshots == repo->list_snapshot_ids().size());
}

// A repository with two versions of `src`.
void make_base(const fs::path& repo_dir, const fs::path& src) {
    {
        LocalBackend backend(u8(repo_dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, kPassword, options);
    }
    auto repo = open_plain(repo_dir);
    BackupOptions o;
    o.paths = {u8(src)};
    write_random(src / "a.bin", 300u << 10, 1);
    run_backup(*repo, o);
    write_random(src / "b.bin", 300u << 10, 2);
    run_backup(*repo, o);
}

const char* fault_name(FaultyBackend::Fault f) {
    switch (f) {
        case FaultyBackend::Fault::FailWrite: return "write fails";
        case FaultyBackend::Fault::TruncateWrite: return "write cut short";
        case FaultyBackend::Fault::FailRename: return "rename fails";
        case FaultyBackend::Fault::FailRemove: return "delete fails";
        case FaultyBackend::Fault::FailRead: return "read fails";
        case FaultyBackend::Fault::None: break;
    }
    return "none";
}

}  // namespace

TEST_CASE("faults: a backup cut short leaves the repository whole, and the next backup works") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path base = tmp.path / "base";
    make_base(base, src);
    write_random(src / "c.bin", 600u << 10, 3);  // what the interrupted backup adds

    for (const auto fault : {FaultyBackend::Fault::FailWrite, FaultyBackend::Fault::TruncateWrite,
                             FaultyBackend::Fault::FailRename}) {
        for (int at = 1; at <= 6; ++at) {
            const std::string what = std::string(fault_name(fault)) + " at write " + std::to_string(at);
            CAPTURE(what);
            const fs::path dir = tmp.path / ("r" + std::to_string(static_cast<int>(fault)) + "-" + std::to_string(at));
            fs::copy(base, dir, fs::copy_options::recursive);
            {
                FaultyBackend* faulty = nullptr;
                auto repo = open_faulty(dir, fault, at, &faulty);
                BackupOptions o;
                o.paths = {u8(src)};
                bool threw = false;
                try {
                    run_backup(*repo, o);
                } catch (const std::exception&) {
                    threw = true;
                }
                CHECK(threw == faulty->fired());
            }
            check_whole(dir, "after the interrupted backup");
            // The next backup, with the drive back.
            {
                auto repo = open_plain(dir);
                BackupOptions o;
                o.paths = {u8(src)};
                const BackupResult r = run_backup(*repo, o);
                CHECK(r.snapshot.stats.errors == 0);
                CHECK(r.snapshot.stats.files == 3);
            }
            check_whole(dir, "after the next backup");
            fs::remove_all(dir);
        }
    }
}

TEST_CASE("faults: a prune cut short leaves every kept version whole, and the next prune finishes") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path base = tmp.path / "base";
    make_base(base, src);
    {
        // A third version without a.bin, then forget the first two: a prune has packs to drop
        // and to rewrite.
        fs::remove(src / "a.bin");
        write_random(src / "c.bin", 300u << 10, 3);
        auto repo = open_plain(base);
        BackupOptions o;
        o.paths = {u8(src)};
        run_backup(*repo, o);
        ForgetOptions fo;
        fo.policy.last = 1;
        REQUIRE(run_forget(*repo, fo).removed == 2);
    }
    for (const auto fault : {FaultyBackend::Fault::FailWrite, FaultyBackend::Fault::TruncateWrite,
                             FaultyBackend::Fault::FailRename, FaultyBackend::Fault::FailRemove}) {
        for (int at = 1; at <= 4; ++at) {
            const std::string what = std::string(fault_name(fault)) + " at " + std::to_string(at);
            CAPTURE(what);
            const fs::path dir = tmp.path / ("p" + std::to_string(static_cast<int>(fault)) + "-" + std::to_string(at));
            fs::copy(base, dir, fs::copy_options::recursive);
            {
                FaultyBackend* faulty = nullptr;
                auto repo = open_faulty(dir, fault, at, &faulty);
                bool threw = false;
                try {
                    run_prune(*repo, PruneOptions{});
                } catch (const std::exception&) {
                    threw = true;
                }
                if (fault != FaultyBackend::Fault::FailRemove) CHECK(threw == faulty->fired());
            }
            check_whole(dir, "after the interrupted prune");
            {
                auto repo = open_plain(dir);
                run_prune(*repo, PruneOptions{});
                RestoreOptions ro;
                ro.target = u8(dir / "restored");
                const RestoreResult rr = run_restore(*repo, repo->load_snapshot(repo->list_snapshot_ids().at(0)), ro);
                CHECK(rr.errors == 0);
                CHECK(rr.files == 2);
            }
            check_whole(dir, "after the next prune");
            fs::remove_all(dir);
        }
    }
}

TEST_CASE("faults: a health check cut short by a failing read changes nothing, and the next one covers all") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path dir = tmp.path / "repo";
    make_base(dir, src);
    jobs::PackTimes state;
    std::size_t packs = 0;
    for (int at = 1; at <= 2; ++at) {
        CAPTURE(at);
        state.clear();
        FaultyBackend* faulty = nullptr;
        auto repo = open_faulty(dir, FaultyBackend::Fault::FailRead, at, &faulty);
        packs = repo->index().packs().size();
        VerifyOptions vo;
        vo.read_data = true;
        vo.add_parity = 5;  // a check also writes repair data: none may be half there
        vo.pack_state = &state;
        vo.now = 100;
        vo.fresh_ticks = 1000;
        try {
            run_verify(*repo, vo);
        } catch (const std::exception&) {
        }
        CHECK(faulty->fired());
        CHECK(state.size() == packs - 1);  // the pack it could not read is not taken as checked
    }
    check_whole(dir, "after the interrupted checks");
    auto repo = open_plain(dir);
    VerifyOptions vo;
    vo.read_data = true;
    vo.pack_state = &state;
    vo.now = 200;
    vo.fresh_ticks = 1000;
    CHECK(run_verify(*repo, vo).errors == 0);
    CHECK(state.size() == packs);
}
