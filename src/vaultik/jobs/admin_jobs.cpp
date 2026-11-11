#include "vaultik/jobs/admin_jobs.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <system_error>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/alerts.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/hex.hpp"
#ifdef _WIN32
#include <windows.h>
// After windows.h:
#include <lm.h>
#include <sddl.h>

#include "vaultik/util/win.hpp"
#endif

namespace vaultik::jobs {

#ifdef _WIN32
namespace {

// A field of the job for the question below, shown in full. Only an explicit set of narrow,
// visible characters is shown as it is: printable ASCII, and the letters of Latin (with accents),
// Greek and Cyrillic, without combining marks. Every other character, invisible, blank-looking,
// combining, wide or from another script, is shown as its code ("[U+3164]"), so a field cannot
// hide text or fake the layout or wording of the question. A run of more than 3 spaces is shown
// as "[n spaces]". A field too long to show in full is refused rather than cut: what is approved
// must be exactly what is seen.
std::wstring shown(const std::string& utf8, const std::string& what) {
    const auto plain = [](wchar_t c) {
        return (c >= 0x21 && c <= 0x7E) ||                                 // ASCII
               (c >= 0xA1 && c <= 0x24F && c != 0xAD) ||                   // Latin-1, Latin Extended A/B
               (c >= 0x370 && c <= 0x3FF) ||                               // Greek
               (c >= 0x400 && c <= 0x52F && !(c >= 0x483 && c <= 0x489));  // Cyrillic, without its marks
    };
    std::wstring out;
    std::size_t spaces = 0;
    const auto flush_spaces = [&] {
        if (spaces > 3) out += L"[" + std::to_wstring(spaces) + L" spaces]";
        else out.append(spaces, L' ');
        spaces = 0;
    };
    for (const wchar_t c : widen(utf8)) {
        if (c == L' ') {
            ++spaces;
            continue;
        }
        flush_spaces();
        if (plain(c)) {
            out += c;
        } else {
            wchar_t code[16];
            swprintf(code, 16, L"[U+%04X]", static_cast<unsigned>(c));
            out += code;
        }
    }
    flush_spaces();
    if (out.size() > 200) throw Error(what + " is too long to show in full for approval (over 200 characters)");
    return out;
}

}  // namespace

namespace {
std::string prepare_admin_store(const std::string& sid);  // below
}  // namespace

std::string own_admin_store() {
    if (!os::process_elevated()) throw Error("the admin store needs administrator rights");
    prepare_admin_store(os::current_user_sid());
    return open_admin_store().dir();
}

void check_admin_command(const std::string& command, const std::string& what) {
    if (command.empty()) return;
    if (command.find_first_of("%^!") != std::string::npos)
        throw Error(what + " of a job with administrator rights must not use %variables%, !variables! or ^");
    // A short name ("POWERS~1") reaches a program without its name being seen.
    if (command.find('~') != std::string::npos)
        throw Error(what + " of a job with administrator rights must not use short names with ~; write the full names");
    if (command.find("..") != std::string::npos)
        throw Error(what + " of a job with administrator rights must not use \"..\"");
    if (command.find("\\\\") != std::string::npos || command.find("//") != std::string::npos)
        throw Error(what + " of a job with administrator rights must not use network paths");
    for (const char c : command)
        if (static_cast<unsigned char>(c) < 0x20 && c != '\t')
            throw Error(what + " of a job with administrator rights must be a single line");
    // Split into words as cmd would see them (a quoted part is one word), noting where each
    // command starts: at the beginning and after an unquoted &, |, ( or ).
    struct Word {
        std::string text;
        bool starts_command = false;   // the first word of a command
        bool redirect_target = false;  // the file after < or >
    };
    std::vector<Word> words;
    {
        Word word;
        bool quoted = false;
        bool next_starts = true;
        bool next_redirect = false;
        std::string quoted_part;
        const auto push = [&] {
            if (!word.text.empty()) {
                word.starts_command = next_starts && !next_redirect;
                word.redirect_target = next_redirect;
                if (word.starts_command) next_starts = false;
                next_redirect = false;
                words.push_back(word);
            }
            word = Word{};
        };
        char prev = 0;
        for (const char d : command) {
            const char before = prev;
            prev = d;
            if (d == '&' && (before == '>' || before == '<') && !quoted) continue;  // "2>&1": a handle
            if (d == '"') {
                // A quoted part with these would be read again by a cmd started inside the command.
                if (quoted && quoted_part.find_first_of("&|<>()") != std::string::npos)
                    throw Error(what + " of a job with administrator rights must not quote &, |, <, >, ( or )");
                quoted = !quoted;
                quoted_part.clear();
                continue;
            }
            if (quoted) {
                quoted_part += d;
                word.text += d;
                continue;
            }
            if (d == '&' || d == '|' || d == '(' || d == ')') {
                push();
                next_starts = true;
                next_redirect = false;
            } else if (d == '<' || d == '>') {
                // "2>" names a handle, not a word.
                if (!word.text.empty() &&
                    std::all_of(word.text.begin(), word.text.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    word = Word{};
                push();
                if (next_starts)
                    throw Error(what + " of a job with administrator rights must put a redirection after the program");
                next_redirect = true;
            } else if (d == ' ' || d == '\t' || d == ',' || d == ';' || d == '=') {
                push();
            } else {
                word.text += d;
            }
        }
        if (quoted) throw Error(what + " of a job with administrator rights has an unclosed quote");
        push();
    }
    const auto refuse = [&](const std::string& word, const std::string& why) {
        throw Error(what + " of a job with administrator rights cannot use \"" + word + "\": " + why);
    };
    const auto lower = [](std::string t) {
        for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return t;
    };
    // Every word with a path separator must be a full path ("C:\x\y.cmd") on an admin-only chain,
    // or a switch like "/c"; anything else (a path relative to a drive, "C:x", or to its root,
    // "\x") could resolve to a folder the user can change and is refused. Bare names resolve in the
    // admin-only working folder and the system folders of the fixed environment.
    const auto check_path = [&](const std::string& word) {
        const bool drive = word.size() >= 2 && ((word[0] | 0x20) >= 'a' && (word[0] | 0x20) <= 'z') && word[1] == ':';
        if (drive) {
            if (word.size() < 3 || (word[2] != '\\' && word[2] != '/'))
                refuse(word, "a path relative to a drive; write the full path");
            // A file that does not exist yet could still be put there: judge its folder then.
            std::string probe = word;
            while (!probe.empty() && !os::exists(probe)) probe = os::parent_of(probe);
            std::string why;
            if (probe.empty() || !os::admin_only_chain(probe, &why))
                refuse(word, "programs without administrator rights can change it" + (why.empty() ? "" : " (" + why + ")"));
            return;
        }
        if (word.find('\\') != std::string::npos) refuse(word, "not a full path; write it as C:\\...");
        if (word.find('/') != std::string::npos) {
            // Only a switch such as /c, /q or /v:off (a slash and up to 5 letters, digits or a
            // colon) that is not also the name of something at the top of the system drive.
            bool sw = word[0] == '/' && word.size() >= 2 && word.size() <= 6;
            for (std::size_t k = 1; sw && k < word.size(); ++k)
                sw = std::isalnum(static_cast<unsigned char>(word[k])) != 0 || word[k] == ':';
            if (sw) {
                // cmd would read it as a path at the top of the drive, with any of the usual
                // program extensions: refuse it when anything there answers to it.
                char windows[MAX_PATH] = {};
                const UINT n = GetSystemWindowsDirectoryA(windows, MAX_PATH);
                if (n < 2 || n >= MAX_PATH) refuse(word, "the system drive cannot be told");
                const std::string at_top = std::string(windows, 2) + "\\" + word.substr(1, word.find(':') - 1);
                for (const char* ext : {"", ".com", ".exe", ".bat", ".cmd", ".vbs", ".js", ".wsf", ".msc", ".ps1"})
                    if (os::exists(at_top + ext)) sw = false;
            }
            if (!sw) refuse(word, "not a full path; write it as C:\\...");
        }
    };
    // What a command starts with is run as a program: only a program or a batch file, never a
    // file that the user's own file types would open (a .vbs, .js or .ps1), and nothing that
    // would start another one out of sight of these checks.
    const auto check_program = [&](const std::string& word) -> std::string {
        const std::size_t cut = word.find_last_of("\\/:");
        const std::string name = lower(cut == std::string::npos ? word : word.substr(cut + 1));
        const std::size_t dot = name.find_last_of('.');
        const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
        if (!ext.empty() && ext != ".exe" && ext != ".com" && ext != ".bat" && ext != ".cmd")
            refuse(word, "only a program (.exe, .com) or a batch file (.bat, .cmd) can be run; run a script "
                         "through its program, such as powershell -NoProfile -File C:\\...\\x.ps1");
        const bool drive = word.size() >= 2 && word[1] == ':';
        if (!drive && std::any_of(name.begin(), name.end(), [](char c) {
                return !(std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-');
            }))
            refuse(word, "not a program name; write a program's name or its full path");
        const std::string base = ext.empty() ? name : name.substr(0, dot);
        // (Programs whose job is to start others, often through the user's own settings.)
        for (const char* banned : {"start", "if", "for", "forfiles", "mshta", "wscript", "cscript", "explorer",
                                   "rundll32", "regsvr32", "wmic", "pcalua", "conhost", "runas", "cmstp", "wsl",
                                   "bash", "hh", "mmc", "control", "msiexec", "ftp", "scriptrunner",
                                   // Changing what later commands find, or where.
                                   "set", "setlocal", "path", "cd", "chdir", "pushd",
                                   // Reading their settings from the user's own profile.
                                   "ssh", "scp", "sftp", "git", "scalar", "plink", "pscp", "psftp", "tortoiseplink",
                                   "tortoisegitplink", "putty", "puttytel", "pageant", "kitty", "klink", "winscp",
                                   "curl", "wget", "aws", "az", "gcloud", "gh", "hg", "svn", "npm", "npx", "node",
                                   "python", "python3", "py", "pip", "rclone", "gpg", "java", "javaw", "perl", "ruby",
                                   "php", "docker", "kubectl"})
            if (base == banned || base.starts_with("git-") || base.starts_with("pwsh") ||
                (base.starts_with("powershell") && base != "powershell"))
                refuse(word, "it can start programs these checks do not see, or reads settings any program of "
                             "yours can change; put the steps in a .cmd file in a folder only administrators can "
                             "change");
        // A bare name is found by cmd on the system's own path: only Windows' own tools for jobs
        // like these. Anything else is given by its full path (in a folder only administrators
        // can change, see check_path), so what runs is what was approved.
        if (!drive) {
            static const char* const kSystemTools[] = {
                "cmd", "powershell", "echo", "ver", "rem", "exit", "type", "timeout", "net", "net1", "sc",
                "vssadmin", "diskshadow", "wbadmin", "robocopy", "xcopy", "copy", "move", "del", "erase", "mkdir",
                "md", "rmdir", "rd", "ren", "rename", "dir", "icacls", "takeown", "attrib", "wevtutil",
                "eventcreate", "schtasks", "shutdown", "taskkill", "tasklist", "ping", "reg", "mountvol",
                "fsutil", "manage-bde", "sync", "waitfor", "choice", "find", "findstr", "sort", "more"};
            if (std::none_of(std::begin(kSystemTools), std::end(kSystemTools),
                             [&](const char* t) { return base == t; }))
                refuse(word, "only Windows' own tools (such as net, sc, robocopy or vssadmin) can be named "
                             "alone; give any other program by its full path, in a folder only administrators "
                             "can change");
        }
        return base;
    };
    // "-NoProfile", or any shortening of it PowerShell accepts ("-nop" at least).
    const auto no_profile = [&](const std::string& word) {
        const std::string w = lower(word);
        if (w.size() < 4 || (w[0] != '-' && w[0] != '/')) return false;
        return std::string("noprofile").rfind(w.substr(1), 0) == 0;
    };
    for (std::size_t i = 0; i < words.size(); ++i) {
        check_path(words[i].text);
        if (!words[i].starts_command) continue;
        std::string program = words[i].text;
        if (!program.empty() && program[0] == '@') program.erase(0, 1);
        if (lower(program) == "call") {  // "call x": x is the program
            if (i + 1 >= words.size() || words[i + 1].redirect_target) refuse(program, "nothing to call");
            words[i + 1].starts_command = true;
            continue;
        }
        const std::string base = check_program(program);
        if (base == "powershell") {
            // Its profile scripts are files the user can change, and text after -Command would be
            // a second command line these checks do not read: only "-NoProfile -File <full path>"
            // (the script in a folder only administrators can change), then its arguments.
            const auto is_file = [&](const std::string& w) {
                const std::string l = lower(w);
                return l == "-file" || l == "-f" || l == "/file" || l == "/f";
            };
            std::size_t k = i + 1;
            if (k >= words.size() || !no_profile(words[k].text))
                refuse(words[i].text, "PowerShell must be started with -NoProfile first");
            // Only within this command: its switches, -File and the script all before the next &,
            // |, ( or ) and not a redirection's file (else PowerShell, with no script of its own,
            // would read commands from its input).
            const auto own = [&](std::size_t at) {
                return at < words.size() && !words[at].starts_command && !words[at].redirect_target;
            };
            if (!own(k)) refuse(words[i].text, "PowerShell must be started with -NoProfile first");
            // PowerShell does not split its arguments at ; , or = as cmd does: refuse them, so the
            // words judged here are the ones it gets.
            if (command.find_first_of(";,=") != std::string::npos)
                refuse(words[i].text, "a command with PowerShell must not use ; , or =");
            // In order: switches, an execution policy only right after its switch, then -File.
            const auto script_only = [&](const std::string& w) {
                refuse(w, "PowerShell can only run a script: -NoProfile -File C:\\...\\x.ps1");
            };
            for (++k; own(k) && !is_file(words[k].text); ++k) {
                const std::string l = lower(words[k].text);
                if (l == "-noninteractive" || l == "-noni" || l == "-nologo") continue;
                if (l != "-executionpolicy" && l != "-ep") script_only(words[k].text);
                ++k;
                const std::string v = own(k) ? lower(words[k].text) : std::string();
                if (v != "bypass" && v != "remotesigned" && v != "allsigned" && v != "restricted")
                    script_only(words[i].text);
            }
            if (!own(k) || !own(k + 1) || words[k + 1].text.size() < 3 || words[k + 1].text[1] != ':')
                refuse(words[i].text, "PowerShell can only run a script: -NoProfile -File C:\\...\\x.ps1");
        } else if (base == "cmd") {
            // A cmd inside the command: without /d it runs the user's AutoRun command, without
            // /v:off it may expand !names!; what follows its /c or /k is a command again.
            bool autorun_off = false;
            bool expansion_off = false;
            std::size_t k = i + 1;
            for (; k < words.size(); ++k) {
                const std::string sw = lower(words[k].text);
                // Only switches known not to change how the rest is read.
                if (sw != "/d" && sw != "/v:off" && sw != "/e:on" && sw != "/q" && sw != "/s" && sw != "/c" &&
                    sw != "/k")
                    refuse(words[i].text, "cmd must be given /d /v:off and then /c, and no other switches");
                if (sw == "/d") autorun_off = true;
                if (sw == "/v:off") expansion_off = true;
                if (sw == "/c" || sw == "/k") break;
            }
            if (k >= words.size() || !autorun_off || !expansion_off)
                refuse(words[i].text, "cmd must be given /d /v:off and then /c");
            if (k + 1 < words.size() && !words[k + 1].redirect_target) words[k + 1].starts_command = true;
        }
    }
}

namespace {

// A job with administrator rights reads any file (a shadow copy included), writes the backup where
// the job says, and runs its commands, all as administrator. The job is copied from the user's
// own job file, which any program running as the user could have changed (the backup password is
// readable by the user too), and the approval prompt does not show what the job contains. So the
// elevated step shows what the job does and asks, unless the admin copy is exactly the same job
// with the same cloud keys. A program without administrator rights cannot answer this window. The
// commands and destinations come first; a job whose description does not fit is refused.
void confirm_job(const Job& job, const std::optional<Job>& approved, bool same_secrets, const AlertSettings& alerts,
                 bool same_alerts) {
    if (approved && same_secrets && same_alerts && to_json(*approved) == to_json(job)) return;
    if (job.sources.size() > 12 || job.excludes.size() > 12)
        throw Error("the job has too many folders or patterns to show in full for approval (at most 12 of each)");
    std::wstring text = L"The backup job \"" + shown(job.name, "the job's name") +
                        L"\" will run as the system account, with full administrator rights.\n\n";
    if (!job.before_command.empty())
        text += L"Command run before each backup:\n    " + shown(job.before_command, "the command before the backup") + L"\n";
    if (!job.after_command.empty())
        text += L"Command run after each backup:\n    " + shown(job.after_command, "the command after the backup") + L"\n";
    if (approved && same_secrets) text += L"(Some other setting of the job changed.)\n";
    if (!same_secrets) text += L"(Its cloud storage keys or sign-ins are new or changed.)\n";
    text += L"Saves the backup to:\n    " + shown(job.repository, "the destination") + L"\n";
    if (!job.cloud_key_id.empty()) text += L"    with the access key " + shown(job.cloud_key_id, "the access key") + L"\n";
    if (!job.cloud_region.empty())
        text += (cloud_kind(job.repository) == CloudKind::Sftp ? L"    trusting the server key " : L"    in the region ") +
                shown(job.cloud_region, "the server key") + L"\n";
    if (!job.second_copy.repository.empty()) {
        text += L"And a second copy to:\n    " + shown(job.second_copy.repository, "the second copy's destination") + L"\n";
        if (!job.second_copy.cloud_key_id.empty())
            text += L"    with the access key " + shown(job.second_copy.cloud_key_id, "the access key") + L"\n";
    }
    if (alerts.any()) {
        text += L"Sends alerts to:\n";
        if (!alerts.ntfy_url.empty()) text += L"    " + shown(alerts.ntfy_url, "the ntfy URL") + L"\n";
        if (!alerts.ntfy_email.empty()) text += L"    " + shown(alerts.ntfy_email, "the alert email") + L"\n";
        if (!alerts.telegram_chat.empty())
            text += L"    Telegram chat " + shown(alerts.telegram_chat, "the Telegram chat") + L"\n";
        if (!alerts.webhook_url.empty()) text += L"    " + shown(alerts.webhook_url, "the webhook URL") + L"\n";
    }
    text += L"Backs up:\n";
    for (const std::string& src : job.sources) text += L"    " + shown(src, "a folder to back up") + L"\n";
    if (!job.excludes.empty()) {
        text += L"Skipping:\n";
        for (const std::string& ex : job.excludes) text += L"    " + shown(ex, "a pattern to skip") + L"\n";
    }
    text += L"\nAnyone who knows the backup's password can read what it backs up. Allow this only if you set "
            L"the job up like this yourself. Allow?";
    // Everything must be on screen: a message box does not scroll. Count each line as it wraps,
    // assuming no more than 50 characters fit on one; every character shown is a narrow one, and a
    // message box is wider than that on any usual screen.
    std::size_t lines = 0;
    for (std::size_t start = 0; start <= text.size();) {
        std::size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) end = text.size();
        lines += std::max<std::size_t>(1, (end - start + 49) / 50);
        start = end + 1;
    }
    if (lines > 40)
        throw Error("the job's description is too long to show in full for approval; shorten its folders, "
                    "patterns or commands");
    const int answer = MessageBoxW(nullptr, text.c_str(), L"Vaultik: a job with administrator rights",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    if (answer != IDYES)
        throw Error("the job was not allowed to run with administrator rights; nothing was changed");
}

}  // namespace
#endif

#ifndef _WIN32
void check_admin_command(const std::string&, const std::string&) {}
#endif

namespace {

constexpr const char* kSubdirs[] = {"jobs", "secrets", "history"};

std::string vaultik_root() {
    return os::join(os::program_data_dir(), "Vaultik");
}

constexpr const char* kSystemSid = "S-1-5-18";

// The account a run as the system account works for (act_for_account).
std::string& acting_account() {
    static std::string sid;
    return sid;
}

std::string sid_or_current(const std::string& sid) {
    if (!sid.empty()) return sid;
    if (!acting_account().empty()) return acting_account();
    return os::current_user_sid();
}

// "S-1-5-21-...": it names folders and goes into task definitions.
bool account_sid(const std::string& sid) {
    return sid.size() > 8 && sid.size() <= 184 && sid.starts_with("S-1-5-") && sid != kSystemSid &&
           std::all_of(sid.begin() + 2, sid.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || c == '-'; });
}

// The admin store is trusted only when nobody without administrator rights can change it, or
// rename or re-permission a folder above it. Its subfolders are checked as strictly as the store.
void check_admin_store(const std::string& dir) {
    std::string why;
    bool ok = os::admin_only_chain(dir, &why);
    for (const char* sub : kSubdirs) {
        if (!ok) break;
        const std::string path = os::join(dir, sub);
        if (os::exists(path)) ok = os::admin_only(path, false, &why);
    }
    if (!ok) {
        throw Error("the administrator copy of the job is not protected (" + why +
                    "); save the job again in Vaultik to repair it");
    }
}

// Elevated side: the same account, with its full administrator token.
void check_elevated_caller(const std::string& user_sid) {
    if (!os::process_elevated()) throw Error("this step needs administrator rights");
    if (!account_sid(user_sid)) throw Error("not an account: " + user_sid);
    if (os::current_user_sid() != user_sid) {
        throw Error("the approval was given with a different account. A job can run with administrator rights "
                    "only for an account that is an administrator itself");
    }
}

// Creates (or repairs) the admin store of `sid` and returns it.
std::string prepare_admin_store(const std::string& sid) {
    const std::string root = vaultik_root();
    os::make_admin_dir(root);
    const std::string admin = os::join(root, "admin");
    os::make_admin_dir(admin);
    const std::string dir = os::join(admin, sid);
    os::make_admin_dir(dir, sid);
    for (const char* sub : kSubdirs) os::make_admin_dir(os::join(dir, sub), sid);
    return dir;
}

}  // namespace

std::string admin_store_dir(const std::string& sid) {
    return os::join(os::join(vaultik_root(), "admin"), sid_or_current(sid));
}

bool account_is_administrator(const std::string& sid_text) {
#ifdef _WIN32
    if (!account_sid(sid_text)) return false;
    PSID raw = nullptr;
    if (!ConvertStringSidToSidW(widen(sid_text).c_str(), &raw)) return false;
    const std::unique_ptr<void, decltype(&LocalFree)> sid(raw, &LocalFree);
    const auto name_of = [](PSID s, std::wstring* name, std::wstring* domain, SID_NAME_USE* use) {
        wchar_t n[256];
        wchar_t d[256];
        DWORD nn = 256;
        DWORD dn = 256;
        if (!LookupAccountSidW(nullptr, s, n, &nn, d, &dn, use)) return false;
        *name = n;
        *domain = d;
        return true;
    };
    std::wstring name;
    std::wstring domain;
    SID_NAME_USE use{};
    if (!name_of(sid.get(), &name, &domain, &use) || use != SidTypeUser) return false;
    // A disabled account of this computer (an account of a domain is the domain's to disable).
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD cn = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computer, &cn) && _wcsicmp(computer, domain.c_str()) == 0) {
        USER_INFO_1* info = nullptr;
        if (NetUserGetInfo(nullptr, name.c_str(), 1, reinterpret_cast<LPBYTE*>(&info)) != NERR_Success) return false;
        const bool disabled = (info->usri1_flags & UF_ACCOUNTDISABLE) != 0;
        NetApiBufferFree(info);
        if (disabled) return false;
    }
    // Administrators, directly or through a group (whatever the group is called in this language).
    BYTE admins_buf[SECURITY_MAX_SID_SIZE];
    DWORD admins_size = sizeof admins_buf;
    if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins_buf, &admins_size)) return false;
    std::wstring admins;
    std::wstring builtin;
    SID_NAME_USE group_use{};
    if (!name_of(admins_buf, &admins, &builtin, &group_use)) return false;
    LOCALGROUP_USERS_INFO_0* groups = nullptr;
    DWORD read = 0;
    DWORD total = 0;
    const std::wstring account = domain + L"\\" + name;
    if (NetUserGetLocalGroups(nullptr, account.c_str(), 0, LG_INCLUDE_INDIRECT, reinterpret_cast<LPBYTE*>(&groups),
                              MAX_PREFERRED_LENGTH, &read, &total) != NERR_Success)
        return false;
    bool member = false;
    for (DWORD i = 0; i < read && !member; ++i) member = _wcsicmp(groups[i].lgrui0_name, admins.c_str()) == 0;
    NetApiBufferFree(groups);
    return member;
