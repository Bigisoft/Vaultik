#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/ops/databases.hpp"
#include "vaultik/ops/docker.hpp"
#include "vaultik/platform/os.hpp"
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
        path = fs::temp_directory_path() / ("vaultik-containers-test-" + to_hex(rnd));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// Whether any argument holds `text` (a password must never be on a command line).
bool any_has(const std::vector<std::string>& v, const std::string& text) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& a) { return a.find(text) != std::string::npos; });
}

DatabaseDump sample(DatabaseKind kind) {
    DatabaseDump db;
    db.id = "a1b2c3";
    db.kind = kind;
    db.database = "shop";
    return db;
}

}  // namespace

TEST_CASE("Docker volume names and version paths") {
    CHECK(valid_volume_name("pgdata"));
    CHECK(valid_volume_name("acumatica_prod_files"));
    CHECK(valid_volume_name("my-app.data"));
    CHECK(valid_volume_name("9a"));
    CHECK_FALSE(valid_volume_name("a"));  // Docker wants two characters at least
    CHECK_FALSE(valid_volume_name(""));
    CHECK_FALSE(valid_volume_name("-v"));
    CHECK_FALSE(valid_volume_name("_x"));
    CHECK_FALSE(valid_volume_name("a b"));
    CHECK_FALSE(valid_volume_name("a/b"));
    CHECK_FALSE(valid_volume_name("a:b"));
    CHECK_FALSE(valid_volume_name(std::string(256, 'a')));
    CHECK(valid_container_name("postgres-1"));
    CHECK_FALSE(valid_container_name("--privileged"));

    CHECK(volume_place("pgdata") == std::vector<std::string>{"Docker", "pgdata.tar"});
    CHECK(volume_snapshot_path("pgdata") == "Docker/pgdata.tar");
    CHECK(volume_of_path("Docker/pgdata.tar") == "pgdata");
    CHECK(volume_of_path("\\Docker\\my-app.data.tar") == "my-app.data");
    CHECK(volume_of_path("Docker/pgdata.tar/x") == "");
    CHECK(volume_of_path("C/Docker/pgdata.tar") == "");
    CHECK(volume_of_path("Docker/a b.tar") == "");
    CHECK(volume_of_path("WSL/Ubuntu.tar") == "");
}

TEST_CASE("what docker prints is read") {
    const auto volumes = parse_volume_list("pgdata\tlocal\r\nacumatica_prod_files\tlocal\n\nbad name\tlocal\nweb\n");
    REQUIRE(volumes.size() == 3);
    CHECK(volumes[0].name == "acumatica_prod_files");  // sorted
    CHECK(volumes[1].name == "pgdata");
    CHECK(volumes[1].driver == "local");
    CHECK(volumes[2].name == "web");
    CHECK(volumes[2].driver.empty());

    // Bind-mounted folders are paths, which are no volume names; a volume used twice lists both.
    const auto mounts = parse_container_mounts(
        "acu_dev\td:\\programming\\acumatica\\site,acumatica_prod_files\n"
        "db\tpgdata,/home/me/conf\n"
        "db2\tpgdata\n"
        "idle\t\n");
    CHECK(mounts.size() == 2);
    CHECK(mounts.at("acumatica_prod_files") == std::vector<std::string>{"acu_dev"});
    CHECK(mounts.at("pgdata") == std::vector<std::string>{"db", "db2"});

    CHECK(parse_image_list("busybox:latest\n<none>:<none>\r\nmcr.microsoft.com/windows/nanoserver:ltsc2025\n") ==
          std::vector<std::string>{"busybox:latest", "mcr.microsoft.com/windows/nanoserver:ltsc2025"});

    CHECK(docker_problem("error during connect: Get \"http://%2F%2F.%2Fpipe%2FdockerDesktopLinuxEngine/v1.47/info\": "
                         "open //./pipe/dockerDesktopLinuxEngine: The system cannot find the file specified.",
                         1)
              .starts_with("Docker is not running"));
    CHECK(docker_problem("Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon "
                         "running?",
                         1)
              .starts_with("Docker is not running"));
    CHECK(docker_problem("permission denied while trying to connect to the Docker daemon socket", 1).find("docker group") !=
          std::string::npos);
    CHECK(docker_problem("\nError response from daemon: get nope: no such volume\n", 1) ==
          "Error response from daemon: get nope: no such volume");
    CHECK(docker_problem("", 3) == "docker failed (exit code 3)");
}

