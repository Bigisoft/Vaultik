#pragma once

// Database dumps: before a backup, a job can have a database write a consistent copy of itself
// with its own tool, which is then backed up as the file Databases/<name>.<ext> of the version.
// The database's files themselves change while it runs, so a plain file backup (or a Docker
// volume's export) can catch them half-written.
//
//   SQL Server: sqlcmd runs BACKUP DATABASE ... TO DISK ... WITH COPY_ONLY (a copy outside the
//        backup chain, so the database's own log and differential backups are not disturbed).
//        SQL Server itself writes the file, so it must run on this computer (Windows; its service
//        is given write access to the dump folder) or in a Docker container.
//   MySQL and MariaDB: mysqldump --single-transaction (one consistent snapshot of InnoDB tables
//        without locking them), as SQL text.
//   PostgreSQL: pg_dump -Fc (its compressed custom format, restored with pg_restore).
//
// With a Docker container, the tool inside it runs through "docker exec" and its output is copied
// out. The password never goes on a command line: it is passed in an environment variable of the
// program only (SQLCMDPASSWORD, MYSQL_PWD with docker exec -e, PGPASSWORD with docker exec -e) or in
// a private file deleted after the dump (mysqldump's --defaults-extra-file, PostgreSQL's PGPASSFILE).
// Dumps go to a private folder in the temporary folder and are removed after the backup.

#include <chrono>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vaultik {

// The top folder of a version that holds the dumps.
inline constexpr char kDatabasesFolder[] = "Databases";

enum class DatabaseKind { SqlServer, MySql, Postgres };
// "sqlserver", "mysql", "postgres".
const char* to_string(DatabaseKind kind);
// Also takes "mssql", "mariadb", "postgresql" and "pg"; throws Error for anything else.
DatabaseKind parse_database_kind(std::string_view text);
// "SQL Server", "MySQL or MariaDB", "PostgreSQL".
const char* kind_name(DatabaseKind kind);
// The port the kind uses when none is given: 1433, 3306, 5432.
int default_port(DatabaseKind kind);

struct DatabaseDump {
    // Names the stored password (secrets/<job id>.db-<id>): lowercase letters and digits.
    std::string id;
    DatabaseKind kind = DatabaseKind::Postgres;
    // The dump's file name in the version, without its extension; empty: the database's name.
    std::string name;
    // The server: a host name or address (SQL Server also "host\instance"); empty: this computer
    // (inside the container, with one).
    std::string host;
    int port = 0;  // 0: the default (none given to the tool)
    std::string database;
    // Empty: SQL Server signs in with the Windows account; MySQL and PostgreSQL use the tool's
    // default (in a container: root, postgres).
    std::string user;
    // A Docker container the database runs in: the tool inside it runs through docker exec.
    std::string container;

    bool operator==(const DatabaseDump&) const = default;
};

// A new random id for DatabaseDump::id.
std::string new_database_id();
bool valid_database_id(std::string_view id);

// The dump's file name: "<name>.bak" (SQL Server), ".sql" (MySQL), ".dump" (PostgreSQL), with
// characters file names cannot have replaced (see vm_file_name).
std::string dump_file_name(const DatabaseDump& db);
// Where it is in a version: {"Databases", "<file name>"}.
std::vector<std::string> database_place(const DatabaseDump& db);

// Throws Error saying what is wrong (no database, a name the tools would take as an option, ...).
void check_database(const DatabaseDump& db);
// Throws Error when two dumps of a job would get the same file name.
void check_databases(const std::vector<DatabaseDump>& dbs);

// "PostgreSQL shop on db.local:5432 as backup, in the container pg".
std::string describe(const DatabaseDump& db);

// The command line form: kind://[user@][host][:port]/database[?container=<c>&name=<n>], with
// %XX for characters that would be read otherwise (e.g. postgres://app@localhost/shop,
// sqlserver://.\SQLEXPRESS/Sales, mysql://root@/shop?container=db). Throws Error.
DatabaseDump parse_database_spec(std::string_view spec);
std::string database_spec(const DatabaseDump& db);

// What runs for a dump. argv[0] is the tool's bare name ("pg_dump", "docker", ...), found on PATH
// when it runs. `env` is set for that program only (it holds the password).
struct DumpCommand {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    bool to_stdout = false;      // the dump is what it prints (otherwise it writes `out_file` itself)
    std::string secret_file;     // a private file to write `secret_text` to first, removed afterwards
    std::string secret_text;
    // SQL Server in a container: the file BACKUP writes inside it, copied out and removed after.
    std::string container_file;
};
// The command for one dump into `out_file`; `private_dir` is a folder only this account can read
// (for the password file). `container_file` names the file inside a container (SQL Server).
DumpCommand dump_command(const DatabaseDump& db, const std::string& password, const std::string& out_file,
                         const std::string& private_dir, const std::string& container_file = "/tmp/vaultik.bak");

// The T-SQL statement that writes a copy-only backup of `database` to `file`.
std::string sqlserver_backup_statement(const std::string& database, const std::string& file);
// A ~/.pgpass line matching every server, with ':' and '\' escaped.
std::string pgpass_text(const std::string& password);
// A MySQL option file with the password for every client program.
std::string mysql_option_text(const std::string& password);

// The SQL Server service (MSSQLSERVER, MSSQL$<instance>) for a server name ("." or "host\instance").
std::string sqlserver_service(const std::string& host);
// Whether a server name means this computer ("", ".", "(local)", "localhost", "127.0.0.1", this
// computer's name; with an instance or port).
bool is_local_server(const std::string& host, const std::string& computer_name);

// Writes the dump of `db` into `folder` (a private folder; created when missing) and returns the
// dump file's path. Throws Error with what went wrong: the tool is not installed, Docker is not
// running, the tool's own message.
std::string run_dump(const DatabaseDump& db, const std::string& password, const std::string& folder,
                     const std::function<bool()>& cancelled = {},
                     std::chrono::seconds timeout = std::chrono::hours(12));

}  // namespace vaultik
