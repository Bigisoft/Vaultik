#include "vaultik/util/win.hpp"

#include "vaultik/util/error.hpp"
#include "vaultik/util/win_handle.hpp"

#include <lmcons.h>

#include <cstdio>
#include <vector>

namespace vaultik {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    if (n <= 0) throw Error("invalid UTF-8 text");
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    // No WC_ERR_INVALID_CHARS: NTFS names may contain unpaired surrogates; they become U+FFFD.
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                      nullptr);
    if (n <= 0) throw Error("cannot convert text to UTF-8");
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string win32_message(std::uint32_t error) {
    wchar_t* buf = nullptr;
    const DWORD len = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string msg;
    if (len > 0 && buf != nullptr) {
        std::wstring w(buf, len);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ' || w.back() == L'.'))
            w.pop_back();
        msg = narrow(w);
    }
    if (buf != nullptr) LocalFree(buf);
    char code[32];
    std::snprintf(code, sizeof code, " (error %lu)", static_cast<unsigned long>(error));
    return (msg.empty() ? std::string("unknown error") : msg) + code;
}

void throw_win32(const std::string& what, std::uint32_t error) {
    throw Error(what + ": " + win32_message(error));
}

void throw_last_error(const std::string& what) {
    throw_win32(what, GetLastError());
}

std::wstring full_path(const std::wstring& path) {
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (n == 0) throw_last_error("cannot resolve path " + narrow(path));
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(path.c_str(), n, out.data(), nullptr);
    if (n == 0) throw_last_error("cannot resolve path " + narrow(path));
    out.resize(n);
    return strip_long_prefix(out);
}

std::wstring long_path(const std::wstring& absolute) {
    if (absolute.starts_with(L"\\\\?\\")) return absolute;
    if (absolute.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + absolute.substr(2);
    return L"\\\\?\\" + absolute;
}

std::wstring strip_long_prefix(const std::wstring& path) {
    if (path.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + path.substr(8);
    if (path.starts_with(L"\\\\?\\")) return path.substr(4);
    return path;
}

std::string display_path(const std::wstring& path) {
    return narrow(strip_long_prefix(path));
}

std::wstring join_path(const std::wstring& dir, std::wstring_view name) {
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out.append(name);
    return out;
}

std::wstring to_lower(std::wstring_view s) {
    std::wstring out(s);
    if (!out.empty()) CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

std::string computer_name() {
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) return "unknown";
    return narrow(std::wstring_view(buf, n));
}

std::string user_name() {
    wchar_t buf[UNLEN + 1];
    DWORD n = UNLEN + 1;
    if (!GetUserNameW(buf, &n) || n == 0) return "unknown";
    return narrow(std::wstring_view(buf, n - 1));
}

bool enable_privilege(const wchar_t* name) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) return false;
    UniqueHandle token(raw);
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) return false;
    if (!AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof tp, nullptr, nullptr)) return false;
    return GetLastError() == ERROR_SUCCESS;  // ERROR_NOT_ALL_ASSIGNED when the privilege is not held
}

std::uint64_t now_filetime() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return filetime_to_u64(ft);
}

std::string format_filetime_local(std::uint64_t filetime) {
    if (filetime == 0) return "-";
    const FILETIME utc = u64_to_filetime(filetime);
    SYSTEMTIME st_utc;
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&utc, &st_utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &st_utc, &st)) return "?";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
                  st.wMinute, st.wSecond);
    return buf;
}

std::wstring get_env(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, out.data(), n);
    out.resize(got);
    return out;
}

}  // namespace vaultik
