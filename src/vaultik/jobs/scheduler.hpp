#pragma once

// Registers saved jobs with the operating system's scheduler, so backups run without Vaultik
// staying in memory:
//   Windows: a Task Scheduler task "\Vaultik\<id>" for the current user, started through the
//            windowless launcher vaultik-task.exe so no console window flashes up.
//   Linux:   a systemd user timer vaultik-<id>.timer with Persistent=true (missed runs catch up).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

// Creates, replaces or (for a Manual schedule) removes the job's OS schedule. `program_dir` is
// the folder holding vaultik (and vaultik-task on Windows). Throws Error with the scheduler's
// message on failure. On Windows, a job with run_as_admin is installed through an approved
// "vaultik admin-job install" (see admin_jobs.hpp), which first needs `program_dir` to be
// admin-only; replacing an administrator task asks for approval as well. A declined approval
// throws Error.
void apply_schedule(const Job& job, const std::string& program_dir);

// Pauses or resumes saved job `job`: saves it with schedule.paused set to `paused`, then applies
// its schedule with `apply` (apply_schedule, which for a job with administrator rights asks for
// approval). When that fails (the approval was declined, the scheduler refused), the job is saved
// again as it was and the Error is thrown on: a job never shows paused while its tasks (and the
// approved copy an administrator run reads) still run it. `job` is left as saved.
void set_paused(const JobStore& store, Job& job, bool paused, const std::function<void(const Job&)>& apply);

// Removes the job's OS schedule if there is one; one that runs as administrator (or an admin copy
// of the job) asks for approval. Never throws.
void remove_schedule(const std::string& job_id);

#ifdef _WIN32
// Registers the job's task directly, with the rights of this process, writing the task XML into
// `temp_dir` (which must be a folder only this account, or only administrators, can change).
// Returns the schtasks exit code; its output goes to `output`.
int create_task(const Job& job, const std::string& program_dir, const std::string& temp_dir, std::string* output);
// Deletes the job's task, if any, with the rights of this process.
void delete_task(const std::string& job_id);
#endif

// The scheduler definition for a job, exposed for tests: Task Scheduler XML on Windows, the
// systemd timer's OnCalendar= value on Linux.
std::string schedule_definition(const Job& job, const std::string& program_dir);

#ifdef _WIN32
// The second task's XML, started when the computer is idle, the user locks it or Windows Update
// starts installing an update (empty when the job has none of these); exposed for tests.
std::string events_definition(const Job& job, const std::string& program_dir);
#endif

// Backing up before Windows updates. Windows Update writes event 43 ("Installation Started") of
// Microsoft-Windows-WindowsUpdateClient to the System log when it starts installing an update
// (the provider's manifest puts it there, not in the WindowsUpdateClient/Operational log), with
// the update's title in EventData/updateTitle. The events task has an event trigger for it. The
// same event comes for the daily Defender definitions and for Microsoft Store apps, which are not
// worth a backup, so a run started by the events task first looks at what was installed.
constexpr const char* kUpdateEventQuery =
    "*[System[Provider[@Name='Microsoft-Windows-WindowsUpdateClient'] and EventID=43]]";
// The updateTitle values in wevtutil's XML output (any number of <Event> elements).
std::vector<std::string> update_titles(const std::string& events_xml);
// Whether an update is routine (Defender security intelligence, a Store app's package, e.g.
// "9NRZT3Q9R3DL-Microsoft.WindowsAppRuntime.2"), so not worth a backup.
bool routine_update(const std::string& title);
// The titles of the updates Windows started installing in the last `minutes` (newest first);
// empty when none or when the log cannot be read. Windows only (empty elsewhere).
std::vector<std::string> recent_update_installs(int minutes);
// What started a run of the events task, from the job's triggers and the updates started lately:
// "update" when a real update started (or updates are its only trigger and the log could not
// tell), "event" when the idle or lock trigger must have started it, and "" when only a routine
// update did (the run is skipped).
std::string events_trigger(const Schedule& schedule, const std::vector<std::string>& recent_updates);

// While a backup started by an update runs, it leaves <dir>/running/update-<job id> holding its
// process id, so the tray can hold up the restart that follows the update until it is done.
void mark_update_run(const std::string& dir, const std::string& job_id);
void clear_update_run(const std::string& dir, const std::string& job_id);
// The process ids of the update backups running now (files of processes that ended are removed).
std::vector<std::uint32_t> running_update_backups(const std::string& dir);

}  // namespace vaultik::jobs