#else
    (void)sid_text;
    return false;
#endif
}

void act_for_account(const std::string& sid) {
#ifdef _WIN32
    if (os::current_user_sid() != kSystemSid) throw Error("--user is only for runs as the system account");
    if (!account_sid(sid)) throw Error("not an account: " + sid);
    // Approved by an administrator, but the system account's rights are not that account's: an
    // account taken out of the administrators (or disabled) no longer has its jobs run with them.
    if (!account_is_administrator(sid))
        throw Error("the account that approved this job is no longer an administrator of this computer (or is "
                    "disabled), so the job does not run with administrator rights; an administrator can save it "
                    "again in Vaultik");
    acting_account() = sid;
#else
    (void)sid;
#endif
}

bool in_admin_store(const std::string& path) {
#ifdef _WIN32
    try {
        const std::string root = os::fold_case(os::join(os::join(vaultik_root(), "admin"), ""));
        return os::fold_case(os::absolute_path(path)).starts_with(root);
    } catch (const Error&) {
        return false;
    }
#else
    (void)path;
    return false;
#endif
}

std::string admin_error_file(const std::string& sid) {
#ifdef _WIN32
    return os::join(admin_store_dir(sid), "last-error.txt");
#else
    (void)sid;
    return {};
#endif
}

JobStore open_admin_store() {
    const std::string dir = admin_store_dir();
    check_admin_store(dir);
    return JobStore(dir);
}

