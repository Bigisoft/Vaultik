#include "vaultik/ops/databases.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/ops/docker.hpp"
#include "vaultik/ops/virt.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/hex.hpp"

namespace vaultik {

namespace {

namespace fs = std::filesystem;

fs::path fs_path(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    std::size_t at = 0;
    while ((at = s.find(from, at)) != std::string::npos) {
        s.replace(at, from.size(), to);
        at += to.size();
    }
    return s;
}

std::string first_line(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string line(text.substr(start, end - start));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        const auto first = line.find_first_not_of(" \t");
        if (first != std::string::npos) return line.substr(first);
        start = end + 1;
    }
    return {};
}

bool has_control(std::string_view s) {
    return std::any_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u < 0x20 || u == 0x7F;
    });
}

// A value a tool would take as an option when it starts with '-'.
void check_value(const std::string& what, const std::string& value, std::size_t max) {
    if (value.size() > max) throw Error(what + " is too long");
    if (has_control(value)) throw Error(what + " contains control characters");
    if (value.starts_with("-")) throw Error(what + " cannot start with '-'");
}

// Runs a program whose standard output is the dump, into `path`. A watchdog stops it when the
// run is cancelled or it takes longer than `timeout`.
void pump_to_file(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env,
                  const std::string& path, const std::string& what, const std::function<bool()>& cancelled,
                  std::chrono::seconds timeout) {
    os::PipedProcess process(argv, os::PipedProcess::Direction::FromProgram, env);
    std::mutex lock;
    std::condition_variable wake;
    bool done = false;
    int stopped = 0;  // 1 cancelled, 2 timed out
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> g(lock);
        while (!done) {
            wake.wait_for(g, std::chrono::milliseconds(500));
            if (done) break;
            const bool cancel = cancelled && cancelled();
            if (cancel || std::chrono::steady_clock::now() > deadline) {
                stopped = cancel ? 1 : 2;
                process.stop();
                break;
            }
        }
    });
    const auto stop_watching = [&] {
        {
            const std::lock_guard<std::mutex> g(lock);
            done = true;
        }
        wake.notify_all();
        if (watchdog.joinable()) watchdog.join();
    };
    int code = 0;
    try {
        std::ofstream file(fs_path(path), std::ios::binary | std::ios::trunc);
        if (!file) throw Error("cannot write " + path);
        std::vector<std::uint8_t> buf(1 << 20);
        for (;;) {
            const std::size_t n = process.read(buf.data(), buf.size());
            if (n == 0) break;
            file.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n));
            if (!file) throw Error("cannot write " + path + " (is the drive full?)");
        }
        file.close();
        if (!file) throw Error("cannot write " + path + " (is the drive full?)");
        code = process.wait();
    } catch (...) {
        process.stop();
        stop_watching();
        if (stopped == 1) throw Error("cancelled");
        throw;
    }
    stop_watching();
    if (stopped == 1) throw Error("cancelled");
    if (stopped == 2) throw Error(what + " did not finish in time and was stopped");
    if (code != 0) {
        const std::string why = first_line(console_text(process.messages()));
        throw Error(what + " failed (exit code " + std::to_string(code) + ")" + (why.empty() ? "" : ": " + why));
    }
}

// Runs a program that writes the dump itself; throws Error with its message when it fails.
void run_tool(const std::vector<std::string>& argv, const std::vector<std::pair<std::string, std::string>>& env,
              const std::string& what, const std::function<bool()>& cancelled, std::chrono::seconds timeout,
              const std::string& hint = {}) {
    std::string output;
    os::CommandOptions o;
    o.output = &output;
    o.timeout = timeout;
    o.cancelled = cancelled;
    o.extra_environment = env;
    const int code = os::run_command(argv, o);
    if (code == 0) return;
    // sqlcmd prints its error after a "Msg 3201, Level 16, ..." line: show the line that says why.
    std::string text = console_text(output);
    std::string why;
    std::size_t start = 0;
    while (start < text.size() && why.empty()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = first_line(text.substr(start, end - start));
        if (!line.empty() && !line.starts_with("Msg ")) why = line;
        start = end + 1;
    }
    std::string message = what + " failed (exit code " + std::to_string(code) + ")" + (why.empty() ? "" : ": " + why);
    if (!hint.empty() && (text.find("Operating system error 5") != std::string::npos ||
                          lower(text).find("access is denied") != std::string::npos))
        message += "; " + hint;
    throw Error(message);
}

