#pragma once

#include <QString>

#include "vaultik/jobs/job.hpp"

// The frontend's own preferences (not part of any job), kept in the user's registry hive.
struct AppSettings {
    // Notification area
    bool tray_icon = true;           // show Vaultik's icon while it runs
    bool tray_status = true;         // the icon turns red when a backup needs attention
    bool close_to_tray = true;       // closing the window keeps Vaultik running in the tray
    bool minimize_to_tray = false;   // minimizing hides the window to the tray
    bool single_click_toggle = true; // a single click on the icon shows or hides the window

    // Notifications
    bool notify_failures = true;  // a notification when a backup fails or skips files
    bool notify_overdue = true;   // a notification when a job has not backed up for too long
    int remind_after_days = 7;    // when a job without a schedule counts as overdue (0: never)

    static AppSettings load();
    void save() const;
};

// Starting with Windows (a Run entry for the current user), optionally hidden in the tray.
struct StartupSetting {
    bool enabled = false;
    bool hidden = false;  // starts with --tray: no window, only the tray icon
};
StartupSetting startup_setting();
void set_startup_setting(const StartupSetting& setting);

// How many days without a backup make a job overdue: about twice its schedule's interval, or
// `remind_after_days` for a job without one (0: never overdue).
int overdue_after_days(const vaultik::jobs::Job& job, int remind_after_days);

// The start time (FILETIME) of the job's last run that saved a version, 0 when there is none.
std::uint64_t last_saved(const vaultik::jobs::JobStore& store, const vaultik::jobs::Job& job);
