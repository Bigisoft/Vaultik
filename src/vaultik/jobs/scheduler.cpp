#include "vaultik/jobs/scheduler.hpp"

#include <cstdio>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/util/hex.hpp"

#ifdef _WIN32
#include "vaultik/util/win.hpp"
#endif

namespace vaultik::jobs {

namespace {

std::string two(int v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02d", v);
    return buf;
}

ByteSpan bytes_of(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

std::string xml_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

#ifdef _WIN32

constexpr const char* kTaskFolder = "\\Vaultik\\";
const char* kDayElements[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

std::string task_name(const std::string& id) {
    return kTaskFolder + id;
}

// The second task, started by the computer being idle or locked.
std::string events_task_name(const std::string& id) {
    return kTaskFolder + id + "-events";
}

#else

std::string unit_dir() {
    const std::string xdg = os::get_env("XDG_CONFIG_HOME");
    const std::string base = !xdg.empty() ? xdg : os::join(os::get_env("HOME"), ".config");
    return os::join(os::join(base, "systemd"), "user");
}

// systemd unit files quote arguments with double quotes; backslashes and quotes are escaped.
std::string unit_quote(const std::string& s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (c == '%') {
            out += "%%";
            continue;
        }
        out.push_back(c);
    }
    return out + "\"";
}

#endif

}  // namespace

#ifdef _WIN32

namespace {

// A job's task: `triggers` start `vaultik-task run-job <id><args>`. A job without administrator
// rights runs as the user who registers it, while they are logged on, with the filtered token.
// One with them runs as the system account (logged on or not): the user's own profile, registry
// and environment, which any program of theirs can change, play no part. The account it works for
// is passed on (its admin store); that account may read the task, and only administrators change
// it.
std::string task_xml(const Job& job, const std::string& program_dir, const std::string& triggers,
                     const std::string& extra_settings, const std::string& args, bool start_when_available) {
    const std::string launcher = os::join(program_dir, "vaultik-task.exe");
    const std::string sid = job.run_as_admin ? os::current_user_sid() : std::string();
    const std::string principals =
        job.run_as_admin
            ? "  <Principals>\n    <Principal id=\"Author\">\n      <UserId>S-1-5-18</UserId>\n"
              "      <RunLevel>HighestAvailable</RunLevel>\n    </Principal>\n  </Principals>\n"
            : "  <Principals>\n    <Principal id=\"Author\">\n      <LogonType>InteractiveToken</LogonType>\n"
              "      <RunLevel>LeastPrivilege</RunLevel>\n    </Principal>\n  </Principals>\n";
    const std::string security =
        job.run_as_admin ? "<SecurityDescriptor>D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;" + xml_escape(sid) +
                               ")</SecurityDescriptor>"
                         : std::string();
    return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
           "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           "  <RegistrationInfo><Description>Vaultik backup job: " +
           xml_escape(job.name) + "</Description>" + security +
           "</RegistrationInfo>\n"
           "  <Triggers>\n" +
           triggers +
           "  </Triggers>\n"
           "  <Settings>\n"
           "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
           "    <DisallowStartIfOnBatteries>" +
           std::string(job.schedule.skip_on_battery ? "true" : "false") +
           "</DisallowStartIfOnBatteries>\n"
           "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
           "    <StartWhenAvailable>" + std::string(start_when_available ? "true" : "false") + "</StartWhenAvailable>\n"
           "    <WakeToRun>" +
           std::string(job.schedule.wake_computer && start_when_available ? "true" : "false") +
           "</WakeToRun>\n" + extra_settings +
           "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
           "    <Priority>7</Priority>\n"
           "    <Enabled>true</Enabled>\n"
           "  </Settings>\n" +
           principals +
           "  <Actions Context=\"Author\">\n"
           "    <Exec><Command>" +
           xml_escape(launcher) + "</Command><Arguments>run-job " + xml_escape(job.id) + args +
           // An administrator job reads only the admin-only copy of itself (see admin_jobs.hpp).
           (job.run_as_admin ? " --admin --user " + xml_escape(sid) : std::string()) +
           "</Arguments></Exec>\n"
           "  </Actions>\n"
           "</Task>\n";
}

}  // namespace

std::string events_definition(const Job& job, const std::string& program_dir) {
    const Schedule& s = job.schedule;
    if (!s.on_idle && !s.on_lock && !s.on_update) return {};
    std::string triggers;
    std::string settings;
    if (s.on_idle) {
        triggers += "    <IdleTrigger><Enabled>true</Enabled></IdleTrigger>\n";
        // Idle for 10 minutes; the backup goes on when the user comes back.
        settings += "    <IdleSettings><Duration>PT10M</Duration><WaitTimeout>PT1H</WaitTimeout>"
                    "<StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\n"
                    "    <RunOnlyIfIdle>false</RunOnlyIfIdle>\n";
    }
    if (s.on_lock) {
        // Only this user locking the computer (an administrator job's task runs as the system
        // account, so the account is named either way).
        triggers += "    <SessionStateChangeTrigger><StateChange>SessionLock</StateChange><UserId>" +
                    xml_escape(os::current_user_sid()) + "</UserId></SessionStateChangeTrigger>\n";
    }
    if (s.on_update) {
        // Windows Update starting to install an update (event 43 in the System log, which every
        // user may read). The query is XML inside the task's XML, so it is escaped once more.
        const std::string query = "<QueryList><Query Id=\"0\" Path=\"System\"><Select Path=\"System\">" +
                                  xml_escape(kUpdateEventQuery) + "</Select></Query></QueryList>";
        triggers += "    <EventTrigger><Enabled>true</Enabled><Subscription>" + xml_escape(query) +
                    "</Subscription></EventTrigger>\n";
    }
    return task_xml(job, program_dir, triggers, settings, " --scheduled --on-event", false);
}

#endif

std::vector<std::string> update_titles(const std::string& xml) {
    std::vector<std::string> out;
    const std::string open = "<Data Name='updateTitle'>";
    for (std::size_t at = xml.find(open); at != std::string::npos; at = xml.find(open, at)) {
        at += open.size();
        const std::size_t end = xml.find("</Data>", at);
        if (end == std::string::npos) break;
        std::string title = xml.substr(at, end - at);
        // The few entities XML output uses.
        for (const auto& [entity, c] : {std::pair<const char*, char>{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
                                        {"&apos;", '\''}, {"&amp;", '&'}}) {
            for (std::size_t p = title.find(entity); p != std::string::npos; p = title.find(entity, p + 1))
                title.replace(p, std::char_traits<char>::length(entity), 1, c);
        }
        out.push_back(std::move(title));
        at = end;
    }
    return out;
}

bool routine_update(const std::string& title) {
    if (title.find("KB2267602") != std::string::npos || title.starts_with("Security Intelligence Update"))
        return true;
    // A Microsoft Store package: a 12-character product id, a dash, the package name.
    if (title.size() > 13 && title[12] == '-') {
        bool id = true;
        for (std::size_t i = 0; i < 12 && id; ++i)
            id = (title[i] >= '0' && title[i] <= '9') || (title[i] >= 'A' && title[i] <= 'Z');
        if (id && title.find(' ') == std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> recent_update_installs(int minutes) {
#ifdef _WIN32
    const std::string query = std::string(kUpdateEventQuery).insert(std::string(kUpdateEventQuery).size() - 2,
        " and TimeCreated[timediff(@SystemTime) <= " + std::to_string(static_cast<long long>(minutes) * 60000) + "]");
    std::string output;
    try {
        if (os::run_command({"wevtutil.exe", "qe", "System", "/q:" + query, "/rd:true", "/c:20", "/f:xml"}, &output) != 0)
            return {};
    } catch (const Error&) {
        return {};
    }
    return update_titles(output);
#else
    (void)minutes;
    return {};
#endif
}

std::string events_trigger(const Schedule& s, const std::vector<std::string>& recent) {
    if (!s.on_update) return "event";
    bool any = false, real = false;
    for (const std::string& t : recent) {
        any = true;
        if (!routine_update(t)) real = true;
    }
    if (real) return "update";
    if (s.on_idle || s.on_lock) return "event";  // idle or lock it was, or a routine update: run as for those
    // Updates are the only trigger: a routine one is skipped; when the log said nothing, trust the trigger.
    return any ? std::string() : std::string("update");
}

namespace {
std::string update_marker(const std::string& dir, const std::string& job_id) {
    return os::join(os::join(dir, "running"), "update-" + job_id);
}
}  // namespace

void mark_update_run(const std::string& dir, const std::string& job_id) {
    try {
        os::make_dirs(os::join(dir, "running"));
        os::write_file_atomic(update_marker(dir, job_id), bytes_of(std::to_string(os::process_id())));
    } catch (const Error&) {
        // best effort: without it the restart is only not held up
    }
}

void clear_update_run(const std::string& dir, const std::string& job_id) {
    try {
        os::remove_file(update_marker(dir, job_id));
    } catch (const Error&) {
    }
}

std::vector<std::uint32_t> running_update_backups(const std::string& dir) {
    std::vector<std::uint32_t> out;
    const std::string folder = os::join(dir, "running");
    try {
        if (!os::exists(folder)) return out;
        os::DirReader reader(folder);
        std::string name;
        os::FileInfo info;
        std::vector<std::string> ended;
        while (reader.next(name, info)) {
            if (!name.starts_with("update-")) continue;
            std::uint32_t pid = 0;
            try {
                const Bytes text = os::read_file(os::join(folder, name), 64);
                pid = static_cast<std::uint32_t>(std::stoul(std::string(text.begin(), text.end())));
            } catch (const std::exception&) {
                continue;
            }
            if (pid != 0 && os::process_alive(pid)) out.push_back(pid);
            else ended.push_back(name);
        }
        for (const std::string& n : ended) os::remove_file(os::join(folder, n));
    } catch (const std::exception&) {
    }
    return out;
}

std::string schedule_definition(const Job& job, const std::string& program_dir) {
    const Schedule& s = job.schedule;
#ifdef _WIN32
    const std::string start = "2026-01-01T" + two(s.kind == Schedule::Kind::Hourly ? 0 : s.hour) + ":" +
                              two(s.minute) + ":00";
    std::string trigger;
    switch (s.kind) {
        case Schedule::Kind::Manual:
            break;
        case Schedule::Kind::Hourly:
            trigger = "    <TimeTrigger>\n      <Repetition><Interval>PT" + std::to_string(s.every_hours) +
                      "H</Interval></Repetition>\n      <StartBoundary>" + start +
                      "</StartBoundary>\n    </TimeTrigger>\n";
            break;
        case Schedule::Kind::Daily:
            trigger = "    <CalendarTrigger>\n      <StartBoundary>" + start +
                      "</StartBoundary>\n      <ScheduleByDay><DaysInterval>1</DaysInterval></ScheduleByDay>\n"
                      "    </CalendarTrigger>\n";
            break;
        case Schedule::Kind::Weekly: {
            std::string days;
            for (int d = 0; d < 7; ++d)
                if (s.weekdays & (1u << d)) days += std::string("<") + kDayElements[d] + "/>";
            trigger = "    <CalendarTrigger>\n      <StartBoundary>" + start +
                      "</StartBoundary>\n      <ScheduleByWeek><DaysOfWeek>" + days +
                      "</DaysOfWeek><WeeksInterval>1</WeeksInterval></ScheduleByWeek>\n    </CalendarTrigger>\n";
            break;
        }
    }
    return task_xml(job, program_dir, trigger, std::string(), " --scheduled", true);
#else
    (void)program_dir;
    switch (s.kind) {
        case Schedule::Kind::Manual:
            return {};
        case Schedule::Kind::Hourly:
            return "*-*-* 00/" + std::to_string(s.every_hours) + ":" + two(s.minute) + ":00";
        case Schedule::Kind::Daily:
            return "*-*-* " + two(s.hour) + ":" + two(s.minute) + ":00";
        case Schedule::Kind::Weekly: {
            static const char* kDays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
            std::string days;
            for (int d = 0; d < 7; ++d)
                if (s.weekdays & (1u << d)) days += (days.empty() ? "" : ",") + std::string(kDays[d]);
            return days + " *-*-* " + two(s.hour) + ":" + two(s.minute) + ":00";
        }
    }
    return {};
#endif
}

#ifdef _WIN32

namespace {

// Whether the job's current task runs with administrator rights. Changing or deleting such a task
// needs those rights too.
bool task_is_elevated(const std::string& job_id) {
    for (const std::string& name : {task_name(job_id), events_task_name(job_id)}) {
        std::string xml;
        try {
            if (os::run_command({"schtasks.exe", "/Query", "/TN", name, "/XML"}, &xml) != 0) continue;
        } catch (const Error&) {
            continue;
        }
        if (xml.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos) return true;
    }
    return false;
}

// Runs "vaultik admin-job <verb> <id> ..." with administrator approval and turns a failure into an
// Error carrying the message the elevated process left in the admin store.
void run_admin_step(const std::string& program_dir, const std::string& verb, const std::string& job_id) {
    const std::string sid = os::current_user_sid();
    std::vector<std::string> argv = {os::join(program_dir, "vaultik.exe"), "admin-job", verb, job_id, "--user", sid};
    if (verb == "install") {
        argv.push_back("--from");
        argv.push_back(config_dir());
    }
    const int code = os::run_command_elevated(argv);
    if (code == 0) return;
    std::string message;
    try {
        const Bytes text = os::read_file(admin_error_file(sid), 64 * 1024);
        message = trim(std::string(text.begin(), text.end()));
    } catch (const Error&) {
    }
    if (message.empty()) message = "the administrator step failed (exit code " + std::to_string(code) + ")";
    throw Error(message);
}

}  // namespace

namespace {

// Registers one task from its XML, through a file in `temp_dir`.
int register_task(const std::string& name, const std::string& definition, const std::string& temp_dir, std::string* output) {
    // schtasks reads task XML as UTF-16 with a byte order mark.
    const std::wstring xml = widen(definition);
    Bytes data = {0xFF, 0xFE};
    for (const wchar_t c : xml) {
        data.push_back(static_cast<std::uint8_t>(c & 0xFF));
        data.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    std::uint8_t rnd[6];
    random_bytes(rnd);
    const std::string tmp = os::join(temp_dir, "task-" + to_hex(rnd) + ".xml");
    os::write_file_atomic(tmp, data);
    int code = -1;
    try {
        code = os::run_command({"schtasks.exe", "/Create", "/TN", name, "/XML", tmp, "/F"}, output);
    } catch (...) {
        os::remove_file(tmp);
        throw;
    }
    os::remove_file(tmp);
    return code;
}

void delete_named_task(const std::string& name) {
    std::string ignored;
    try {
        os::run_command({"schtasks.exe", "/Delete", "/TN", name, "/F"}, &ignored);
    } catch (const Error&) {
    }
}

}  // namespace

int create_task(const Job& job, const std::string& program_dir, const std::string& temp_dir, std::string* output) {
    // The time schedule, and a second task for the idle and lock triggers: those runs check
    // first whether the last backup was recent (--on-event), which scheduled ones must not.
    int code = 0;
    if (job.schedule.kind != Schedule::Kind::Manual)
        code = register_task(task_name(job.id), schedule_definition(job, program_dir), temp_dir, output);
    else
        delete_named_task(task_name(job.id));
    if (code != 0) return code;
    const std::string events = events_definition(job, program_dir);
    if (!events.empty()) return register_task(events_task_name(job.id), events, temp_dir, output);
    delete_named_task(events_task_name(job.id));
    return 0;
}

void delete_task(const std::string& job_id) {
    delete_named_task(task_name(job_id));
    delete_named_task(events_task_name(job_id));
}

void apply_schedule(const Job& job, const std::string& program_dir) {
    if (!has_system_schedule(job.schedule)) {
        remove_schedule(job.id);
        return;
    }
    if (job.run_as_admin) {
        // Checked here too, so an unsafe install is reported before Windows asks for approval.
        check_program_dir(program_dir);
        run_admin_step(program_dir, "install", job.id);
        return;
    }
    // Replacing an administrator task, or dropping the admin copy of the job, needs the rights,
    // and the program run with them must come from a folder only administrators can change.
    if (task_is_elevated(job.id) || has_admin_copy(job.id)) {
        check_program_dir(program_dir);
        run_admin_step(program_dir, "remove", job.id);
    }
    std::string output;
    if (create_task(job, program_dir, config_dir(), &output) != 0)
        throw Error("Task Scheduler refused the schedule: " + trim(output));
}

void remove_schedule(const std::string& job_id) {
    try {
        if (task_is_elevated(job_id) || has_admin_copy(job_id)) {
            const std::string program_dir = os::parent_of(os::executable_path());
            check_program_dir(program_dir);  // the program run with the rights: admin-only folder
            run_admin_step(program_dir, "remove", job_id);
        } else {
            delete_task(job_id);
        }
    } catch (const Error&) {
    }
}

#else

void apply_schedule(const Job& job, const std::string& program_dir) {
    const std::string dir = unit_dir();
    const std::string base = "vaultik-" + job.id;
    if (!has_system_schedule(job.schedule)) {
        remove_schedule(job.id);
        return;
    }
    const std::string service = "[Unit]\nDescription=Vaultik backup job: " + job.name + "\n" +
                                (job.schedule.skip_on_battery ? "ConditionACPower=true\n" : "") +
                                "\n[Service]\nType=oneshot\nExecStart=" +
                                unit_quote(os::join(program_dir, "vaultik")) + " run-job " + unit_quote(job.id) +
                                " --scheduled\n";
    const std::string timer = "[Unit]\nDescription=Schedule for Vaultik backup job: " + job.name +
                              "\n\n[Timer]\nOnCalendar=" + schedule_definition(job, program_dir) +
                              "\nPersistent=true\n" + (job.schedule.wake_computer ? "WakeSystem=true\n" : "") +
                              "\n[Install]\nWantedBy=timers.target\n";
    os::write_file_atomic(os::join(dir, base + ".service"), bytes_of(service));
    os::write_file_atomic(os::join(dir, base + ".timer"), bytes_of(timer));

    std::string output;
    if (os::run_command({"systemctl", "--user", "daemon-reload"}, &output) != 0)
        throw Error("systemctl daemon-reload failed: " + trim(output));
    output.clear();
    if (os::run_command({"systemctl", "--user", "enable", "--now", base + ".timer"}, &output) != 0)
        throw Error("cannot enable the systemd timer: " + trim(output));
}

void remove_schedule(const std::string& job_id) {
    const std::string dir = unit_dir();
    const std::string base = "vaultik-" + job_id;
    try {
        std::string ignored;
        os::run_command({"systemctl", "--user", "disable", "--now", base + ".timer"}, &ignored);
        os::remove_file(os::join(dir, base + ".timer"));
        os::remove_file(os::join(dir, base + ".service"));
        os::run_command({"systemctl", "--user", "daemon-reload"}, &ignored);
    } catch (const Error&) {
    }
}

#endif

}  // namespace vaultik::jobs