// The shell script that runs a tool inside a container, given as "sh -c <script> vaultik <args>":
// the first of `programs` the container has gets the arguments, which never pass through the shell.
std::string pick_tool_script(const std::vector<std::string>& programs, const std::string& missing) {
    std::string list;
    for (const std::string& p : programs) list += (list.empty() ? "" : " ") + p;
    return "for t in " + list +
           "; do if [ -x \"$t\" ] || command -v \"$t\" >/dev/null 2>&1; then exec \"$t\" \"$@\"; fi; done; echo \"" +
           missing + "\" >&2; exit 127";
}

std::string tool_name(DatabaseKind kind) {
    switch (kind) {
        case DatabaseKind::SqlServer:
            return "sqlcmd";
        case DatabaseKind::MySql:
            return "mysqldump";
        case DatabaseKind::Postgres:
            return "pg_dump";
    }
    return "pg_dump";
}

// Where each tool's installer puts it when it does not add itself to PATH.
std::string find_tool(DatabaseKind kind) {
    std::vector<std::string> extra;
#ifdef _WIN32
    const std::string pf = os::get_env("ProgramFiles");
    const std::string pf86 = os::get_env("ProgramFiles(x86)");
    for (const std::string& base : {pf, pf86}) {
        if (base.empty()) continue;
        switch (kind) {
            case DatabaseKind::SqlServer:
                extra.push_back(base + "\\SqlCmd");
                extra.push_back(base + "\\Microsoft SQL Server\\Client SDK\\ODBC\\*\\Tools\\Binn");
                extra.push_back(base + "\\Microsoft SQL Server\\*\\Tools\\Binn");
                break;
            case DatabaseKind::MySql:
                extra.push_back(base + "\\MySQL\\MySQL Server *\\bin");
                extra.push_back(base + "\\MariaDB *\\bin");
                break;
            case DatabaseKind::Postgres:
                extra.push_back(base + "\\PostgreSQL\\*\\bin");
                break;
        }
    }
#else
    switch (kind) {
        case DatabaseKind::SqlServer:
            extra = {"/opt/mssql-tools18/bin", "/opt/mssql-tools/bin"};
            break;
        case DatabaseKind::MySql:
            break;
        case DatabaseKind::Postgres:
            extra = {"/usr/lib/postgresql/*/bin", "/usr/pgsql-*/bin"};
            break;
    }
#endif
    std::string found = find_program(tool_name(kind), extra);
    if (found.empty() && kind == DatabaseKind::MySql) found = find_program("mariadb-dump", extra);
    return found;
}

std::string missing_tool(DatabaseKind kind) {
    switch (kind) {
        case DatabaseKind::SqlServer:
            return "sqlcmd (SQL Server's command line tool) is not installed on this computer, or not on PATH: install "
                   "the SQL Server command line tools (sqlcmd)";
        case DatabaseKind::MySql:
            return "mysqldump (or MariaDB's mariadb-dump) is not installed on this computer, or not on PATH: install "
                   "the MySQL or MariaDB client programs, or name the database's Docker container";
        case DatabaseKind::Postgres:
            return "pg_dump (PostgreSQL's backup tool) is not installed on this computer, or not on PATH: install "
                   "PostgreSQL's command line tools, or name the database's Docker container";
    }
    return "the database's tool is not installed";
}

