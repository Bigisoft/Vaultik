#pragma once

// Operating system layer: the only place that talks to Win32 or POSIX for files, paths and
// process facts. Everything above it is portable.
//
// Paths are UTF-8 strings. Unless noted they must be absolute (see absolute_path). The Windows
// implementation adds the \\?\ prefix internally, so paths longer than MAX_PATH work.
//
// Times are FILETIME ticks (100 ns since 1601-01-01 UTC) on every platform, because that is the
// unit the repository format stores.

#include <array>
#include <cstdint>
#include <utility>
#include <functional>
#include <chrono>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "vaultik/util/bytes.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::os {

#ifdef _WIN32
constexpr char kSeparator = '\\';
constexpr bool kCaseInsensitivePaths = true;
#else
constexpr char kSeparator = '/';
constexpr bool kCaseInsensitivePaths = false;
#endif

// ---- errors -------------------------------------------------------------------------------

// GetLastError() value on Windows, errno elsewhere.
using ErrorCode = std::uint32_t;

ErrorCode last_error();
std::string error_message(ErrorCode code);
bool is_not_found(ErrorCode code);
bool is_exists(ErrorCode code);
// The call does not exist on this system (POSIX stubs of Windows-only calls).
bool is_not_supported(ErrorCode code);

// An Error that carries the OS error code, so a caller can skip one file and carry on.
class OsError : public Error {
public:
    OsError(const std::string& what, ErrorCode code) : Error(what + ": " + error_message(code)), code_(code) {}
    ErrorCode code() const { return code_; }

private:
    ErrorCode code_;
};

[[noreturn]] void throw_error(const std::string& what, ErrorCode code);
[[noreturn]] void throw_last_error(const std::string& what);

// ---- paths --------------------------------------------------------------------------------

// Absolute, normalized form of `path` (relative to the current directory), without a trailing
// separator except for a root such as "C:\" or "/". Symlinks are not resolved.
std::string absolute_path(std::string_view path);

// Joins a directory and a single child name with exactly one separator.
std::string join(std::string_view dir, std::string_view name);

// Parent directory; empty for a root ("C:\", "\\server\share", "/").
std::string parent_of(std::string_view absolute);

// Splits an absolute path into snapshot tree components:
//   Windows: "C:\Users\x" -> {"C", "Users", "x"}, "\\srv\share\d" -> {"UNC", "srv", "share", "d"}
//   POSIX:   "/home/x" -> {"home", "x"}, "/" -> {}
std::vector<std::string> path_components(std::string_view absolute);

// Case folding used where the platform compares paths case-insensitively (identity on POSIX).
std::string fold_case(std::string_view utf8);

// ---- file metadata ------------------------------------------------------------------------

enum class EntryType : std::uint8_t {
    File,
    Dir,
    Symlink,  // POSIX symbolic link (target is stored)
    Other,    // stored as metadata only: Windows reparse points and cloud placeholders, POSIX
              // devices, sockets and pipes
};

struct FileInfo {
    EntryType type = EntryType::File;
    std::uint32_t attributes = 0;  // Windows FILE_ATTRIBUTE_* bits (0 on POSIX)
    std::uint32_t mode = 0;        // POSIX permission bits, 07777 (0 on Windows)
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::uint64_t ctime = 0;   // Windows: creation time. POSIX: status change time
    std::uint64_t mtime = 0;   // last modification
    std::uint64_t size = 0;    // files only
    std::uint64_t device = 0;  // POSIX st_dev, to notice mount points (0 on Windows)
};

// Metadata of `path` itself (links are not followed). Returns false and sets `err` on failure.
bool stat(const std::string& path, FileInfo& info, ErrorCode& err);

bool is_directory(const std::string& path);
bool exists(const std::string& path);

// Lists one directory (no "." or ".."). Construction never throws; check ok().
class DirReader {
public:
    explicit DirReader(const std::string& path);
    ~DirReader();
    DirReader(const DirReader&) = delete;
    DirReader& operator=(const DirReader&) = delete;

    bool ok() const { return error_ == 0; }
    ErrorCode error() const { return error_; }

    // Next entry; false at the end. If listing stopped early, error() is set afterwards.
    bool next(std::string& name, FileInfo& info);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
    ErrorCode error_ = 0;
};

// ---- files --------------------------------------------------------------------------------

