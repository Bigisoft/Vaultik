#include <windows.h>

#include "vaultik/ops/apps.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/win.hpp"

namespace vaultik {

namespace {

constexpr const wchar_t* kUninstallKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

std::string read_string(HKEY key, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, nullptr,
                     &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t) || size > (64u << 10))
        return {};
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf.data(),
                     &size) != ERROR_SUCCESS)
        return {};
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    return narrow(buf);
}

DWORD read_dword(HKEY key, const wchar_t* name) {
    DWORD value = 0;
    DWORD size = sizeof value;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return 0;
    return value;
}

void read_view(HKEY root, REGSAM view, std::vector<UninstallEntry>& out) {
    HKEY list = nullptr;
    if (RegOpenKeyExW(root, kUninstallKey, 0, KEY_READ | view, &list) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD len = static_cast<DWORD>(std::size(name));
        const LSTATUS st = RegEnumKeyExW(list, i, name, &len, nullptr, nullptr, nullptr, nullptr);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) continue;
        HKEY sub = nullptr;
        if (RegOpenKeyExW(list, name, 0, KEY_READ | view, &sub) != ERROR_SUCCESS) continue;
        UninstallEntry e;
        e.key_name = narrow(std::wstring_view(name, len));
        e.display_name = read_string(sub, L"DisplayName");
        e.display_version = read_string(sub, L"DisplayVersion");
        e.publisher = read_string(sub, L"Publisher");
        e.parent_key_name = read_string(sub, L"ParentKeyName");
        e.release_type = read_string(sub, L"ReleaseType");
        e.system_component = read_dword(sub, L"SystemComponent") == 1;
        RegCloseKey(sub);
        out.push_back(std::move(e));
    }
    RegCloseKey(list);
}

}  // namespace

std::vector<UninstallEntry> read_uninstall_entries() {
    std::vector<UninstallEntry> out;
    read_view(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, out);
    read_view(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, out);
    read_view(HKEY_CURRENT_USER, 0, out);
    return out;
}

std::string winget_program() {
    try {
        // The system account and the service accounts have no winget.
        const std::string sid = os::current_user_sid();
        if (sid == "S-1-5-18" || sid == "S-1-5-19" || sid == "S-1-5-20") return {};
        std::string local = os::get_env("LOCALAPPDATA");
        if (local.empty()) local = os::user_folder(os::UserFolder::LocalAppData);
        const std::string path = local + "\\Microsoft\\WindowsApps\\winget.exe";
        return os::exists(path) ? path : std::string();
    } catch (const std::exception&) {
        return {};
    }
}

}  // namespace vaultik