// %XX for what a spec part cannot hold as it is.
std::string encode(std::string_view s) {
    std::string out;
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                           c == '-' || c == '.' || c == '_' || c == '~' || c == '\\' || c == '$' || u >= 0x80;
        if (plain) {
            out.push_back(c);
        } else {
            static const char kHex[] = "0123456789ABCDEF";
            out.push_back('%');
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 15]);
        }
    }
    return out;
}

std::string decode(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) throw Error("a '%' in a database spec must be followed by two hex digits");
            const Bytes b = from_hex(s.substr(i + 1, 2));
            out.push_back(static_cast<char>(b[0]));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

}  // namespace

const char* to_string(DatabaseKind kind) {
    switch (kind) {
        case DatabaseKind::SqlServer:
            return "sqlserver";
        case DatabaseKind::MySql:
            return "mysql";
        case DatabaseKind::Postgres:
            return "postgres";
    }
    return "postgres";
}

DatabaseKind parse_database_kind(std::string_view text) {
    const std::string t = lower(text);
    if (t == "sqlserver" || t == "mssql") return DatabaseKind::SqlServer;
    if (t == "mysql" || t == "mariadb") return DatabaseKind::MySql;
    if (t == "postgres" || t == "postgresql" || t == "pg") return DatabaseKind::Postgres;
    throw Error("the kind of database must be sqlserver, mysql or postgres, not \"" + std::string(text) + "\"");
}

const char* kind_name(DatabaseKind kind) {
    switch (kind) {
        case DatabaseKind::SqlServer:
            return "SQL Server";
        case DatabaseKind::MySql:
            return "MySQL or MariaDB";
        case DatabaseKind::Postgres:
            return "PostgreSQL";
    }
    return "PostgreSQL";
}

int default_port(DatabaseKind kind) {
    switch (kind) {
        case DatabaseKind::SqlServer:
            return 1433;
        case DatabaseKind::MySql:
            return 3306;
        case DatabaseKind::Postgres:
            return 5432;
    }
    return 0;
}

std::string new_database_id() {
    std::uint8_t rnd[6];
    random_bytes(rnd);
    return to_hex(rnd);
}

