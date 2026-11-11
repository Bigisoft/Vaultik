#include "vaultik/cli/console.hpp"

#include <windows.h>

#include <cstdio>
#include <string>

#include "vaultik/util/error.hpp"
#include "vaultik/util/win.hpp"

namespace vaultik::cli {

namespace {

bool g_status_shown = false;
std::size_t g_status_len = 0;

bool is_console(DWORD std_handle) {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(std_handle), &mode) != 0;
}

void write_handle(DWORD std_handle, std::string_view text) {
    HANDLE h = GetStdHandle(std_handle);
    if (is_console(std_handle)) {
        const std::wstring w = widen(text);
        DWORD n = 0;
        WriteConsoleW(h, w.data(), static_cast<DWORD>(w.size()), &n, nullptr);
    } else {
        DWORD n = 0;
        WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    }
}

}  // namespace

void init_console() {
    SetConsoleOutputCP(CP_UTF8);
}

bool interrupted() {
    return false;
}

void out(std::string_view text) {
    clear_status();
    write_handle(STD_OUTPUT_HANDLE, text);
}

void err(std::string_view text) {
    clear_status();
    write_handle(STD_ERROR_HANDLE, text);
}

void status(std::string_view text) {
    if (!is_console(STD_ERROR_HANDLE)) return;
    std::wstring w = widen(text);
    constexpr std::size_t kMaxWidth = 110;
    if (w.size() > kMaxWidth) w = w.substr(0, 40) + L"..." + w.substr(w.size() - (kMaxWidth - 43));
    std::wstring line = L"\r" + w;
    if (w.size() < g_status_len) line.append(g_status_len - w.size(), L' ');
    DWORD n = 0;
    WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    g_status_shown = true;
    g_status_len = w.size();
}

void clear_status() {
    if (!g_status_shown) return;
    std::wstring line = L"\r" + std::wstring(g_status_len, L' ') + L"\r";
    DWORD n = 0;
    WriteConsoleW(GetStdHandle(STD_ERROR_HANDLE), line.data(), static_cast<DWORD>(line.size()), &n, nullptr);
    g_status_shown = false;
    g_status_len = 0;
}

std::string read_password(std::string_view prompt) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(in, &mode))
        throw Error("no console to ask for the password; set VAULTIK_PASSWORD or use --password-file");
    err(prompt);
    SetConsoleMode(in, (mode & ~ENABLE_ECHO_INPUT) | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    std::wstring line;
    wchar_t buf[256];
    for (;;) {
        DWORD n = 0;
        if (!ReadConsoleW(in, buf, 256, &n, nullptr) || n == 0) break;
        line.append(buf, n);
        if (line.find(L'\n') != std::wstring::npos) break;
    }
    SetConsoleMode(in, mode);
    err("\n");
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
    std::string utf8 = narrow(line);
    SecureZeroMemory(line.data(), line.size() * sizeof(wchar_t));
    SecureZeroMemory(buf, sizeof buf);
    return utf8;
}

}  // namespace vaultik::cli