void check_program_dir(const std::string& program_dir) {
    std::string why;
    bool ok = os::admin_only_chain(program_dir, &why);
    for (const char* exe : {"vaultik.exe", "vaultik-task.exe"}) {
        if (!ok) break;
        ok = os::admin_only(os::join(program_dir, exe), false, &why);
    }
    if (!ok) {
        throw Error("Vaultik runs from " + program_dir + ", but " + why +
                    ". A job with administrator rights would run whatever is put there, so install Vaultik "
                    "in a folder only administrators can change, such as C:\\Program Files\\Vaultik, and "
                    "run it from there");
    }
}

std::vector<RunRecord> combined_history(const JobStore& store, const std::string& id, std::size_t limit) {
    std::vector<RunRecord> all = store.history(id, limit);
#ifdef _WIN32
    try {
        const std::string dir = admin_store_dir();
        if (dir != store.dir() && os::exists(dir)) {
            std::vector<RunRecord> more = JobStore(dir).history(id, limit);
            all.insert(all.end(), more.begin(), more.end());
            std::stable_sort(all.begin(), all.end(),
                             [](const RunRecord& a, const RunRecord& b) { return a.started < b.started; });
            if (all.size() > limit) all.erase(all.begin(), all.end() - static_cast<std::ptrdiff_t>(limit));
        }
    } catch (const Error&) {
        // no admin store, or unreadable: the user's own history is still worth showing
    }
#endif
    return all;
}