bool valid_database_id(std::string_view id) {
    return !id.empty() && id.size() <= 32 &&
           std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

std::string dump_file_name(const DatabaseDump& db) {
    const char* ext = db.kind == DatabaseKind::SqlServer ? ".bak" : db.kind == DatabaseKind::MySql ? ".sql" : ".dump";
    return vm_file_name(db.name.empty() ? db.database : db.name) + ext;
}

std::vector<std::string> database_place(const DatabaseDump& db) {
    return {kDatabasesFolder, dump_file_name(db)};
}

void check_database(const DatabaseDump& db) {
    if (!valid_database_id(db.id)) throw Error("invalid database dump id \"" + db.id + "\"");
    if (db.database.empty()) throw Error("a database dump needs the name of the database");
    check_value("the database's name", db.database, 128);
    check_value("the database's server", db.host, 255);
    check_value("the database's user", db.user, 128);
    if (db.name.size() > 100 || has_control(db.name)) throw Error("the name of the dump \"" + db.name + "\" is not valid");
    if (db.port < 0 || db.port > 65535) throw Error("the database's port must be between 1 and 65535");
    if (!db.container.empty() && !valid_container_name(db.container))
        throw Error("invalid Docker container name \"" + db.container + "\"");
    // pg_dump takes a database "name" with '=', or one starting "postgresql://" or "postgres://", as
    // a whole connection string: it would connect elsewhere (with the password).
    if (db.kind == DatabaseKind::Postgres && db.database.find('=') != std::string::npos)
        throw Error("a PostgreSQL database name with '=' is not supported");
    if (db.kind == DatabaseKind::Postgres &&
        (lower(db.database).starts_with("postgresql://") || lower(db.database).starts_with("postgres://")))
        throw Error("a PostgreSQL database name cannot be a connection URI (postgresql://...); give the server, "
                    "port and user as settings of the dump");
    if (db.kind == DatabaseKind::SqlServer && !db.container.empty() && db.user.empty())
        throw Error("SQL Server in a Docker container needs a user name (such as sa) and its password");
}

void check_databases(const std::vector<DatabaseDump>& dbs) {
    std::vector<std::string> names, ids;
    for (const DatabaseDump& db : dbs) {
        check_database(db);
        const std::string file = lower(dump_file_name(db));
        if (std::find(names.begin(), names.end(), file) != names.end())
            throw Error("two database dumps of the job would both be saved as " + dump_file_name(db) +
                        "; give one of them another name");
        if (std::find(ids.begin(), ids.end(), db.id) != ids.end()) throw Error("two database dumps share an id");
        names.push_back(file);
        ids.push_back(db.id);
    }
}

std::string describe(const DatabaseDump& db) {
    std::string s = std::string(kind_name(db.kind)) + " " + db.database + " on ";
    s += db.host.empty() ? (db.container.empty() ? std::string("this computer") : std::string("localhost")) : db.host;
    if (db.port != 0) s += ":" + std::to_string(db.port);
    if (!db.user.empty()) s += " as " + db.user;
    if (!db.container.empty()) s += ", in the container " + db.container;
    return s;
}

DatabaseDump parse_database_spec(std::string_view spec) {
    const std::size_t scheme = spec.find("://");
    if (scheme == std::string_view::npos)
        throw Error("a database is given as kind://[user@][host][:port]/database, e.g. postgres://app@localhost/shop");
    DatabaseDump db;
    db.id = new_database_id();
    db.kind = parse_database_kind(spec.substr(0, scheme));
    std::string_view rest = spec.substr(scheme + 3);
    std::string_view query;
    if (const std::size_t q = rest.find('?'); q != std::string_view::npos) {
        query = rest.substr(q + 1);
        rest = rest.substr(0, q);
    }
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash + 1 >= rest.size())
        throw Error("the database's name is missing in \"" + std::string(spec) + "\"");
    db.database = decode(rest.substr(slash + 1));
    std::string_view authority = rest.substr(0, slash);
    if (const std::size_t at = authority.rfind('@'); at != std::string_view::npos) {
        db.user = decode(authority.substr(0, at));
        authority = authority.substr(at + 1);
    }
    // "host:port"; a bracketed IPv6 address keeps its own colons.
    std::string_view host = authority;
    const std::size_t close = authority.rfind(']');
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && (close == std::string_view::npos || colon > close)) {
        const std::string_view port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) {
                return c >= '0' && c <= '9';
            }))
            throw Error("invalid port in \"" + std::string(spec) + "\"");
        db.port = std::stoi(std::string(port));
        if (db.port < 1 || db.port > 65535) throw Error("invalid port in \"" + std::string(spec) + "\"");
        host = authority.substr(0, colon);
    }
    db.host = decode(host);
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        if (pair.empty()) continue;
        const std::size_t eq = pair.find('=');
        const std::string key(pair.substr(0, eq));
        const std::string value = eq == std::string_view::npos ? std::string() : decode(pair.substr(eq + 1));
        if (key == "container") db.container = value;
        else if (key == "name") db.name = value;
        else throw Error("unknown setting \"" + key + "\" in a database spec (container, name)");
    }
    check_database(db);
    return db;
}

std::string database_spec(const DatabaseDump& db) {
    std::string s = std::string(to_string(db.kind)) + "://";
    if (!db.user.empty()) s += encode(db.user) + "@";
    s += encode(db.host);
    if (db.port != 0) s += ":" + std::to_string(db.port);
    s += "/" + encode(db.database);
    std::string q;
    if (!db.container.empty()) q += "container=" + encode(db.container);
    if (!db.name.empty()) q += std::string(q.empty() ? "" : "&") + "name=" + encode(db.name);
    if (!q.empty()) s += "?" + q;
    return s;
}

