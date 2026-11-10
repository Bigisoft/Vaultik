#include "vaultik/jobs/admin_jobs.hpp"

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"
#include "vaultik/util/hex.hpp"
#ifdef _WIN32
#include <windows.h>

#include "vaultik/util/win.hpp"
#endif

namespace vaultik::jobs {

#ifdef _WIN32
namespace {

// A field of the job for the question below, shown in full: control and text-direction
// characters (which could fake its layout or wording) become U+FFFD, and a run of more than 3
// spaces is shown as "[n spaces]" so it cannot push text out of view. A field too long to show in
// full is refused rather than cut: what is approved must be exactly what is seen.
std::wstring shown(const std::string& utf8, const std::string& what) {
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
        const bool hidden = c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x061C || c == 0x180E ||
                            (c >= 0x200B && c <= 0x200F) || (c >= 0x2028 && c <= 0x202E) ||
                            (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF;
        out += hidden ? L'\uFFFD' : c;
    }
    flush_spaces();
    if (out.size() > 400) throw Error(what + " is too long to show in full for approval; shorten it");
    return out;
}

// A job with administrator rights reads any file (a shadow copy included), writes the backup where
// the job says, and runs its commands, all as administrator. The job is copied from the user's
// own job file, which any program running as the user could have changed (the backup password is
// readable by the user too), and the approval prompt does not show what the job contains. So the
// elevated step shows what the job does and asks, unless the admin copy is exactly the same job
// with the same cloud keys. A program without administrator rights cannot answer this window.
void confirm_job(const Job& job, const std::optional<Job>& approved, bool same_secrets) {
    if (approved && same_secrets && to_json(*approved) == to_json(job)) return;
    std::wstring text = L"The backup job \"" + shown(job.name, "the job's name") + L"\" will run with administrator rights:\n\n";
    text += L"Back up:\n";
    if (job.sources.size() > 20)
        throw Error("the job backs up more than 20 folders and files, too many to show in full for approval");
    for (const std::string& src : job.sources) text += L"    " + shown(src, "a folder to back up") + L"\n";
    if (!job.excludes.empty()) {
        text += L"Skipping:  ";
        for (std::size_t i = 0; i < job.excludes.size() && i < 10; ++i)
            text += (i ? L", " : L"") + shown(job.excludes[i], "a pattern to skip");
        text += job.excludes.size() > 10 ? L", ...\n" : L"\n";
    }
    text += L"To:  " + shown(job.repository, "the destination") + L"\n";
    if (!job.cloud_key_id.empty()) text += L"    with the access key " + shown(job.cloud_key_id, "the access key") + L"\n";
    if (!job.second_copy.repository.empty()) {
        text += L"Second copy:  " + shown(job.second_copy.repository, "the second copy's destination") + L"\n";
        if (!job.second_copy.cloud_key_id.empty())
            text += L"    with the access key " + shown(job.second_copy.cloud_key_id, "the access key") + L"\n";
    }
    if (!job.before_command.empty()) text += L"Command before:  " + shown(job.before_command, "the command before the backup") + L"\n";
    if (!job.after_command.empty()) text += L"Command after:  " + shown(job.after_command, "the command after the backup") + L"\n";
    if (approved && same_secrets) text += L"\n(Some other setting of the job changed.)\n";
    if (!same_secrets) text += L"\n(Its cloud storage keys or sign-ins are new or changed.)\n";
    text += L"\nAnyone who knows the backup's password can read what it backs up. Allow this only if you set "
            L"the job up like this yourself. Allow?";
    const int answer = MessageBoxW(nullptr, text.c_str(), L"Vaultik: a job with administrator rights",
                                   MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    if (answer != IDYES)
        throw Error("the job was not allowed to run with administrator rights; nothing was changed");
}

}  // namespace
#endif

namespace {

constexpr const char* kSubdirs[] = {"jobs", "secrets", "history"};

std::string vaultik_root() {
    return os::join(os::program_data_dir(), "Vaultik");
}

std::string sid_or_current(const std::string& sid) {
    return sid.empty() ? os::current_user_sid() : sid;
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

void install_admin_job(const JobStore& from, const std::string& id, const std::string& user_sid) {
#ifdef _WIN32
    check_elevated_caller(user_sid);
    const std::string program_dir = os::parent_of(os::executable_path());
    check_program_dir(program_dir);

    // The job as the user saved it. Its password must open with this account's DPAPI key, so a
    // file planted in place of the user's secret cannot be passed through.
    std::optional<Job> job = from.find(id);
    if (!job || job->id != id) throw Error("there is no saved job \"" + id + "\"");
    if (!job->run_as_admin) throw Error("the job \"" + job->name + "\" is not set to run with administrator rights");
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
        confirm_job(*job, to.find(id), same_secrets);
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