// Move-only file handle. Every handle gets the resource policy's I/O priority, and reads and
// writes count against its read and write rate limits.
// Which file an open handle or a path is (Windows: volume serial number and 128-bit file id),
// to make sure a path reopened later still names the item created earlier.
struct FileId {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> id{};
    bool operator==(const FileId&) const = default;
};

class File {
public:
    File() = default;
    ~File();
    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // Sequential read of a source file (Windows: backup semantics, so ACLs do not block an
    // elevated run). Returns an invalid File and sets `err` on failure.
    static File open_read(const std::string& path, ErrorCode& err);
    // Like open_read, for a file found by a backup scan: never follows a link and only opens a
    // regular file, so an entry swapped for a link after the scan cannot redirect the read.
    static File open_source(const std::string& path, ErrorCode& err);
    // Random-access read of a repository file.
    static File open_random(const std::string& path, ErrorCode& err);
    // Appends to a file, creating it if missing. `mode` as for create.
    static File open_append(const std::string& path, ErrorCode& err, std::uint32_t mode = 0644);
    // New file for writing. Without `overwrite` it fails with is_exists(err) if present; with it,
    // an existing file or link is deleted and a new file created (never written through).
    // `mode` sets POSIX permissions (ignored on Windows).
    static File create(const std::string& path, bool overwrite, ErrorCode& err, std::uint32_t mode = 0644);
    // Windows: creates (or replaces) the alternate data stream `name` of the file at `path`,
    // without following a link: fails when `path` is a link, a folder or a file with other hard
    // links, so a restore cannot be redirected into another file. Not supported on POSIX.
    // `expected` is the id of the file as restored (from id() before it was closed): the stream
    // is refused when the path now names another file, whatever link was put in on the way.
    static File create_stream(const std::string& path, const std::string& name, const FileId& expected,
                              ErrorCode& err);
    // This file's id; empty when it cannot be told (POSIX).
    std::optional<FileId> id() const;
    // Windows: creates a new folder and keeps it open, so its permissions can be set on exactly
    // the folder made, whatever happens to its path. Fails (is_exists) when something is there.
    // Not supported on POSIX.
    static File create_directory(const std::string& path, ErrorCode& err);
    // Windows: opens an existing folder and keeps it from being renamed, moved or deleted while
    // the handle is open (no delete sharing), so a path through it keeps leading to it. With
    // `real_folder`, refuses (ERROR_STOPPED_ON_SYMLINK) a link or junction in its place. Not
    // supported on POSIX.
    static File hold_directory(const std::string& path, bool real_folder, ErrorCode& err);
    // Applies a security descriptor through this handle (create_directory's). Without the right
    // to set another owner, the access list alone.
    ErrorCode set_security(ByteSpan descriptor);
    // Windows: opens a file, folder or link itself (never what a link points to) only to delete
    // it with discard(), so where it really is can be checked first. Not supported on POSIX.
    static File open_for_delete(const std::string& path, ErrorCode& err);
    // Where this file really is, every link and junction on the way resolved ("C:\x\y");
    // empty when it cannot be told (POSIX).
    std::string final_path() const;
    // Deletes the file or empty folder this handle is open on (a read-only one too), whatever its
    // path leads to now. Needs a handle with delete access: create's, create_directory's or
    // open_for_delete's. Not supported on POSIX.
    ErrorCode discard();

    bool valid() const;
    void close();

    // Reads up to `max` bytes; 0 at end of file. Throws OsError.
    std::size_t read(std::uint8_t* dst, std::size_t max);
    // Reads exactly `length` bytes at `offset`. Throws Error (truncated) or OsError.
    void read_at(std::uint64_t offset, std::uint8_t* dst, std::size_t length);
    // Throws OsError.
    void write_all(ByteSpan data);
    // Flushes to stable storage. Throws OsError.
    void sync();
    // Reserves space so large files are not fragmented. Best effort.
    void preallocate(std::uint64_t size);

private:
    explicit File(std::intptr_t h) : h_(h) {}
    std::intptr_t h_ = -1;  // HANDLE or file descriptor
};

// Creates one directory. Returns 0, or the error (is_exists(err) when something is there).
ErrorCode make_dir(const std::string& path);
// Atomically replaces `to` with `from`, durably. Throws OsError.
void rename_replace(const std::string& from, const std::string& to);
// Returns 0 or the error.
ErrorCode remove_file(const std::string& path);
// Clears a read-only flag so the file can be replaced (no-op on POSIX).
void make_writable(const std::string& path);
// Applies times, attributes or permissions, and (POSIX, when running as root) ownership.
// Best effort: failures are ignored.
void set_metadata(const std::string& path, const FileInfo& info);