bool connect_backup_due(const JobStore& store, const Job& job) {
    if (!job.backup_on_connect || job.repository_volume.empty() || !drive_connected(job)) return false;
    const std::vector<RunRecord> history = combined_history(store, job.id, 50);
    const RunRecord* last = last_backup(history);
    constexpr std::uint64_t kQuiet = 10ull * 60 * 10'000'000;  // 10 minutes in FILETIME ticks
    return !(last && last->status != RunStatus::Failed && last->finished + kQuiet > os::now());
}

bool event_backup_due(const JobStore& store, const Job& job) {
    if (!drive_connected(job)) return false;
    const std::vector<RunRecord> history = combined_history(store, job.id, 50);
    const RunRecord* last = last_backup(history);
    const std::uint64_t gap = static_cast<std::uint64_t>(std::clamp(job.schedule.event_gap_hours, 1, 168)) * 3600 * 10'000'000;
    return !(last && last->status != RunStatus::Failed && last->finished + gap > os::now());
}

bool has_admin_copy(const std::string& id) {
#ifdef _WIN32
    try {
        return os::exists(os::join(os::join(admin_store_dir(), "jobs"), id + ".json"));
    } catch (const Error&) {
        return false;
    }
#else
    (void)id;
    return false;
#endif
}

#ifdef _WIN32
namespace {
// Every file in the program folder admin-only too (below): a library planted there would load
// into an elevated run.
void check_whole_program_dir(const std::string& program_dir);

// A network share: "\\server\share\..." or a drive letter mapped to one.
bool on_network(const std::string& path) {
    if (path.empty() || is_cloud_location(path)) return false;
    std::string p = path;
    if (p.starts_with("\\\\?\\UNC\\") || p.starts_with("//?/UNC/")) return true;
    if (p.starts_with("\\\\?\\") || p.starts_with("\\\\.\\")) p = p.substr(4);
    if (p.starts_with("\\\\") || p.starts_with("//")) return true;
    if (p.size() < 2 || p[1] != ':') return false;
    const std::wstring root = widen(p.substr(0, 2)) + L"\\";
    return GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
}
}  // namespace
#endif

