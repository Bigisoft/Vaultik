#include "vaultik/platform/acl.hpp"

#include "vaultik/util/error.hpp"

#ifdef _WIN32

#include <algorithm>
#include <cstring>
#include <string_view>
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

namespace {

// The groups a standard (filtered) token holds for denying access only: administrators and the
// other groups whose members Windows gives an elevated token.
bool elevating_group(PSID sid) {
    for (const WELL_KNOWN_SID_TYPE kind :
         {WinBuiltinAdministratorsSid, WinBuiltinPowerUsersSid, WinBuiltinAccountOperatorsSid,
          WinBuiltinSystemOperatorsSid, WinBuiltinPrintOperatorsSid, WinBuiltinBackupOperatorsSid,
          WinBuiltinNetworkConfigurationOperatorsSid, WinBuiltinCryptoOperatorsSid, WinLocalAccountAndAdministratorSid})
        if (IsWellKnownSid(sid, kind)) return true;
    // Domain groups: Domain Admins, Cert Publishers, Schema Admins, Enterprise Admins, Group Policy
    // Creator Owners, domain controllers.
    const SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    const SID_IDENTIFIER_AUTHORITY* authority = GetSidIdentifierAuthority(sid);
    const UCHAR count = *GetSidSubAuthorityCount(sid);
    if (authority == nullptr || std::memcmp(authority, &nt, sizeof nt) != 0 || count != 5 ||
        *GetSidSubAuthority(sid, 0) != SECURITY_NT_NON_UNIQUE)
        return false;
    const DWORD rid = *GetSidSubAuthority(sid, 4);
    return rid == 512 || rid == 516 || rid == 517 || rid == 518 || rid == 519 || rid == 520 || rid == 498 || rid == 521;
}

std::vector<std::uint8_t> token_info(HANDLE token, TOKEN_INFORMATION_CLASS which) {
    DWORD size = 0;
    GetTokenInformation(token, which, nullptr, 0, &size);
    std::vector<std::uint8_t> buf(size == 0 ? 1 : size);
    if (!GetTokenInformation(token, which, buf.data(), static_cast<DWORD>(buf.size()), &size))
        throw_last_error("cannot read this program's rights");
    return buf;
}

BOOL WINAPI ignore_ctrl(DWORD type) {
    // Ctrl+C reaches the child too; it decides how to stop, and this process waits for it.
    return type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ? TRUE : FALSE;
}

}  // namespace

