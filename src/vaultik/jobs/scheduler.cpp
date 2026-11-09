#include "vaultik/jobs/scheduler.hpp"

#include <cstdio>

#include "vaultik/crypto/crypto.hpp"
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
    const std::string launcher = os::join(program_dir, "vaultik-task.exe");
    // The task runs as the user who registers it, while they are logged on. HighestAvailable gives
    // it the user's full administrator token, LeastPrivilege (the default) the filtered one.
    const std::string principals =
        std::string("  <Principals>\n    <Principal id=\"Author\">\n      <LogonType>InteractiveToken</LogonType>\n"
                    "      <RunLevel>") +
        (job.run_as_admin ? "HighestAvailable" : "LeastPrivilege") +
        "</RunLevel>\n    </Principal>\n  </Principals>\n";
    return "<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
           "<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
           "  <RegistrationInfo><Description>Vaultik backup job: " +
           xml_escape(job.name) +
           "</Description></RegistrationInfo>\n"
           "  <Triggers>\n" +
           trigger +
           "  </Triggers>\n"
           "  <Settings>\n"
           "    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
           "    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
           "    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
           "    <StartWhenAvailable>true</StartWhenAvailable>\n"
           "    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
           "    <Priority>7</Priority>\n"
           "    <Enabled>true</Enabled>\n"
           "  </Settings>\n" +
           principals +
           "  <Actions Context=\"Author\">\n"
           "    <Exec><Command>" +
           xml_escape(launcher) + "</Command><Arguments>run-job " + xml_escape(job.id) +
           "</Arguments></Exec>\n"
           "  </Actions>\n"
           "</Task>\n";
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
    std::string xml;
    try {
        if (os::run_command({"schtasks.exe", "/Query", "/TN", task_name(job_id), "/XML"}, &xml) != 0) return false;
    } catch (const Error&) {
        return false;
    }
    return xml.find("<RunLevel>HighestAvailable</RunLevel>") != std::string::npos;
}

}  // namespace

void apply_schedule(const Job& job, const std::string& program_dir) {
    if (job.schedule.kind == Schedule::Kind::Manual) {
        remove_schedule(job.id);
        return;
    }
    // schtasks reads task XML as UTF-16 with a byte order mark.
    const std::wstring xml = widen(schedule_definition(job, program_dir));
    Bytes data = {0xFF, 0xFE};
    for (const wchar_t c : xml) {
        data.push_back(static_cast<std::uint8_t>(c & 0xFF));
        data.push_back(static_cast<std::uint8_t>(c >> 8));
    }
    std::uint8_t rnd[6];
    random_bytes(rnd);
    const std::string tmp = os::join(config_dir(), "task-" + to_hex(rnd) + ".xml");
    os::write_file_atomic(tmp, data);
    const std::vector<std::string> create = {"schtasks.exe", "/Create", "/TN", task_name(job.id), "/XML", tmp, "/F"};
    // Replacing a task that runs as administrator needs the rights as well, also when the new one
    // does not.
    const bool elevated = job.run_as_admin || task_is_elevated(job.id);
    std::string output;
    int code = -1;
    try {
        code = elevated ? os::run_command_elevated(create) : os::run_command(create, &output);
    } catch (...) {
        os::remove_file(tmp);
        throw;
    }
    os::remove_file(tmp);
    if (code != 0) {
        if (elevated) throw Error("Task Scheduler refused the schedule (schtasks exit code " + std::to_string(code) + ")");
        throw Error("Task Scheduler refused the schedule: " + trim(output));
    }
}

void remove_schedule(const std::string& job_id) {
    const std::vector<std::string> remove = {"schtasks.exe", "/Delete", "/TN", task_name(job_id), "/F"};
    try {
        if (task_is_elevated(job_id)) {
            os::run_command_elevated(remove);
        } else {
            std::string ignored;
            os::run_command(remove, &ignored);
        }
    } catch (const Error&) {
    }
}

#else

void apply_schedule(const Job& job, const std::string& program_dir) {
    const std::string dir = unit_dir();
    const std::string base = "vaultik-" + job.id;
    if (job.schedule.kind == Schedule::Kind::Manual) {
        remove_schedule(job.id);
        return;
    }
    const std::string service = "[Unit]\nDescription=Vaultik backup job: " + job.name +
                                "\n\n[Service]\nType=oneshot\nExecStart=" +
                                unit_quote(os::join(program_dir, "vaultik")) + " run-job " + unit_quote(job.id) +
                                "\n";
    const std::string timer = "[Unit]\nDescription=Schedule for Vaultik backup job: " + job.name +
                              "\n\n[Timer]\nOnCalendar=" + schedule_definition(job, program_dir) +
                              "\nPersistent=true\n\n[Install]\nWantedBy=timers.target\n";
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
