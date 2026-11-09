#pragma once

#include <QString>

#include "vaultik/jobs/job.hpp"

// The frontend's own preferences (not part of any job), kept in the user's registry hive.
struct AppSettings {
    bool notify_failures = true;  // a notification when a backup fails or skips files
    bool notify_overdue = true;   // a notification when a job has not backed up for too long
    int remind_after_days = 7;    // when a job without a schedule counts as overdue (0: never)

    static AppSettings load();
    void save() const;
};

// Whether Vaultik starts in the tray when the user signs in (HKCU ...\Run), and changes it.
bool starts_with_windows();
void set_start_with_windows(bool on);

// How many days without a backup make a job overdue: about twice its schedule's interval, or
// `remind_after_days` for a job without one (0: never overdue).
int overdue_after_days(const vaultik::jobs::Job& job, int remind_after_days);

// The start time (FILETIME) of the job's last run that saved a version, 0 when there is none.
std::uint64_t last_saved(const vaultik::jobs::JobStore& store, const vaultik::jobs::Job& job);