void install_admin_job(const JobStore& from, const std::string& id, const std::string& user_sid) {
#ifdef _WIN32
    check_elevated_caller(user_sid);
    const std::string program_dir = os::parent_of(os::executable_path());
    check_whole_program_dir(program_dir);

    // The job as the user saved it. Its password must open with this account's DPAPI key, so a
    // file planted in place of the user's secret cannot be passed through.
    std::optional<Job> job = from.find(id);
    if (!job || job->id != id) throw Error("there is no saved job \"" + id + "\"");
    if (!job->run_as_admin) throw Error("the job \"" + job->name + "\" is not set to run with administrator rights");
    // The run is the system account's, which has no sign-in for a network share.
    for (const std::string* where : {&job->repository, &job->second_copy.repository})
        if (on_network(*where))
            throw Error("a job with administrator rights runs as the system account, which cannot sign in to the "
                        "network share " + *where + "; back up to a drive of this computer or to a cloud or SFTP "
                        "location, or turn off administrator rights for the job");
    for (const std::string& src : job->sources)
        if (on_network(src))
            throw Error("a job with administrator rights runs as the system account, which cannot sign in to the "
                        "network share " + src + "; turn off administrator rights for the job to back it up");
    std::string pw = from.password(id);
    std::string cloud = from.cloud_secret(id);
    std::string copy = from.copy_secret(id);
    const auto wipe_all = [&] {
        secure_wipe(pw.data(), pw.size());
        secure_wipe(cloud.data(), cloud.size());
        secure_wipe(copy.data(), copy.size());
    };

    const std::string dir = prepare_admin_store(user_sid);
    const JobStore to(dir);
    try {
        // The cloud keys decide where the data goes as much as the job does.
        bool same_secrets = false;
        try {
            std::string old_cloud = to.cloud_secret(id);
            std::string old_copy = to.copy_secret(id);
            same_secrets = old_cloud == cloud && old_copy == copy;
            secure_wipe(old_cloud.data(), old_cloud.size());
            secure_wipe(old_copy.data(), old_copy.size());
        } catch (const std::exception&) {
            same_secrets = false;  // no readable approved copy: ask
        }
        // The alerts of administrator runs: the user's alert settings, copied into the admin store
        // with the job and approved with it (an elevated run never reads the user's own copy).
        const AlertSettings alerts = load_alerts(from.dir());
        std::string ntfy = alert_secret("ntfy", from.dir());
        std::string telegram = alert_secret("telegram", from.dir());
        std::string old_ntfy = alert_secret("ntfy", dir);
        std::string old_telegram = alert_secret("telegram", dir);
        const bool same_alerts = load_alerts(dir) == alerts && old_ntfy == ntfy && old_telegram == telegram;
        secure_wipe(old_ntfy.data(), old_ntfy.size());
        secure_wipe(old_telegram.data(), old_telegram.size());
        try {
            check_admin_command(job->before_command, "the command before the backup");
            check_admin_command(job->after_command, "the command after the backup");
            confirm_job(*job, to.find(id), same_secrets, alerts, same_alerts);
            save_alerts(alerts, dir);
            set_alert_secret("ntfy", ntfy, dir);
            set_alert_secret("telegram", telegram, dir);
        } catch (...) {
            secure_wipe(ntfy.data(), ntfy.size());
            secure_wipe(telegram.data(), telegram.size());
            throw;
        }
        secure_wipe(ntfy.data(), ntfy.size());
        secure_wipe(telegram.data(), telegram.size());
        to.save(*job);
        if (pw.empty()) {
            os::remove_file(os::join(os::join(dir, "secrets"), id + ".key"));
        } else {
            to.set_password(id, pw);
        }
        to.set_cloud_secret(id, cloud);
        to.set_copy_secret(id, copy);
    } catch (...) {
        wipe_all();
        throw;
    }
    wipe_all();

    // The task XML is written inside the admin store, where nobody else can swap it before
    // schtasks reads it.
    std::string output;
    if (create_task(*job, program_dir, dir, &output) != 0) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        throw Error("Task Scheduler refused the schedule: " + output);
    }
