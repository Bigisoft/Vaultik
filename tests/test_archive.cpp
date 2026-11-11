#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

#include "vaultik/jobs/job.hpp"
#include "vaultik/ops/backup.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/ops/restore.hpp"
#include "vaultik/ops/thaw.hpp"
#include "vaultik/ops/verify.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/local_backend.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/repo/s3_backend.hpp"
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

Bytes read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A folder that behaves like S3 archive storage: what is written with put_data cannot be read
// until it is thawed.
class ColdBackend final : public Backend {
public:
    explicit ColdBackend(const std::string& root) : inner_(root) {}

    std::set<std::string> cold;    // written with put_data
    std::set<std::string> thawed;  // readable although cold
    bool says_thaw = true;         // data_needs_thaw (false: archived without the job saying so)

    std::string location() const override { return inner_.location(); }
    bool exists(const std::string& name) override { return inner_.exists(name); }
    void put(const std::string& name, ByteSpan data) override {
        cold.erase(name);
        inner_.put(name, data);
    }
    Bytes get(const std::string& name) override {
        check(name);
        return inner_.get(name);
    }
    Bytes get_range(const std::string& name, std::uint64_t offset, std::size_t length) override {
        check(name);
        return inner_.get_range(name, offset, length);
    }
    std::uint64_t size(const std::string& name) override { return inner_.size(name); }
    std::vector<std::string> list(const std::string& dir) override { return inner_.list(dir); }
    void remove(const std::string& name) override {
        cold.erase(name);
        inner_.remove(name);
    }
    std::string data_class() const override { return "DEEP_ARCHIVE"; }
    bool data_needs_thaw() const override { return says_thaw; }
    void put_data(const std::string& name, ByteSpan data) override {
        inner_.put(name, data);
        cold.insert(name);
    }

private:
    void check(const std::string& name) const {
        if (cold.contains(name) && !thawed.contains(name)) throw ArchivedError(name + " is in archive storage");
    }
    LocalBackend inner_;
};

std::string snapshot_path(const fs::path& p) {
    std::string out;
    for (const std::string& c : os::path_components(u8(p))) out += (out.empty() ? "" : "/") + c;
    return out;
}

}  // namespace

TEST_CASE("storage class names") {
    CHECK(parse_storage_class("").empty());
    CHECK(parse_storage_class("standard").empty());
    CHECK(parse_storage_class("deep_archive") == "DEEP_ARCHIVE");
    CHECK(parse_storage_class("Glacier-IR") == "GLACIER_IR");
    CHECK(parse_storage_class("STANDARD_IA") == "STANDARD_IA");
    CHECK_THROWS_AS(parse_storage_class("COLD"), Error);
    CHECK(storage_class_needs_thaw("GLACIER"));
    CHECK(storage_class_needs_thaw("DEEP_ARCHIVE"));
    CHECK_FALSE(storage_class_needs_thaw("GLACIER_IR"));
    CHECK_FALSE(storage_class_needs_thaw(""));
}

TEST_CASE("S3 storage class header only on uploads of file data") {
    const std::string abc = "abc";
    const ByteSpan body(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size());
    CloudCredentials c("AKID", "secret");
    const auto has_class = [](const std::vector<std::pair<std::string, std::string>>& h, const std::string& v) {
        return std::any_of(h.begin(), h.end(), [&](const auto& x) { return x.first == "x-amz-storage-class" && x.second == v; });
    };
    CHECK(s3::put_headers("data/ab/ab12", body, c, true, 0).empty());  // standard: no header at all

    c.storage_class = "DEEP_ARCHIVE";
    CHECK(has_class(s3::put_headers("data/ab/ab12", body, c, true, 0), "DEEP_ARCHIVE"));
    CHECK(has_class(s3::put_headers("parity/ab/ab12", body, c, true, 0), "DEEP_ARCHIVE"));
    for (const char* name : {"config", "index/ab12", "snapshots/ab12", "keys/k", "locks/l", "parity/percent", "data/cd/cd34"})
        CHECK(s3::put_headers(name, body, c, false, 0).empty());

    // With Object Lock too: the lock headers, then the class; lock files are never locked.
    c.lock_days = 30;
    const auto both = s3::put_headers("data/ab/ab12", body, c, true, 0);
    REQUIRE(both.size() == 4);
    CHECK(both[0].first == "content-md5");
    CHECK(both[2].second == "1970-01-31T00:00:00Z");
    CHECK(has_class(both, "DEEP_ARCHIVE"));
    CHECK(s3::put_headers("index/ab12", body, c, false, 0).size() == 3);
    CHECK(s3::put_headers("locks/l", body, c, false, 0).empty());
}