TEST_CASE("the image that runs tar is one already on this computer") {
    const std::vector<std::string> images = {"acumatica:26.200", "docker.io/library/alpine:3.20", "busybox:1.36",
                                             "mcr.microsoft.com/windows/servercore:ltsc2022",
                                             "mcr.microsoft.com/windows/nanoserver:ltsc2025"};
    CHECK(pick_helper_image(images, "linux") == "busybox:1.36");  // busybox before alpine
    CHECK(pick_helper_image({"localhost:5000/busybox:1", "alpine:3"}, "linux") == "alpine:3");  // another registry
    CHECK(pick_helper_image({"docker.io/library/alpine:3.20"}, "linux") == "docker.io/library/alpine:3.20");
    CHECK(pick_helper_image(images, "windows") == "mcr.microsoft.com/windows/nanoserver:ltsc2025");
    CHECK(pick_helper_image(images, "linux", "my/tools:1") == "my/tools:1");  // the job's own
    CHECK_THROWS_WITH_AS(pick_helper_image({"acumatica:26.200"}, "linux"), doctest::Contains("docker pull busybox"),
                         Error);
    CHECK_THROWS_WITH_AS(pick_helper_image({"busybox:1"}, "windows"), doctest::Contains("nanoserver"), Error);
}

TEST_CASE("Docker command lines for exporting and importing a volume") {
    const auto ex = volume_export_command("docker", "pgdata", "busybox:1.36", "linux", "vaultik-export-1");
    CHECK(ex == std::vector<std::string>{"docker", "run", "--rm", "--pull", "never", "--network", "none",
                                         "--log-driver", "none", "--name", "vaultik-export-1", "-v",
                                         "pgdata:/vaultik-volume:ro", "--entrypoint", "tar", "--user", "0:0",
                                         "busybox:1.36", "-c", "-f", "-", "-C", "/vaultik-volume", "."});
    const auto im = volume_import_command("docker", "pgdata-restored", "busybox:1.36", "linux", "vaultik-import-1");
    CHECK(contains(im, "-i"));
    CHECK(contains(im, "pgdata-restored:/vaultik-volume"));  // writable, only the new volume
    CHECK(!contains(im, "pgdata:/vaultik-volume"));
    CHECK(std::vector<std::string>(im.end() - 5, im.end()) ==
          std::vector<std::string>{"-x", "-f", "-", "-C", "/vaultik-volume"});

    const auto win = volume_export_command("docker.exe", "data", "nano:1", "windows", "c1");
    CHECK(contains(win, "data:C:\\vaultik-volume:ro"));
    CHECK(!contains(win, "--user"));
    CHECK(win.back() == ".");

    const std::string name = helper_container_name("export");
    CHECK(name.starts_with("vaultik-export-"));
    CHECK(valid_container_name(name));
    CHECK(name != helper_container_name("export"));

    DockerVolume v{"pgdata", "local", {}};
    CHECK(volume_in_use_warning(v).empty());
    v.running = {"db", "admin"};
    const std::string w = volume_in_use_warning(v);
    CHECK(w.find("pgdata") != std::string::npos);
    CHECK(w.find("containers db, admin") != std::string::npos);
    CHECK(w.find("database dumps") != std::string::npos);
}

