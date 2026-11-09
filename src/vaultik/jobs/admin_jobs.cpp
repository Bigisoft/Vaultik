#include "vaultik/jobs/admin_jobs.hpp"

#include <algorithm>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/error.hpp"

namespace vaultik::jobs {

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
    const auto wipe_all = [&] {
        secure_wipe(pw.data(), pw.size());
        secure_wipe(cloud.data(), cloud.size());
    };

    const std::string dir = prepare_admin_store(user_sid);
    const JobStore to(dir);
    try {
        to.save(*job);
        if (pw.empty()) {
            os::remove_file(os::join(os::join(dir, "secrets"), id + ".key"));
        } else {
            to.set_password(id, pw);
        }
        to.set_cloud_secret(id, cloud);
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

}  // namespace vaultik::jobs