// Symbolic links. Return 0 or the error.
ErrorCode make_symlink(const std::string& target, const std::string& path);
ErrorCode read_symlink(const std::string& path, std::string& target);

// ---- Windows extras (win_extras.cpp; on POSIX nothing is read and writes are not supported) ----

// The security descriptor of a file or folder (owner, group and access list), self-relative.
// Empty when it cannot be read.
Bytes read_security(const std::string& path);
// Applies one. Without the right to set another owner, applies the access list alone.
// Refused (ERROR_STOPPED_ON_SYMLINK) when the item is a link, a hard-linked file, or, with
// `expected`, any item other than that one: an elevated restore must not be redirected.
ErrorCode write_security(const std::string& path, ByteSpan descriptor, const FileId* expected = nullptr);
// The path of a folder with every link and junction on the way resolved ("C:\Users\me\Docs"
// for a junction that points there); empty when it cannot be told (and on POSIX).
std::string final_path(const std::string& path);
// The id of the item at `path` itself (a link is not followed); empty when it cannot be read.
std::optional<FileId> file_id(const std::string& path);

// The alternate data streams of a file or folder (not its own content).
struct StreamEntry {
    std::string name;  // without the colons and the ":$DATA" type
    std::uint64_t size = 0;
};
std::vector<StreamEntry> list_streams(const std::string& path);
// The path that opens a stream: "C:\x\a.txt" and "info" give "C:\x\a.txt:info".
std::string stream_path(const std::string& path, const std::string& name);

// A Windows symlink or junction.
struct ReparseLink {
    std::uint32_t tag = 0;  // 0xA000000C symlink, 0xA0000003 junction (mount point)
    std::string target;     // "C:\Data", "\\server\share\x", or a relative "..\x"
    bool relative = false;
};
// False when `path` is not such a link or it cannot be read.
bool read_link(const std::string& path, ReparseLink& out);
// Creates one (`directory` for a link to a folder). Returns 0 or the error.
ErrorCode create_link(const std::string& path, const ReparseLink& link, bool directory);

// True for kernel pseudo file systems (/proc, /sys, ...) that must never be backed up.
bool is_pseudo_filesystem(const std::string& path);

// Where a local folder lives. `volume` is a name that stays the same when the drive gets another
// letter (Windows: "\\?\Volume{GUID}\"), `root` the volume's current mount path ("F:\") and
// `relative` the rest of the path below it ("Backups\vaultik"). `volume` is empty for network
// shares, folders that do not exist, and systems without such names. `system` is true for the
// volume Windows runs from.
struct VolumeLocation {
    std::string volume;
    std::string root;
    std::string relative;
    bool system = false;
    bool external = false;  // a USB, FireWire or memory card drive (not an internal disk)
};
VolumeLocation volume_of(const std::string& path);
// The current mount path of a volume named by volume_of ("F:\"), or empty when it is not connected.
std::string volume_root(const std::string& volume);

// ---- small files (os_common.cpp, built on File) ----------------------------------------------

// Creates `path` and any missing parents. Throws OsError.
void make_dirs(const std::string& path);
// Whole file. Throws OsError (is_not_found(code) when missing) or Error when over `max_size`.
Bytes read_file(const std::string& path, std::size_t max_size = 64u << 20);
// Writes via a temporary file and an atomic rename, creating parent folders. Throws OsError.
void write_file_atomic(const std::string& path, ByteSpan data, std::uint32_t mode = 0644);
// Appends, creating the file and its parent folders if needed. Throws OsError.
void append_file(const std::string& path, ByteSpan data, std::uint32_t mode = 0644);

// Creates a folder for secrets (and missing parents) that only this account can open: POSIX
// mode 0700, also set on a folder that is already there. Windows: make_dirs (the profile's
// permissions already keep other accounts out). Throws OsError.
void make_private_dirs(const std::string& path);

// Encrypts a secret so only the current user can read it back (Windows DPAPI). On POSIX the
// data is returned unchanged and protection comes from the file mode (0600) inside a 0700 folder
// (make_private_dirs): anyone who can read the account's files (root, a backup of the home
// folder, a program running as the user) can read it, as with an SSH key without a passphrase.
// `machine`: sealed with the computer's key instead of the account's (Windows), so another account
// (the system account of a job with administrator rights) can open it; the file's permissions are
// then all that keeps others out.
Bytes protect_secret(ByteSpan plain, bool machine = false);
Bytes unprotect_secret(ByteSpan sealed);  // throws Error when it cannot be decrypted