#else
    (void)from;
    (void)id;
    (void)user_sid;
    throw Error("jobs with administrator rights are only available on Windows");
#endif
}

void remove_admin_job(const std::string& id, const std::string& user_sid) {
#ifdef _WIN32
    check_elevated_caller(user_sid);
    // The id names the task and files: only an id a job can have, before anything is done.
    const bool valid = !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
    if (!valid) throw Error("not a job id: " + id);
    delete_task(id);
    const std::string dir = admin_store_dir(user_sid);
    if (!os::exists(dir)) return;
    // Delete files only in a store that is still admin-only: elsewhere a planted link could point
    // the deletion at something else.
    check_admin_store(dir);
    JobStore(dir).remove(id);
#else
    (void)id;
    (void)user_sid;
    throw Error("jobs with administrator rights are only available on Windows");
#endif
}

std::string elevated_gui_task(const std::string& sid) {
    return "\\Vaultik\\Vaultik as administrator " + sid_or_current(sid);
}

bool elevated_gui_installed() {
#ifdef _WIN32
    try {
        std::string ignored;
        return os::run_command({"schtasks.exe", "/Query", "/TN", elevated_gui_task()}, &ignored) == 0;
    } catch (const Error&) {
        return false;
    }
#else
    return false;
#endif
}