int run_with_user_rights(const std::string& program, const std::string& command_line) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT, &raw))
        throw_last_error("cannot read this program's rights");
    UniqueHandle token(raw);

    // Administrator groups only deny from now on.
    std::vector<std::uint8_t> groups_buf = token_info(token.get(), TokenGroups);
    const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groups_buf.data());
    std::vector<SID_AND_ATTRIBUTES> disable;
    for (DWORD i = 0; i < groups->GroupCount; ++i)
        if (elevating_group(groups->Groups[i].Sid)) disable.push_back({groups->Groups[i].Sid, 0});
    // Only the privileges a standard token has.
    std::vector<std::uint8_t> privs_buf = token_info(token.get(), TokenPrivileges);
    const auto* privs = reinterpret_cast<const TOKEN_PRIVILEGES*>(privs_buf.data());
    std::vector<LUID_AND_ATTRIBUTES> remove;
    for (DWORD i = 0; i < privs->PrivilegeCount; ++i) {
        wchar_t name[128];
        DWORD len = 128;
        LUID luid = privs->Privileges[i].Luid;
        const bool known = LookupPrivilegeNameW(nullptr, &luid, name, &len) != FALSE;
        const std::wstring_view n = known ? std::wstring_view(name, len) : std::wstring_view();
        if (n == SE_CHANGE_NOTIFY_NAME || n == SE_SHUTDOWN_NAME || n == SE_UNDOCK_NAME ||
            n == SE_INC_WORKING_SET_NAME || n == SE_TIME_ZONE_NAME)
            continue;
        remove.push_back({luid, 0});
    }
    HANDLE r = nullptr;
    if (!CreateRestrictedToken(token.get(), 0, static_cast<DWORD>(disable.size()), disable.empty() ? nullptr : disable.data(),
                               static_cast<DWORD>(remove.size()), remove.empty() ? nullptr : remove.data(), 0, nullptr, &r))
        throw_last_error("cannot reduce this program's rights");
    UniqueHandle restricted(r);

    // At most medium integrity, as a program started normally.
    std::vector<std::uint8_t> label_buf = token_info(restricted.get(), TokenIntegrityLevel);
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label_buf.data());
    const UCHAR label_count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (label_count == 0 || *GetSidSubAuthority(label->Label.Sid, label_count - 1u) > SECURITY_MANDATORY_MEDIUM_RID) {
        SID_IDENTIFIER_AUTHORITY mandatory = SECURITY_MANDATORY_LABEL_AUTHORITY;
        PSID medium = nullptr;
        if (!AllocateAndInitializeSid(&mandatory, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium))
            throw_last_error("cannot reduce this program's rights");
        TOKEN_MANDATORY_LABEL lower{};
        lower.Label.Sid = medium;
        lower.Label.Attributes = SE_GROUP_INTEGRITY;
        const BOOL ok = SetTokenInformation(restricted.get(), TokenIntegrityLevel, &lower,
                                            static_cast<DWORD>(sizeof lower + GetLengthSid(medium)));
        FreeSid(medium);
        if (!ok) throw_last_error("cannot reduce this program's rights");
    }
    // What it creates belongs to the account (not to Administrators, which it can no longer use),
    // and its own objects are open to the account and the system only.
    std::vector<std::uint8_t> user_buf = token_info(restricted.get(), TokenUser);
    PSID user = reinterpret_cast<const TOKEN_USER*>(user_buf.data())->User.Sid;
    TOKEN_OWNER owner{user};
    if (!SetTokenInformation(restricted.get(), TokenOwner, &owner, sizeof owner))
        throw_last_error("cannot reduce this program's rights");
    BYTE system_sid[SECURITY_MAX_SID_SIZE];
    DWORD system_size = sizeof system_sid;
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_sid, &system_size))
        throw_last_error("cannot reduce this program's rights");
    const DWORD acl_size = static_cast<DWORD>(sizeof(ACL) + 2 * sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(user) +
                                              GetLengthSid(system_sid));
    std::vector<std::uint8_t> acl_buf(acl_size);
    auto* acl = reinterpret_cast<ACL*>(acl_buf.data());
    if (!InitializeAcl(acl, acl_size, ACL_REVISION) || !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, user) ||
        !AddAccessAllowedAce(acl, ACL_REVISION, GENERIC_ALL, system_sid))
        throw_last_error("cannot reduce this program's rights");
    TOKEN_DEFAULT_DACL dacl{acl};
    if (!SetTokenInformation(restricted.get(), TokenDefaultDacl, &dacl, sizeof dacl))
        throw_last_error("cannot reduce this program's rights");
    // Never start it with administrator rights after all.
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(restricted.get(), TokenElevation, &elevation, sizeof elevation, &size) ||
        elevation.TokenIsElevated != 0)
        throw Error("cannot reduce this program's rights: Windows still counts them as an administrator's");

    // The child is killed with this process (the job closes with it).
    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid()) throw_last_error("cannot start the program");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof limits))
        throw_last_error("cannot start the program");

    // Only the standard handles are passed on.
    std::vector<HANDLE> inherit;
    const HANDLE std_handles[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                                   GetStdHandle(STD_ERROR_HANDLE)};
    for (const HANDLE h : std_handles) {
        if (h == nullptr || h == INVALID_HANDLE_VALUE) continue;
        if (std::find(inherit.begin(), inherit.end(), h) != inherit.end()) continue;
        if (SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) inherit.push_back(h);
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    std::vector<std::uint8_t> attr_buf;
    if (!inherit.empty()) {
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        attr_buf.resize(attr_size);
        si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
        if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size))
            throw_last_error("cannot start the program");
        if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit.data(),
                                       inherit.size() * sizeof(HANDLE), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            throw_last_error("cannot start the program");
        }
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = std_handles[0];
        si.StartupInfo.hStdOutput = std_handles[1];
        si.StartupInfo.hStdError = std_handles[2];
    }
    std::wstring app = widen(program);
    std::wstring cmd = widen(command_line);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessAsUserW(
        restricted.get(), app.c_str(), cmd.data(), nullptr, nullptr, inherit.empty() ? FALSE : TRUE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | (inherit.empty() ? 0 : EXTENDED_STARTUPINFO_PRESENT), nullptr,
        nullptr, &si.StartupInfo, &pi);
    const DWORD start_error = GetLastError();
    if (si.lpAttributeList != nullptr) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!started) {
        SetLastError(start_error);
        throw_last_error("cannot start " + program + " without administrator rights");
    }
    UniqueHandle process(pi.hProcess);
    UniqueHandle thread(pi.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        TerminateProcess(process.get(), 1);
        throw_last_error("cannot start the program");
    }
    SetConsoleCtrlHandler(ignore_ctrl, TRUE);
    ResumeThread(thread.get());
    WaitForSingleObject(process.get(), INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.get(), &code);
    return static_cast<int>(code);
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

int run_with_user_rights(const std::string&, const std::string&) {
    throw Error("running with reduced rights is only available on Windows");
}

std::string program_data_dir() {
    throw Error("ProgramData is only available on Windows");
}

}  // namespace vaultik::os

#endif