TEST_CASE("S3 thaw requests and x-amz-restore") {
    CHECK(s3::restore_request(7, "standard") ==
          "<RestoreRequest xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><Days>7</Days>"
          "<GlacierJobParameters><Tier>Standard</Tier></GlacierJobParameters></RestoreRequest>");
    CHECK(s3::restore_request(1, "BULK").find("<Tier>Bulk</Tier>") != std::string::npos);
    CHECK(s3::restore_request(365, "Expedited").find("<Days>365</Days>") != std::string::npos);
    CHECK_THROWS_AS(s3::restore_request(0, "Standard"), Error);
    CHECK_THROWS_AS(s3::restore_request(7, "fast"), Error);

    s3::RestoreHeader h = s3::parse_restore_header("ongoing-request=\"true\"");
    CHECK(h.present);
    CHECK(h.ongoing);
    CHECK(h.expiry.empty());
    h = s3::parse_restore_header("ongoing-request=\"false\", expiry-date=\"Fri, 21 Dec 2012 00:00:00 GMT\"");
    CHECK(h.present);
    CHECK_FALSE(h.ongoing);
    CHECK(h.expiry == "Fri, 21 Dec 2012 00:00:00 GMT");
    CHECK_FALSE(s3::parse_restore_header("").present);
}

TEST_CASE("jobs keep the storage class, older job files have none") {
    jobs::Job j;
    j.id = "cold";
    j.name = "Cold";
    j.repository = "s3:https://s3.eu-west-1.amazonaws.com/bucket/pc";
    j.cloud_key_id = "AKID";
    j.sources = {"C:/Users/me"};
    j.cloud_storage_class = "DEEP_ARCHIVE";
    j.second_copy.repository = "s3:https://s3.eu-west-1.amazonaws.com/other/pc";
    j.second_copy.cloud_key_id = "AKID";
    j.second_copy.cloud_storage_class = "GLACIER";
    CHECK_NOTHROW(jobs::validate(j));
    const jobs::Job back = jobs::job_from_json(jobs::to_json(j));
    CHECK(back.cloud_storage_class == "DEEP_ARCHIVE");
    CHECK(back.second_copy.cloud_storage_class == "GLACIER");

    auto old = nlohmann::json::parse(jobs::to_json(j));
    old.erase("cloud_storage_class");
    old["second_copy"].erase("cloud_storage_class");
    const jobs::Job older = jobs::job_from_json(old.dump());
    CHECK(older.cloud_storage_class.empty());
    CHECK(older.second_copy.cloud_storage_class.empty());

    j.cloud_storage_class = "deep_archive";  // not as S3 names it
    CHECK_THROWS_AS(jobs::validate(j), Error);
    j.cloud_storage_class = "DEEP_ARCHIVE";
    j.repository = "E:/Backups";
    CHECK_THROWS_AS(jobs::validate(j), Error);  // a folder has no storage classes
}