std::string sqlserver_backup_statement(const std::string& database, const std::string& file) {
    return "BACKUP DATABASE [" + replace_all(database, "]", "]]") + "] TO DISK = N'" + replace_all(file, "'", "''") +
           "' WITH COPY_ONLY, INIT";
}

std::string pgpass_text(const std::string& password) {
    return "*:*:*:*:" + replace_all(replace_all(password, "\\", "\\\\"), ":", "\\:") + "\n";
}

std::string mysql_option_text(const std::string& password) {
    std::string v = replace_all(password, "\\", "\\\\");
    v = replace_all(v, "\"", "\\\"");
    v = replace_all(v, "\n", "\\n");
    return "[client]\npassword=\"" + v + "\"\n";
}

std::string sqlserver_service(const std::string& host) {
    std::string instance;
    if (const std::size_t bs = host.find('\\'); bs != std::string::npos) instance = host.substr(bs + 1);
    if (const std::size_t comma = instance.find(','); comma != std::string::npos) instance = instance.substr(0, comma);
    instance = upper(instance);
    return instance.empty() || instance == "MSSQLSERVER" ? "MSSQLSERVER" : "MSSQL$" + instance;
}

bool is_local_server(const std::string& host, const std::string& computer_name) {
    std::string h = lower(host);
    for (const char* prefix : {"tcp:", "lpc:", "np:"})
        if (h.starts_with(prefix)) h = h.substr(std::string_view(prefix).size());
    if (const std::size_t cut = h.find_first_of("\\,"); cut != std::string::npos) h = h.substr(0, cut);
    return h.empty() || h == "." || h == "(local)" || h == "localhost" || h == "127.0.0.1" || h == "::1" ||
           h == "[::1]" || (!computer_name.empty() && h == lower(computer_name));
}

