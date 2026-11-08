#pragma once

// Thin Win32 helpers. This header deliberately does not include <windows.h>.

#include <cstdint>
#include <string>
#include <string_view>

namespace vaultik {

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Human-readable text for a Win32 error code.
std::string win32_message(std::uint32_t error);

// Throws Error("<what>: <message for error>").
[[noreturn]] void throw_win32(const std::string& what, std::uint32_t error);
// Same, using GetLastError().
[[noreturn]] void throw_last_error(const std::string& what);

// Absolute path via GetFullPathNameW, without the \\?\ prefix.
std::wstring full_path(const std::wstring& path);

// Adds the \\?\ (or \\?\UNC\) prefix so paths longer than MAX_PATH work. Input must be absolute.
std::wstring long_path(const std::wstring& absolute);

// Removes a \\?\ or \\?\UNC\ prefix for display.
std::wstring strip_long_prefix(const std::wstring& path);

// UTF-8 display form of a (possibly prefixed) path.
std::string display_path(const std::wstring& path);

// Joins a directory and a child name with exactly one backslash.
std::wstring join_path(const std::wstring& dir, std::wstring_view name);

std::wstring to_lower(std::wstring_view s);

std::string computer_name();
std::string user_name();

// Enables a privilege such as SeBackupPrivilege on the process token. Returns false if not held.
bool enable_privilege(const wchar_t* name);

// Current time as FILETIME ticks (100 ns since 1601-01-01 UTC).
std::uint64_t now_filetime();

// "2026-11-08 14:48:06" in local time.
std::string format_filetime_local(std::uint64_t filetime);

// Reads an environment variable; returns empty when unset.
std::wstring get_env(const wchar_t* name);

}  // namespace vaultik
