#include "vaultik/platform/acl.hpp"

#include "vaultik/util/error.hpp"

#ifdef _WIN32

#include <vector>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/util/hex.hpp"
#include "vaultik/util/win.hpp"
#include "vaultik/util/win_handle.hpp"

// After <windows.h>.
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

namespace vaultik::os {

namespace {

// Rights that let someone change what a file or folder holds, delete or rename it, or change who
// may do so. Folders: FILE_WRITE_DATA adds files, FILE_APPEND_DATA adds folders.
constexpr DWORD kChangeRights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | DELETE |
                                WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL;
// For a folder above: renaming it or something in it, or changing its permissions.
constexpr DWORD kAncestorRights = FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;

constexpr wchar_t kTrustedInstaller[] = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

struct LocalMem {
    void* p = nullptr;
    ~LocalMem() {
        if (p != nullptr) LocalFree(p);
    }
};

std::string sid_text(PSID sid) {
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return "an unknown account";
    LocalMem guard{text};
    // A readable name when there is one ("BUILTIN\Users").
    wchar_t name[256];
    wchar_t domain[256];
    DWORD name_len = 256;
    DWORD domain_len = 256;
    SID_NAME_USE use{};
    if (LookupAccountSidW(nullptr, sid, name, &name_len, domain, &domain_len, &use))
        return domain_len > 0 ? narrow(domain) + "\\" + narrow(name) : narrow(name);
    return narrow(text);
}

bool trusted(PSID sid) {
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    PSID ti = nullptr;
    if (!ConvertStringSidToSidW(kTrustedInstaller, &ti)) return false;
    LocalMem guard{ti};
    return EqualSid(sid, ti) != FALSE;
}

bool fail(std::string* why, std::string text) {
    if (why != nullptr) *why = std::move(text);
    return false;
}

}  // namespace

bool admin_only(const std::string& path, bool ancestor, std::string* why) {
    const std::wstring wp = long_path(full_path(widen(path)));
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = GetNamedSecurityInfoW(wp.c_str(), SE_FILE_OBJECT,
                                           OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
                                           &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) return fail(why, path + ": cannot read its permissions (" + win32_message(rc) + ")");
    LocalMem guard{sd};
    if (owner == nullptr || !trusted(owner)) {
        return fail(why, path + " is owned by " + (owner != nullptr ? sid_text(owner) : std::string("nobody")) +
                             ", who can change its permissions");
    }
    if (dacl == nullptr) return fail(why, path + " has no permissions set, so everyone can change it");
    const DWORD rights = ancestor ? kAncestorRights : kChangeRights;
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void* ace = nullptr;
        if (!GetAce(dacl, i, &ace)) return fail(why, path + ": cannot read its permissions");
        const auto* header = static_cast<const ACE_HEADER*>(ace);
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;  // applies to new children only
        switch (header->AceType) {
            case ACCESS_DENIED_ACE_TYPE:
            case ACCESS_DENIED_OBJECT_ACE_TYPE:
            case ACCESS_DENIED_CALLBACK_ACE_TYPE:
            case ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE:
                continue;  // a denial never grants anything
            case ACCESS_ALLOWED_ACE_TYPE:
                break;
            default:
                return fail(why, path + " has a kind of permission entry Vaultik does not check");
        }
        const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
        PSID sid = const_cast<DWORD*>(&allowed->SidStart);
        if ((allowed->Mask & rights) != 0 && !trusted(sid))
            return fail(why, sid_text(sid) + " can change " + path);
    }
    return true;
}

bool admin_only_chain(const std::string& path, std::string* why) {
    if (!admin_only(path, false, why)) return false;
    std::wstring dir = full_path(widen(path));
    for (;;) {
        while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
        const std::size_t cut = dir.find_last_of(L"\\/");
        if (cut == std::wstring::npos) return true;
        dir.resize(cut + 1);  // keep the separator: "C:\" is the root, "C:" is not
        if (!admin_only(narrow(dir), true, why)) return false;
        if (dir.size() <= 3) return true;  // "C:\"
        dir.pop_back();
    }
}

void make_admin_dir(const std::string& path, const std::string& reader_sid) {
    std::wstring sddl = L"O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    // 0x1200a9: read and list, without changing anything.
    if (!reader_sid.empty()) sddl += L"(A;OICI;0x1200a9;;;" + widen(reader_sid) + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        throw_last_error("cannot build the permissions for " + path);
    LocalMem guard{sd};
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    const std::wstring wp = long_path(full_path(widen(path)));

    // Someone could create the folder again between moving theirs aside and creating ours (C:\ProgramData
    // lets users add folders), so check again a few times.
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (CreateDirectoryW(wp.c_str(), &sa)) return;
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) throw_win32("cannot create " + path, err);
        const DWORD attrs = GetFileAttributesW(wp.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) &&
            !(attrs & FILE_ATTRIBUTE_REPARSE_POINT) && admin_only(path)) {
            // Ours from an earlier run: refresh the permissions so the reader entry is current.
            const DWORD rc = SetNamedSecurityInfoW(
                const_cast<LPWSTR>(wp.c_str()), SE_FILE_OBJECT,
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                [&] {
                    BOOL present = FALSE;
                    BOOL defaulted = FALSE;
                    PACL acl = nullptr;
                    GetSecurityDescriptorDacl(sd, &present, &acl, &defaulted);
                    return acl;
                }(),
                nullptr);
            if (rc != ERROR_SUCCESS) throw_win32("cannot set the permissions of " + path, rc);
            return;
        }
        std::uint8_t rnd[4];
        random_bytes(rnd);
        const std::wstring aside = wp + L".untrusted-" + widen(to_hex(rnd));
        if (!MoveFileExW(wp.c_str(), aside.c_str(), 0))
            throw_last_error(path + " exists and others can change it; it cannot be moved aside");
    }
    throw Error("cannot create " + path + " safely: something keeps creating it");
}

std::string current_user_sid() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) throw_last_error("cannot read the account");
    UniqueHandle token(raw);
    DWORD size = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
    std::vector<std::uint8_t> buf(size);
    if (!GetTokenInformation(token.get(), TokenUser, buf.data(), size, &size)) throw_last_error("cannot read the account");
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &text))
        throw_last_error("cannot read the account");
    LocalMem guard{text};
    return narrow(text);
}

bool process_elevated() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
    UniqueHandle token(raw);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof elevation, &size) &&
           elevation.TokenIsElevated != 0;
}

std::string program_data_dir() {
    PWSTR path = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_DONT_VERIFY, nullptr, &path);
    if (FAILED(hr)) {
        CoTaskMemFree(path);
        throw Error("cannot find the ProgramData folder");
    }
    std::string out = narrow(path);
    CoTaskMemFree(path);
    return out;
}

}  // namespace vaultik::os

#else

namespace vaultik::os {

bool admin_only(const std::string& path, bool /*ancestor*/, std::string* why) {
    if (why != nullptr) *why = path + ": administrator-only checks are only available on Windows";
    return false;
}

bool admin_only_chain(const std::string& path, std::string* why) {
    return admin_only(path, false, why);
}

void make_admin_dir(const std::string& /*path*/, const std::string& /*reader_sid*/) {
    throw Error("administrator folders are only available on Windows");
}

std::string current_user_sid() {
    throw Error("account identifiers are only available on Windows");
}

bool process_elevated() {
    return false;
}

std::string program_data_dir() {
    throw Error("ProgramData is only available on Windows");
}

}  // namespace vaultik::os

#endif