TEST_CASE("database dump specs") {
    const DatabaseDump pg = parse_database_spec("postgres://app@db.local:5433/shop");
    CHECK(pg.kind == DatabaseKind::Postgres);
    CHECK(pg.user == "app");
    CHECK(pg.host == "db.local");
    CHECK(pg.port == 5433);
    CHECK(pg.database == "shop");
    CHECK(valid_database_id(pg.id));

    const DatabaseDump ms = parse_database_spec("sqlserver://.\\SQLEXPRESS/Sales%20DB?name=sales");
    CHECK(ms.kind == DatabaseKind::SqlServer);
    CHECK(ms.host == ".\\SQLEXPRESS");
    CHECK(ms.user.empty());
    CHECK(ms.database == "Sales DB");
    CHECK(ms.name == "sales");

    const DatabaseDump my = parse_database_spec("mariadb://root@/wp?container=wordpress-db");
    CHECK(my.kind == DatabaseKind::MySql);
    CHECK(my.host.empty());
    CHECK(my.container == "wordpress-db");

    const DatabaseDump v6 = parse_database_spec("postgres://[::1]:5432/x");
    CHECK(v6.host == "[::1]");
    CHECK(v6.port == 5432);

    // Round trip through the spec.
    DatabaseDump odd = sample(DatabaseKind::MySql);
    odd.user = "a@b:c";
    odd.host = "h";
    odd.port = 3307;
    odd.database = "x/y?z";
    odd.container = "db1";
    odd.name = "nightly & full";
    DatabaseDump back = parse_database_spec(database_spec(odd));
    back.id = odd.id;
    CHECK(back == odd);

    CHECK_THROWS_AS(parse_database_spec("shop"), Error);
    CHECK_THROWS_AS(parse_database_spec("oracle://x/y"), Error);
    CHECK_THROWS_AS(parse_database_spec("postgres://host/"), Error);
    CHECK_THROWS_AS(parse_database_spec("postgres://host:99999/x"), Error);
    CHECK_THROWS_AS(parse_database_spec("postgres://host:abc/x"), Error);
    CHECK_THROWS_AS(parse_database_spec("postgres://host/x?user=y"), Error);
    CHECK_THROWS_AS(parse_database_spec("postgres://host/--help"), Error);  // would be an option
    CHECK_THROWS_AS(parse_database_spec("postgres://-h/x"), Error);
    CHECK_THROWS_AS(parse_database_spec("postgres://host/a=b"), Error);   // a connection string
    CHECK_THROWS_AS(parse_database_spec("sqlserver://localhost/db?container=mssql"), Error);  // needs a user
    CHECK_THROWS_AS(parse_database_spec("postgres://host/x?container=bad%20name"), Error);
}

