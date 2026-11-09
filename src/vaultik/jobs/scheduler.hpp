#pragma once

// Registers saved jobs with the operating system's scheduler, so backups run without Vaultik
// staying in memory:
//   Windows: a Task Scheduler task "\Vaultik\<id>" for the current user, started through the
//            windowless launcher vaultik-task.exe so no console window flashes up.
//   Linux:   a systemd user timer vaultik-<id>.timer with Persistent=true (missed runs catch up).

#include <string>

#include "vaultik/jobs/job.hpp"

namespace vaultik::jobs {

// Creates, replaces or (for a Manual schedule) removes the job's OS schedule. `program_dir` is
// the folder holding vaultik (and vaultik-task on Windows). Throws Error with the scheduler's
// message on failure. On Windows, a job with run_as_admin (or replacing a task that had it) asks
// for administrator approval, and a declined approval throws Error.
void apply_schedule(const Job& job, const std::string& program_dir);

// Removes the job's OS schedule if there is one; one that runs as administrator asks for approval.
// Never throws.
void remove_schedule(const std::string& job_id);

// The scheduler definition for a job, exposed for tests: Task Scheduler XML on Windows, the
// systemd timer's OnCalendar= value on Linux.
std::string schedule_definition(const Job& job, const std::string& program_dir);

}  // namespace vaultik::jobs