TEST_CASE("archive storage: file data goes cold, listings stay readable") {
    TempDir tmp;
    const fs::path src = tmp.path / "src";
    const fs::path repo_dir = tmp.path / "repo";
    const Bytes a = random_data(3u << 20, 11);
    const Bytes b = random_data(3u << 20, 12);
    const Bytes c = random_data(3u << 20, 13);
    {
        LocalBackend backend(u8(repo_dir), true);
        InitOptions options;
        options.kdf_iterations = 1000;
        Repository::init(backend, kPassword, options);
    }
    auto owned = std::make_unique<ColdBackend>(u8(repo_dir));
    ColdBackend* cold = owned.get();
    auto repo = Repository::open(std::move(owned), kPassword);
    CHECK(repo->splits_trees());
    repo->set_parity_percent(5);

    write_file(src / "a.bin", a);
    write_file(src / "b.bin", b);
    BackupOptions bo;
    bo.paths = {u8(src)};
    const BackupResult first = run_backup(*repo, bo);
    fs::remove(src / "b.bin");
    write_file(src / "c.bin", c);
    const BackupResult second = run_backup(*repo, bo);

    // Only packs of file data and their repair data went to the archive class.
    std::size_t cold_packs = 0;
    for (const std::string& name : cold->cold) {
        CHECK((name.starts_with("data/") || name.starts_with("parity/")));
        CHECK(name != "parity/percent");
        if (name.starts_with("data/")) ++cold_packs;
    }
    CHECK(cold_packs == 2);  // a and b, then c
    CHECK(cold->cold.size() == 4);
    const std::vector<bool> archived = repo->archived_packs();
    CHECK(static_cast<std::size_t>(std::count(archived.begin(), archived.end(), true)) == cold_packs);
    CHECK(archived.size() > cold_packs);  // the folder listings' own packs

    // Listings and versions can be read; a restore stops with the thaw error.
    CHECK(repo->list_snapshot_ids().size() == 2);
    Node node;
    CHECK(resolve_snapshot_path(*repo, second.snapshot, snapshot_path(src / "a.bin"), node));
    RestoreOptions ro;
    ro.target = u8(tmp.path / "out");
    CHECK_THROWS_AS(run_restore(*repo, second.snapshot, ro), ArchivedError);

    // A health check reads no archived data and finds no damage.
    VerifyOptions vo;
    vo.read_data = true;
    VerifyResult vr = run_verify(*repo, vo);
    CHECK(vr.errors == 0);
    CHECK(vr.packs_archived == cold_packs);
    CHECK(vr.snapshots == 2);
    // The same when the backend did not say its data is archived: reading fails, which is no damage.
    cold->says_thaw = false;
    vr = run_verify(*repo, vo);
    CHECK(vr.errors == 0);
    CHECK(vr.packs_archived == cold_packs);
    cold->says_thaw = true;

    // The packs a thaw asks for: all of the version's, or only those of one file.
    const std::vector<Id> all = packs_for_restore(*repo, second.snapshot, {});
    CHECK(all.size() == 2);
    const auto pack_of = [&](const fs::path& file, const Snapshot& snap) {
        Node n;
        REQUIRE(resolve_snapshot_path(*repo, snap, snapshot_path(file), n));
        REQUIRE_FALSE(n.content.empty());
        return repo->index().packs()[repo->index().find(n.content[0])->pack];
    };
    const Id c_pack = pack_of(src / "c.bin", second.snapshot);
    const Id b_pack = pack_of(src / "b.bin", first.snapshot);
    CHECK(c_pack != b_pack);
    // (A file's permissions may be stored once for all files, in the first pack.)
    const std::vector<Id> only_c = packs_for_restore(*repo, second.snapshot, {snapshot_path(src / "c.bin")});
    CHECK(std::find(only_c.begin(), only_c.end(), c_pack) != only_c.end());
    for (const Id& id : only_c) CHECK(std::find(all.begin(), all.end(), id) != all.end());
    const std::vector<Id> only_b = packs_for_restore(*repo, first.snapshot, {snapshot_path(src / "b.bin")});
    REQUIRE(only_b.size() == 1);
    CHECK(only_b[0] == b_pack);
    CHECK_THROWS_AS(packs_for_restore(*repo, second.snapshot, {snapshot_path(src / "b.bin")}), Error);
    for (const Id& id : all) CHECK(cold->cold.contains(Repository::pack_name(id)));

    // Thawed, it restores.
    for (const Id& id : all) cold->thawed.insert(Repository::pack_name(id));
    CHECK(run_restore(*repo, second.snapshot, ro).errors == 0);
    fs::path restored = tmp.path / "out";
    for (const std::string& comp : os::path_components(u8(src))) restored /= fs::path(std::u8string(comp.begin(), comp.end()));
    CHECK(read_file(restored / "a.bin") == a);
    CHECK(read_file(restored / "c.bin") == c);
    cold->thawed.clear();

    // Prune deletes nothing it would have to read: the first pack (a in use, b not) stays as it is.
    ForgetOptions fo;
    fo.policy.last = 1;
    CHECK(run_forget(*repo, fo).removed == 1);
    PruneOptions po;
    po.repack_unused = 0.0;
    PruneResult pr;
    CHECK_NOTHROW(pr = run_prune(*repo, po));
    CHECK(pr.blobs_unused > 0);
    for (const Id& id : all) CHECK(repo->backend().exists(Repository::pack_name(id)));
}