TEST_CASE("database dump names, checks and descriptions") {
    DatabaseDump db = sample(DatabaseKind::SqlServer);
    CHECK(dump_file_name(db) == "shop.bak");
    db.kind = DatabaseKind::MySql;
    CHECK(dump_file_name(db) == "shop.sql");
    db.kind = DatabaseKind::Postgres;
    CHECK(dump_file_name(db) == "shop.dump");
    db.name = "a:b";
    CHECK(dump_file_name(db).starts_with("a_b-"));  // made a file name, marked as changed
    CHECK(database_place(db) == std::vector<std::string>{"Databases", dump_file_name(db)});

    DatabaseDump other = sample(DatabaseKind::Postgres);
    other.id = "d4e5f6";
    DatabaseDump first = sample(DatabaseKind::Postgres);
    CHECK_THROWS_WITH_AS(check_databases({first, other}), doctest::Contains("shop.dump"), Error);
    other.name = "shop-copy";
    CHECK_NOTHROW(check_databases({first, other}));
    other.id = first.id;
    CHECK_THROWS_AS(check_databases({first, other}), Error);
    // The same database of two kinds has two file names.
    other = sample(DatabaseKind::MySql);
    other.id = "x9";
    CHECK_NOTHROW(check_databases({first, other}));

    DatabaseDump bad = sample(DatabaseKind::Postgres);
    bad.id = "Bad-Id";
    CHECK_THROWS_AS(check_database(bad), Error);
    bad = sample(DatabaseKind::Postgres);
    bad.database.clear();
    CHECK_THROWS_AS(check_database(bad), Error);
    bad = sample(DatabaseKind::Postgres);
    bad.user = "-U root";
    CHECK_THROWS_AS(check_database(bad), Error);
    bad = sample(DatabaseKind::Postgres);
    bad.database = "a\nb";
    CHECK_THROWS_AS(check_database(bad), Error);
    bad = sample(DatabaseKind::Postgres);
    bad.port = 70000;
    CHECK_THROWS_AS(check_database(bad), Error);
    // A PostgreSQL "name" that pg_dump would take as a connection to another server.
    for (const char* uri : {"postgresql://evil.example/db", "postgres://evil.example/db", "PostgreSQL://x/db"}) {
        bad = sample(DatabaseKind::Postgres);
        bad.database = uri;
        CHECK_THROWS_WITH_AS(check_database(bad), doctest::Contains("connection URI"), Error);
    }
    bad = sample(DatabaseKind::Postgres);
    bad.database = "host=evil.example dbname=x";
    CHECK_THROWS_AS(check_database(bad), Error);
    // The same name is fine for MySQL, which takes it as a name only.
    bad = sample(DatabaseKind::MySql);
    bad.database = "postgres://x";
    CHECK_NOTHROW(check_database(bad));

    DatabaseDump d = sample(DatabaseKind::Postgres);
    CHECK(describe(d) == "PostgreSQL shop on this computer");
    d.host = "db";
    d.port = 5433;
    d.user = "app";
    d.container = "pg";
    CHECK(describe(d) == "PostgreSQL shop on db:5433 as app, in the container pg");

    CHECK(parse_database_kind("MSSQL") == DatabaseKind::SqlServer);
    CHECK(parse_database_kind("postgresql") == DatabaseKind::Postgres);
    CHECK(std::string(to_string(DatabaseKind::MySql)) == "mysql");
    CHECK_THROWS_AS(parse_database_kind("sqlite"), Error);
    CHECK(default_port(DatabaseKind::Postgres) == 5432);
}