DumpCommand dump_command(const DatabaseDump& db, const std::string& password, const std::string& out_file,
                         const std::string& private_dir, const std::string& container_file) {
    DumpCommand c;
    const bool docker = !db.container.empty();
    const std::string port = db.port != 0 ? std::to_string(db.port) : std::string();
    switch (db.kind) {
        case DatabaseKind::SqlServer: {
            std::string server = db.host.empty() ? "localhost" : db.host;
            if (!port.empty()) server += "," + port;
            // -b: an error sets the exit code; -x: no $(variables) in the statement; -C: the
            // server's own certificate (newer sqlcmd encrypts by default) is trusted, as it is this
            // computer's or the container's.
            std::vector<std::string> args = {"-S", server, "-d", "master", "-b", "-x", "-C", "-l", "30"};
            if (db.user.empty()) {
                args.push_back("-E");
            } else {
                args.insert(args.end(), {"-U", db.user});
                if (!password.empty()) c.env.emplace_back("SQLCMDPASSWORD", password);
            }
            if (docker) {
                c.container_file = container_file;
                args.insert(args.end(), {"-Q", sqlserver_backup_statement(db.database, container_file)});
                c.argv = {"docker", "exec"};
                if (!password.empty()) c.argv.insert(c.argv.end(), {"-e", "SQLCMDPASSWORD"});
                c.argv.insert(c.argv.end(),
                              {db.container, "sh", "-c",
                               pick_tool_script({"/opt/mssql-tools18/bin/sqlcmd", "/opt/mssql-tools/bin/sqlcmd", "sqlcmd"},
                                                "sqlcmd is not installed in the container"),
                               "vaultik"});
            } else {
                args.insert(args.end(), {"-Q", sqlserver_backup_statement(db.database, out_file)});
                c.argv = {"sqlcmd"};
            }
            c.argv.insert(c.argv.end(), args.begin(), args.end());
            break;
        }
        case DatabaseKind::MySql: {
            std::vector<std::string> args;
            if (!docker && !password.empty()) {
                // Read before every other option, so it must come first.
                c.secret_file = os::join(private_dir, "mysql-" + db.id + ".cnf");
                c.secret_text = mysql_option_text(password);
                args.push_back("--defaults-extra-file=" + c.secret_file);
            }
            args.insert(args.end(), {"--single-transaction", "--quick", "--no-tablespaces"});
            if (!db.host.empty()) args.push_back("--host=" + db.host);
            if (!port.empty()) args.push_back("--port=" + port);
            const std::string user = db.user.empty() && docker ? std::string("root") : db.user;
            if (!user.empty()) args.push_back("--user=" + user);
            if (docker) {
                c.to_stdout = true;
                c.argv = {"docker", "exec"};
                if (!password.empty()) {
                    c.argv.insert(c.argv.end(), {"-e", "MYSQL_PWD"});
                    c.env.emplace_back("MYSQL_PWD", password);
                }
                c.argv.insert(c.argv.end(), {db.container, "sh", "-c",
                                             pick_tool_script({"mariadb-dump", "mysqldump"},
                                                              "neither mysqldump nor mariadb-dump is installed in the "
                                                              "container"),
                                             "vaultik"});
            } else {
                args.push_back("--result-file=" + out_file);
                c.argv = {"mysqldump"};
            }
            args.push_back(db.database);
            c.argv.insert(c.argv.end(), args.begin(), args.end());
            break;
        }
        case DatabaseKind::Postgres: {
            std::vector<std::string> args = {"--format=custom", "--no-password"};
            if (!db.host.empty()) args.push_back("--host=" + db.host);
            if (!port.empty()) args.push_back("--port=" + port);
            const std::string user = db.user.empty() && docker ? std::string("postgres") : db.user;
            if (!user.empty()) args.push_back("--username=" + user);
            if (docker) {
                c.to_stdout = true;
                c.argv = {"docker", "exec"};
                if (!password.empty()) {
                    c.argv.insert(c.argv.end(), {"-e", "PGPASSWORD"});
                    c.env.emplace_back("PGPASSWORD", password);
                }
                c.argv.insert(c.argv.end(), {db.container, "pg_dump"});
            } else {
                args.push_back("--file=" + out_file);
                c.argv = {"pg_dump"};
                if (!password.empty()) {
                    c.secret_file = os::join(private_dir, "pgpass-" + db.id);
                    c.secret_text = pgpass_text(password);
                    c.env.emplace_back("PGPASSFILE", c.secret_file);
                }
                c.env.emplace_back("PGCONNECT_TIMEOUT", "30");
            }
            args.push_back("--dbname=" + db.database);
            c.argv.insert(c.argv.end(), args.begin(), args.end());
            break;
        }
    }
    return c;
}

