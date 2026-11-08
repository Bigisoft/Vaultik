#pragma once

// Operating system layer: the only place that talks to Win32 or POSIX for files, paths and
// process facts. Everything above it is portable.
//
// Paths are UTF-8 strings. Unless noted they must be absolute (see absolute_path). The Windows
// implementation adds the \\?\ prefix internally, so paths longer than MAX_PATH work.
//
// Times are FILETIME ticks (100 ns since 1601-01-01 UTC) on every platform, because that is the
// unit the repository format stores.

#include <cstdint>
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
    // Random-access read of a repository file.
    static File open_random(const std::string& path, ErrorCode& err);
    // Appends to a file, creating it if missing. `mode` as for create.
    static File open_append(const std::string& path, ErrorCode& err, std::uint32_t mode = 0644);
    // New file for writing. Without `overwrite` it fails with is_exists(err) if present.
    // `mode` sets POSIX permissions (ignored on Windows).
    static File create(const std::string& path, bool overwrite, ErrorCode& err, std::uint32_t mode = 0644);

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

// True for kernel pseudo file systems (/proc, /sys, ...) that must never be backed up.
bool is_pseudo_filesystem(const std::string& path);

// ---- small files (os_common.cpp, built on File) ----------------------------------------------

// Creates `path` and any missing parents. Throws OsError.
void make_dirs(const std::string& path);
// Whole file. Throws OsError (is_not_found(code) when missing) or Error when over `max_size`.
Bytes read_file(const std::string& path, std::size_t max_size = 64u << 20);
// Writes via a temporary file and an atomic rename, creating parent folders. Throws OsError.
void write_file_atomic(const std::string& path, ByteSpan data, std::uint32_t mode = 0644);
// Appends, creating the file and its parent folders if needed. Throws OsError.
void append_file(const std::string& path, ByteSpan data, std::uint32_t mode = 0644);

// Encrypts a secret so only the current user can read it back (Windows DPAPI). On POSIX the
// data is returned unchanged and protection comes from the file mode (0600).
Bytes protect_secret(ByteSpan plain);
Bytes unprotect_secret(ByteSpan sealed);  // throws Error when it cannot be decrypted

// ---- process ------------------------------------------------------------------------------

// Runs a program (argv[0] is looked up on PATH) without a console window, waits for it, and
// returns its exit code. stdout and stderr are captured into `output` when given. Throws
// OsError when it cannot be started.
int run_command(const std::vector<std::string>& argv, std::string* output = nullptr);

// Full path of the running executable.
std::string executable_path();

// Lets an elevated run read (backup) or write (restore) files regardless of permissions.
// Windows: SeBackupPrivilege / SeRestorePrivilege. POSIX: no-op (run as root instead).
void enable_backup_privileges();
void enable_restore_privileges();

std::string hostname();
std::string username();
std::uint64_t now();
// "2026-11-08 14:48:06" in local time; "-" for 0.
std::string format_time_local(std::uint64_t time);
// Empty when unset.
std::string get_env(const char* name);

// FILETIME <-> Unix time conversion.
constexpr std::uint64_t kUnixEpochTicks = 116444736000000000ull;
constexpr std::uint64_t from_unix(std::int64_t seconds, std::int64_t nanoseconds) {
    return static_cast<std::uint64_t>((seconds * 10000000ll) + nanoseconds / 100) + kUnixEpochTicks;
}

}  // namespace vaultik::os