TEST_CASE("database dump commands never put the password on the command line") {
    const std::string pw = "s3cr:et\\\"pw";
    const std::string out = "/tmp/run/shop.dump";

    SUBCASE("PostgreSQL on this computer: a private password file") {
        DatabaseDump db = sample(DatabaseKind::Postgres);
        db.host = "localhost";
        db.port = 5433;
        db.user = "backup";
        const DumpCommand c = dump_command(db, pw, out, "/tmp/private");
        CHECK(c.argv.front() == "pg_dump");
        CHECK(contains(c.argv, "--format=custom"));
        CHECK(contains(c.argv, "--no-password"));
        CHECK(contains(c.argv, "--file=" + out));
        CHECK(contains(c.argv, "--host=localhost"));
        CHECK(contains(c.argv, "--port=5433"));
        CHECK(contains(c.argv, "--username=backup"));
        CHECK(c.argv.back() == "--dbname=shop");
        CHECK(!c.to_stdout);
        CHECK(!any_has(c.argv, "s3cr"));
        CHECK(c.secret_file == os::join("/tmp/private", "pgpass-a1b2c3"));
        CHECK(c.secret_text == "*:*:*:*:s3cr\\:et\\\\\"pw\n");
        CHECK(contains({c.env.front().first}, "PGPASSFILE"));
        CHECK(c.env.front().second == c.secret_file);
    }
    SUBCASE("PostgreSQL in a container: docker exec -e with the value in docker's own environment") {
        DatabaseDump db = sample(DatabaseKind::Postgres);
        db.container = "pg";
        const DumpCommand c = dump_command(db, pw, out, "/tmp/private");
        CHECK(std::vector<std::string>(c.argv.begin(), c.argv.begin() + 6) ==
              std::vector<std::string>{"docker", "exec", "-e", "PGPASSWORD", "pg", "pg_dump"});
        CHECK(contains(c.argv, "--username=postgres"));  // root has no role in the official image
        CHECK(!any_has(c.argv, "--file"));
        CHECK(c.to_stdout);
        CHECK(!any_has(c.argv, "s3cr"));
        REQUIRE(c.env.size() == 1);
        CHECK(c.env[0] == std::pair<std::string, std::string>{"PGPASSWORD", pw});
        CHECK(c.secret_file.empty());
    }
    SUBCASE("MySQL on this computer: an option file read first") {
        DatabaseDump db = sample(DatabaseKind::MySql);
        db.user = "root";
        const DumpCommand c = dump_command(db, pw, out, "/tmp/private");
        CHECK(c.argv[0] == "mysqldump");
        CHECK(c.argv[1] == "--defaults-extra-file=" + os::join("/tmp/private", "mysql-a1b2c3.cnf"));
        CHECK(contains(c.argv, "--single-transaction"));
        CHECK(contains(c.argv, "--result-file=" + out));
        CHECK(contains(c.argv, "--user=root"));
        CHECK(c.argv.back() == "shop");
        CHECK(!any_has(c.argv, "s3cr"));
        CHECK(c.secret_text == "[client]\npassword=\"s3cr:et\\\\\\\"pw\"\n");
        CHECK(c.env.empty());
    }
    SUBCASE("MySQL without a password has no option file") {
        const DumpCommand c = dump_command(sample(DatabaseKind::MySql), "", out, "/tmp/private");
        CHECK(c.secret_file.empty());
        CHECK(!any_has(c.argv, "--defaults-extra-file"));
        CHECK(!any_has(c.argv, "--user"));
    }
    SUBCASE("MySQL in a container: mysqldump or mariadb-dump, MYSQL_PWD from docker's environment") {
        DatabaseDump db = sample(DatabaseKind::MySql);
        db.container = "wordpress-db";
        const DumpCommand c = dump_command(db, pw, out, "/tmp/private");
        CHECK(std::vector<std::string>(c.argv.begin(), c.argv.begin() + 7) ==
              std::vector<std::string>{"docker", "exec", "-e", "MYSQL_PWD", "wordpress-db", "sh", "-c"});
        CHECK(c.argv[7].find("mariadb-dump mysqldump") != std::string::npos);
        CHECK(c.argv[8] == "vaultik");  // $0; the arguments that follow are "$@", never shell text
        CHECK(contains(c.argv, "--user=root"));
        CHECK(c.argv.back() == "shop");
        CHECK(c.to_stdout);
        CHECK(!any_has(c.argv, "s3cr"));
        CHECK(c.env == std::vector<std::pair<std::string, std::string>>{{"MYSQL_PWD", pw}});
    }
    SUBCASE("SQL Server on this computer: a copy-only backup, Windows sign-in or SQLCMDPASSWORD") {
        DatabaseDump db = sample(DatabaseKind::SqlServer);
        db.host = ".\\SQLEXPRESS";
        db.database = "Shop]; DROP DATABASE x; --";
        const DumpCommand c = dump_command(db, "", "C:\\T\\it's.bak", "C:\\T");
        CHECK(c.argv[0] == "sqlcmd");
        CHECK(contains(c.argv, ".\\SQLEXPRESS"));
        CHECK(contains(c.argv, "-E"));
        CHECK(contains(c.argv, "-b"));
        CHECK(contains(c.argv, "-x"));  // no $(variables)
        CHECK(c.argv.back() ==
              "BACKUP DATABASE [Shop]]; DROP DATABASE x; --] TO DISK = N'C:\\T\\it''s.bak' WITH COPY_ONLY, INIT");
        CHECK(c.env.empty());

        db.user = "sa";
        db.port = 1434;
        db.host = "localhost";
        const DumpCommand u = dump_command(db, pw, "C:\\T\\x.bak", "C:\\T");
        CHECK(contains(u.argv, "localhost,1434"));
        CHECK(contains(u.argv, "-U"));
        CHECK(!contains(u.argv, "-E"));
        CHECK(!any_has(u.argv, "s3cr"));
        CHECK(u.env == std::vector<std::pair<std::string, std::string>>{{"SQLCMDPASSWORD", pw}});
    }
    SUBCASE("SQL Server in a container: backed up inside it, then copied out") {
        DatabaseDump db = sample(DatabaseKind::SqlServer);
        db.container = "mssql";
        db.user = "sa";
        const DumpCommand c = dump_command(db, pw, out, "/tmp/private", "/tmp/vaultik-1.bak");
        CHECK(std::vector<std::string>(c.argv.begin(), c.argv.begin() + 7) ==
              std::vector<std::string>{"docker", "exec", "-e", "SQLCMDPASSWORD", "mssql", "sh", "-c"});
        CHECK(c.argv[7].find("/opt/mssql-tools18/bin/sqlcmd") != std::string::npos);
        CHECK(c.container_file == "/tmp/vaultik-1.bak");
        CHECK(c.argv.back().find("TO DISK = N'/tmp/vaultik-1.bak' WITH COPY_ONLY") != std::string::npos);
        CHECK(!c.to_stdout);
        CHECK(!any_has(c.argv, "s3cr"));
    }
}