std::string run_dump(const DatabaseDump& db, const std::string& password, const std::string& folder,
                     const std::function<bool()>& cancelled, std::chrono::seconds timeout) {
    check_database(db);
    const bool docker = !db.container.empty();
    const std::string what = docker ? std::string(kind_name(db.kind)) + "'s dump in the container " + db.container
                                    : tool_name(db.kind);
    // A folder made here is private; one that is already there (one the user named) keeps its
    // permissions: the files in it are private instead.
    if (!os::is_directory(folder)) os::make_private_dirs(folder);
    std::string program;
    if (docker) {
        const DockerInfo info = docker_info();
        if (!info.available)
            throw Error(info.problem + ", so the database " + db.database + " in the container " + db.container +
                        " could not be dumped");
        program = info.program;
    } else {
        program = find_tool(db.kind);
        if (program.empty()) throw Error(missing_tool(db.kind));
    }

    std::string out = os::join(folder, dump_file_name(db));
    std::string hint;
    std::string container_file;
    if (db.kind == DatabaseKind::SqlServer && !docker) {
#ifdef _WIN32
        if (!is_local_server(db.host, os::hostname()))
            throw Error("SQL Server writes its backup on its own computer, so only a SQL Server on this computer (or in "
                        "a Docker container) can be backed up this way, not one on " + db.host);
        // SQL Server's service writes the file: a folder of its own that the service may write to.
        const std::string dir = os::join(folder, "sqlserver-" + db.id);
        os::make_dirs(dir);
        const std::string service = sqlserver_service(db.host);
        std::string ignored;
        os::CommandOptions o;
        o.output = &ignored;
        o.timeout = std::chrono::minutes(1);
        try {
            os::run_command({"icacls.exe", dir, "/grant", "NT SERVICE\\" + service + ":(OI)(CI)M"}, o);
        } catch (const std::exception&) {
        }
        hint = "SQL Server's service could not write to " + dir + " (Vaultik gave NT SERVICE\\" + service +
               " access to it; a service running under another account needs it too)";
        out = os::join(dir, dump_file_name(db));
#else
        throw Error("SQL Server on this computer can only be backed up on Windows; for SQL Server in a Docker "
                    "container, name the container");
#endif
    }
    if (db.kind == DatabaseKind::SqlServer && docker) container_file = "/tmp/vaultik-" + new_database_id() + ".bak";
    // The dump is made readable for this account alone (a tool makes it as the umask says, often
    // readable for everyone): an empty private file first, which the tool then writes over and
    // which keeps its permissions. (Not for SQL Server on this computer, whose service makes it.)
    const bool own_file = !(db.kind == DatabaseKind::SqlServer && !docker);
    if (own_file) os::write_file_atomic(out, ByteSpan(), 0600);

    DumpCommand cmd = dump_command(db, password, out, folder, container_file);
    cmd.argv[0] = program;
    // The password file exists only while the tool runs.
    struct SecretFile {
        std::string path;
        ~SecretFile() {
            if (path.empty()) return;
            std::error_code ec;
            const fs::path p = fs_path(path);
            const auto size = fs::file_size(p, ec);
            if (!ec) {  // overwrite before removing, so the text does not stay on the drive
                std::ofstream f(p, std::ios::binary | std::ios::in | std::ios::out);
                if (f) f.write(std::string(static_cast<std::size_t>(size), '\0').data(), static_cast<std::streamsize>(size));
            }
            fs::remove(p, ec);
        }
    } secret{cmd.secret_file};
    if (!cmd.secret_file.empty())
        os::write_file_atomic(cmd.secret_file,
                              ByteSpan(reinterpret_cast<const std::uint8_t*>(cmd.secret_text.data()), cmd.secret_text.size()),
                              0600);
    // A backup file made inside a container is removed there in the end, also when the backup
    // or copying it out failed (a backup that failed half way leaves a partial file).
    struct RemoveInside {
        std::string docker, container, file;
        ~RemoveInside() {
            if (file.empty()) return;
            try {
                std::string ignored;
                os::CommandOptions o;
                o.output = &ignored;
                o.timeout = std::chrono::minutes(2);
                os::run_command({docker, "exec", container, "rm", "-f", file}, o);
            } catch (const std::exception&) {
            }
        }
    } remove_inside{program, db.container, cmd.container_file};
    if (cmd.to_stdout) {
        pump_to_file(cmd.argv, cmd.env, out, what, cancelled, timeout);
    } else {
        run_tool(cmd.argv, cmd.env, what, cancelled, timeout, hint);
    }
    if (!cmd.container_file.empty()) {
        // Copied out of the container, then removed there (by remove_inside).
        pump_to_file({program, "exec", db.container, "cat", cmd.container_file}, {}, out,
                     "copying the backup out of the container " + db.container, cancelled, timeout);
    }
    std::error_code ec;
#ifndef _WIN32
    // (Also when a tool made the file anew.)
    if (own_file) fs::permissions(fs_path(out), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
#endif
    const auto size = fs::file_size(fs_path(out), ec);
    if (ec || size == 0) throw Error(what + " wrote no dump");
    return out;
}

}  // namespace vaultik