// ---- process ------------------------------------------------------------------------------

// Runs a program without a console window (argv[0]: a bare name is taken from the system folder
// on Windows and looked up on PATH on POSIX), waits for it, and
// returns its exit code. stdout and stderr are captured into `output` when given. Throws
// OsError when it cannot be started.
int run_command(const std::vector<std::string>& argv, std::string* output = nullptr);
struct CommandOptions {
    std::string* output = nullptr;  // stdout and stderr, the first 1 MiB
    // Stopped (with everything it started) after this long, or when `cancelled` says so; then
    // run_command throws Error. Standard input is empty.
    std::chrono::seconds timeout = std::chrono::hours(4);
    std::function<bool()> cancelled;
};
int run_command(const std::vector<std::string>& argv, const CommandOptions& options);

// Windows: like run_command, but with administrator rights, so the user sees an approval (UAC)
// prompt unless the process already has them. Output is not captured. Throws Error when the
// approval is declined or the program cannot start; on POSIX it always throws.
int run_command_elevated(const std::vector<std::string>& argv);

// Runs a command line as typed by the user, through the shell (Windows: cmd.exe /s /c; POSIX:
// /bin/sh -c), without a window, and returns its exit code. Captures stdout and stderr into
// `output` when given (its first 64 KiB). Throws OsError when the shell cannot be started.
int run_shell(const std::string& command, std::string* output = nullptr);
struct ShellOptions {
    std::string* output = nullptr;  // stdout and stderr, the first 64 KiB
    // Stopped (with everything it started) after this long, or when `cancelled` says so; then
    // run_shell throws Error. Programs it starts and leaves running are not waited for.
    std::chrono::seconds timeout = std::chrono::hours(4);
    std::function<bool()> cancelled;
    // Windows: a fixed minimal environment (the system folders on Path) instead of this
    // process's, plus `extra_environment`; for commands run with administrator rights.
    bool clean_environment = false;
    std::vector<std::pair<std::string, std::string>> extra_environment;
    std::string directory;  // the working folder (empty: this process's)
};
int run_shell(const std::string& command, const ShellOptions& options);
// Whether the current internet connection is metered (paid by the byte or capped, as Windows
// reports it for mobile data or a connection the user marked so). False when it cannot tell, and
// on POSIX.
bool metered_connection();
// Sets an environment variable of this process (and so of the programs it starts).
void set_env(const std::string& name, const std::string& value);

// Full path of the running executable.
std::string executable_path();

// Lets an elevated run read (backup) or write (restore) files regardless of permissions.
// Windows: SeBackupPrivilege / SeRestorePrivilege. POSIX: no-op (run as root instead).
void enable_backup_privileges();
void enable_restore_privileges();

std::string hostname();
std::string username();
// A stable id of this computer that survives renaming it: Windows' MachineGuid (set when Windows
// is installed), /etc/machine-id elsewhere. Falls back to the host name when there is none.
std::string machine_id();

// An entry in the system's log: the Windows Event Log (Application, source "Vaultik", which the
// installer registers; without it Event Viewer still shows the text, after a note that the source
// is unknown), syslog elsewhere. Never fails: a log that cannot be written is skipped.
enum class LogLevel { Info, Warning, Error };
void system_log(LogLevel level, std::uint32_t event_id, const std::string& text);
std::uint32_t process_id();
// False only when no process with this id runs on this computer (unknown counts as running).
bool process_alive(std::uint32_t pid);
std::uint64_t now();
// "2026-11-08 14:48:06" in local time; "-" for 0.
std::string format_time_local(std::uint64_t time);
// Empty when unset.
std::string get_env(const char* name);

// The current account's own folders: on Windows its known folders, wherever they are (moved into
// OneDrive, say); elsewhere $HOME and the XDG folders. Empty when there is no such folder.
enum class UserFolder { Profile, Documents, Desktop, Pictures, Music, Videos, Downloads, SavedGames, RoamingAppData, LocalAppData };
std::string user_folder(UserFolder folder);

// FILETIME <-> Unix time conversion.
constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ull;
constexpr std::uint64_t from_unix(std::int64_t seconds, std::int64_t nanoseconds) {
    return static_cast<std::uint64_t>((seconds * 10000000ll) + nanoseconds / 100) + kUnixEpochTicks;
}

}  // namespace vaultik::os