TEST_CASE("SQL Server's service and whether a server is this computer") {
    CHECK(sqlserver_service("") == "MSSQLSERVER");
    CHECK(sqlserver_service("localhost") == "MSSQLSERVER");
    CHECK(sqlserver_service(".\\SQLEXPRESS") == "MSSQL$SQLEXPRESS");
    CHECK(sqlserver_service("pc\\dev,1500") == "MSSQL$DEV");
    CHECK(sqlserver_service("pc\\MSSQLSERVER") == "MSSQLSERVER");

    for (const char* local : {"", ".", "(local)", "localhost", "LOCALHOST\\SQLEXPRESS", "127.0.0.1,1433",
                              "tcp:localhost", "MyPC", "mypc\\inst"})
        CHECK_MESSAGE(is_local_server(local, "MYPC"), local);
    CHECK_FALSE(is_local_server("dbserver", "MYPC"));
    CHECK_FALSE(is_local_server("10.0.0.5\\SQLEXPRESS", "MYPC"));
}

TEST_CASE("programs are found on PATH and in their install folders") {
    TempDir tmp;
#ifdef _WIN32
    const std::string exe = "vaultiktool.exe";
#else
    const std::string exe = "vaultiktool";
#endif
    fs::create_directories(tmp.path / "bin");
    fs::create_directories(tmp.path / "PostgreSQL" / "9" / "bin");
    fs::create_directories(tmp.path / "PostgreSQL" / "17" / "bin");
    for (const fs::path& p : {tmp.path / "PostgreSQL" / "9" / "bin" / exe, tmp.path / "PostgreSQL" / "17" / "bin" / exe}) {
        std::ofstream(p) << "x";
#ifndef _WIN32
        fs::permissions(p, fs::perms::owner_all);
#endif
    }
    // Not on PATH: the highest version of the install folder.
    const std::string pattern = u8(tmp.path / "PostgreSQL" / "*" / "bin");
    CHECK(find_program("vaultiktool", {pattern}) == u8(tmp.path / "PostgreSQL" / "17" / "bin" / exe));
    CHECK(find_program("vaultik-no-such-tool", {pattern}).empty());
    CHECK(find_program("../vaultiktool", {pattern}).empty());

    // On PATH: found there first.
    std::ofstream(tmp.path / "bin" / exe) << "x";
#ifndef _WIN32
    fs::permissions(tmp.path / "bin" / exe, fs::perms::owner_all);
    const char sep = ':';
#else
    const char sep = ';';
#endif
    const std::string old_path = os::get_env("PATH");
    os::set_env("PATH", std::string("relative") + sep + u8(tmp.path / "bin") + sep + old_path);
    CHECK(find_program("vaultiktool", {pattern}) == u8(tmp.path / "bin" / exe));
    os::set_env("PATH", old_path);
}