void set_elevated_gui(bool on) {
#ifdef _WIN32
    const std::string sid = os::current_user_sid();
    const std::string engine = os::join(os::parent_of(os::executable_path()), "vaultik.exe");
    const int code = os::run_command_elevated({engine, "admin-job", on ? "gui-on" : "gui-off", "--user", sid});
    if (code == 0) return;
    std::string message;
    try {
        const Bytes text = os::read_file(admin_error_file(sid), 64 * 1024);
        message.assign(text.begin(), text.end());
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
    } catch (const Error&) {
    }
    throw Error(message.empty() ? "the administrator step failed (exit code " + std::to_string(code) + ")" : message);
#else
    (void)on;
    throw Error("starting Vaultik as administrator is only available on Windows");
#endif
}

#ifdef _WIN32
namespace {

// Every file and folder of the program folder (Qt's libraries and plugins included: the app
// loads them all) must be admin-only, besides the checks of check_program_dir.
void check_whole_program_dir(const std::string& program_dir) {
    check_program_dir(program_dir);
    std::string why;
    if (!os::admin_only(os::join(program_dir, "vaultik-gui.exe"), false, &why))
        throw Error("Vaultik runs from " + program_dir + ", but " + why);
    std::error_code ec;
    namespace fs = std::filesystem;
    // Links are not followed: a linked folder is checked as the link itself.
    for (fs::recursive_directory_iterator it(fs::path(widen(program_dir)), ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string path = narrow(it->path().native());
        if (!os::admin_only(path, false, &why))
            throw Error("Vaultik runs from " + program_dir + ", but " + why +
                        ". Starting it as administrator would run whatever is put there, so install Vaultik in "
                        "a folder only administrators can change, such as C:\\Program Files\\Vaultik");
    }
    if (ec) throw Error("cannot check the program folder " + program_dir + ": " + ec.message());
}

std::string elevated_gui_definition(const std::string& program_dir, const std::string& sid) {
    const std::string gui = os::join(program_dir, "vaultik-gui.exe");
    // The account may run the task (that is the point) but not change it; SYSTEM and
    // administrators keep full control.
    return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
           "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           "  <RegistrationInfo>\n"
           "    <Description>Starts Vaultik with administrator rights, without an approval prompt.</Description>\n"
           "    <SecurityDescriptor>D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;" + sid + ")</SecurityDescriptor>\n"
           "  </RegistrationInfo>\n"
           "  <Triggers />\n"
           "  <Settings>\n"
           "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
           "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
           "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
           "    <AllowStartOnDemand>true</AllowStartOnDemand>\n"
           "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
           "    <Priority>4</Priority>\n"
           "    <Enabled>true</Enabled>\n"
           "  </Settings>\n"
           "  <Principals>\n    <Principal id=\"Author\">\n      <UserId>" + sid +
           "</UserId>\n      <LogonType>InteractiveToken</LogonType>\n"
           "      <RunLevel>HighestAvailable</RunLevel>\n    </Principal>\n  </Principals>\n"
           "  <Actions Context=\"Author\">\n"
           "    <Exec><Command>" + gui + "</Command><Arguments>--tray</Arguments></Exec>\n"
           "  </Actions>\n"
           "</Task>\n";
}

}  // namespace
#endif

void install_elevated_gui(const std::string& user_sid) {
#ifdef _WIN32
    check_elevated_caller(user_sid);
    const std::string program_dir = os::parent_of(os::executable_path());
    check_whole_program_dir(program_dir);
    if (program_dir.find_first_of("<>&\"") != std::string::npos)
        throw Error("the program folder's name cannot be put in a task: " + program_dir);
    // The XML goes into the admin store, where nobody else can swap it before schtasks reads it.
    const std::string dir = prepare_admin_store(user_sid);
    const std::string xml = elevated_gui_definition(program_dir, user_sid);
    const std::wstring wide = widen(xml);
    Bytes data = {0xFF, 0xFE};  // schtasks reads task XML as UTF-16 with a byte order mark
    for (const wchar_t c : wide) {
        data.push_back(static_cast<std::uint8_t>(c & 0xFF));
        data.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    std::uint8_t rnd[6];
    random_bytes(rnd);
    const std::string tmp = os::join(dir, "gui-task-" + to_hex(rnd) + ".xml");
    os::write_file_atomic(tmp, data);
    std::string output;
    int code = -1;
    try {
        code = os::run_command({"schtasks.exe", "/Create", "/TN", elevated_gui_task(user_sid), "/XML", tmp, "/F"}, &output);
    } catch (...) {
        os::remove_file(tmp);
        throw;
    }
    os::remove_file(tmp);
    if (code != 0) {
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        throw Error("Task Scheduler refused the task: " + output);
    }
#else
    (void)user_sid;
    throw Error("starting Vaultik as administrator is only available on Windows");
#endif
}

void remove_elevated_gui(const std::string& user_sid) {
#ifdef _WIN32
    check_elevated_caller(user_sid);
    std::string ignored;
    os::run_command({"schtasks.exe", "/Delete", "/TN", elevated_gui_task(user_sid), "/F"}, &ignored);
#else
    (void)user_sid;
    throw Error("starting Vaultik as administrator is only available on Windows");
#endif
}

}  // namespace vaultik::jobs