TEST_CASE("job fields for Docker volumes and database dumps") {
    jobs::Job job;
    job.id = "containers";
    job.name = "Containers";
    job.repository = "D:\\Backups";
    job.docker_volumes = {"pgdata", "acumatica_prod_files"};
    job.docker_image = "busybox:1.36";
    DatabaseDump pg = sample(DatabaseKind::Postgres);
    pg.container = "pg";
    pg.user = "app";
    DatabaseDump ms = sample(DatabaseKind::SqlServer);
    ms.id = "ff00ff";
    ms.host = ".\\SQLEXPRESS";
    ms.port = 1433;
    ms.name = "shop-sql";
    job.databases = {pg, ms};
    CHECK_NOTHROW(jobs::validate(job));  // no folders needed

    const jobs::Job back = jobs::job_from_json(jobs::to_json(job));
    CHECK(back.docker_volumes == job.docker_volumes);
    CHECK(back.docker_image == "busybox:1.36");
    CHECK(back.databases == job.databases);

    SUBCASE("older jobs have none") {
        nlohmann::json j = nlohmann::json::parse(jobs::to_json(job));
        REQUIRE(j.erase("docker") == 1);
        REQUIRE(j.erase("databases") == 1);
        j["sources"] = {"C:\\Data"};
        const jobs::Job old = jobs::job_from_json(j.dump());
        CHECK(old.docker_volumes.empty());
        CHECK(old.docker_image.empty());
        CHECK(old.databases.empty());
    }
    SUBCASE("bad values are refused") {
        jobs::Job j = job;
        j.docker_volumes = {"bad name"};
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j = job;
        j.docker_image = "--privileged";
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j = job;
        j.databases[1].name = j.databases[0].database;  // both "shop" ... but of other kinds: fine
        CHECK_NOTHROW(jobs::validate(j));
        j.databases[1].kind = DatabaseKind::Postgres;  // now both "shop.dump"
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j = job;
        j.databases[0].database = "x\xE2\x80\xAEy";  // a text-direction mark
        CHECK_THROWS_AS(jobs::validate(j), Error);
    }
    SUBCASE("not with administrator rights, nor in a disk image job") {
        jobs::Job j = job;
        j.run_as_admin = true;
        CHECK_THROWS_WITH_AS(jobs::validate(j), doctest::Contains("administrator"), Error);
        j.databases.clear();
        CHECK_THROWS_AS(jobs::validate(j), Error);
        j = job;
        j.image_disks = {"system"};
        j.run_as_admin = true;
        CHECK_THROWS_AS(jobs::validate(j), Error);
    }
    SUBCASE("an elevated run from the user's settings does not run them") {
        CHECK(jobs::elevated_run_refusal(job, false).find("Docker") != std::string::npos);
        jobs::Job j = job;
        j.docker_volumes.clear();
        CHECK(!jobs::elevated_run_refusal(j, false).empty());
        j.databases.clear();
        j.sources = {"C:\\Data"};
        CHECK(jobs::elevated_run_refusal(j, false).empty());
    }
}

TEST_CASE("database passwords are stored per dump and removed with it") {
    TempDir tmp;
    const jobs::JobStore store(u8(tmp.path));
    store.set_database_secret("job1", "a1", "pw-one");
    store.set_database_secret("job1", "b2", "pw-two");
    store.set_database_secret("job2", "a1", "other job");
    CHECK(store.database_secret("job1", "a1") == "pw-one");
    CHECK(store.database_secret("job1", "b2") == "pw-two");
    CHECK(store.database_secret("job1", "c3").empty());
    CHECK_THROWS_AS(store.set_database_secret("job1", "../x", "pw"), Error);
    // The file holds the sealed password, never the text.
    {
        std::ifstream f(tmp.path / "secrets" / "job1.db-a1", std::ios::binary);
        const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(raw.find("pw-one") == std::string::npos);
    }

    jobs::Job job;
    job.id = "job1";
    DatabaseDump keep = sample(DatabaseKind::Postgres);
    keep.id = "b2";
    job.databases = {keep};
    store.remove_unused_database_secrets(job);
    CHECK(store.database_secret("job1", "a1").empty());
    CHECK(store.database_secret("job1", "b2") == "pw-two");
    CHECK(store.database_secret("job2", "a1") == "other job");

    store.set_database_secret("job1", "b2", "");  // an empty one removes it
    CHECK(store.database_secret("job1", "b2").empty());
    store.set_database_secret("job1", "b2", "again");
    store.remove("job1");
    CHECK(store.database_secret("job1", "b2").empty());
    CHECK(store.database_secret("job2", "a1") == "other job");
}

TEST_CASE("a dump with its tool missing says what to install") {
    TempDir tmp;
    const std::string old_path = os::get_env("PATH");
    os::set_env("PATH", u8(tmp.path));  // nothing on it
    DatabaseDump db = sample(DatabaseKind::Postgres);
    // Its install folders may exist on a developer's computer: then it runs and cannot connect,
    // which fails as well; either way no dump.
    CHECK_THROWS_AS(run_dump(db, "", u8(tmp.path / "out"), {}, std::chrono::seconds(30)), Error);
    db.kind = DatabaseKind::MySql;
    db.host = "127.0.0.1";
    db.port = 1;  // nothing listens there
    try {
        run_dump(db, "", u8(tmp.path / "out"), {}, std::chrono::seconds(30));
        FAIL("a dump without a database must fail");
    } catch (const Error& e) {
        const std::string what = e.what();
        CHECK((what.find("mysqldump") != std::string::npos));
    }
    os::set_env("PATH", old_path);
}

#ifndef _WIN32
TEST_CASE("a dump gets its password and settings, and only this account can read it") {
    TempDir tmp;
    const fs::path bin = tmp.path / "bin";
    fs::create_directories(bin);
    // A stand-in pg_dump: writes the dump file it was given, as the real one does (umask 022), with
    // what it was handed through its environment.
    {
        std::ofstream(bin / "pg_dump") << "#!/bin/sh\n"
                                          "for a in \"$@\"; do case \"$a\" in --file=*) f=\"${a#--file=}\";; esac; done\n"
                                          "umask 022\n"
                                          "printf '%s|%s' \"$PGCONNECT_TIMEOUT\" \"$(cat \"$PGPASSFILE\")\" > \"$f\"\n";
    }
    fs::permissions(bin / "pg_dump", fs::perms::owner_all);
    const std::string old_path = os::get_env("PATH");
    os::set_env("PATH", u8(bin) + ":" + old_path);

    // A folder the user named keeps its permissions; the dump in it is private.
    const fs::path to = tmp.path / "dumps";
    fs::create_directories(to);
    fs::permissions(to, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec);
    const DatabaseDump db = sample(DatabaseKind::Postgres);
    const std::string file = run_dump(db, "pw:1", u8(to), {}, std::chrono::seconds(30));
    os::set_env("PATH", old_path);
    std::ifstream in(fs::path(file), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "30|*:*:*:*:pw\\:1");  // (the shell drops the line's end)
    CHECK((fs::status(fs::path(file)).permissions() & fs::perms::all) == (fs::perms::owner_read | fs::perms::owner_write));
    CHECK((fs::status(to).permissions() & fs::perms::all) ==
          (fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
           fs::perms::others_exec));
    // The password file is gone again.
    for (const auto& e : fs::directory_iterator(to)) CHECK(e.path().filename().string().find("pgpass") == std::string::npos);
}
#endif
