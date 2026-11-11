#pragma once

#include <QDate>
#include <QObject>
#include <QTimer>

#include <functional>
#include <map>
#include <string>

#include "vaultik/jobs/job.hpp"

class MainWindow;
class QAction;
class QMenu;
class QSystemTrayIcon;

// Vaultik in the notification area: an icon showing whether every job is fine, a menu to open the
// window or start a backup, and notifications when a backup fails, skips files, or a job has not
// backed up for too long. It reads the jobs' run history, so it also sees scheduled runs.
class Tray : public QObject {
    Q_OBJECT

public:
    Tray(MainWindow* window, const vaultik::jobs::JobStore& store);
    ~Tray() override;

    static bool available();
    void refresh_icons();
    // Looks at every job's history now (also called every minute).
    void check();
    // Tells the user, once, that closing the window left Vaultik running here.
    void explain_hidden();
    // A notification from the icon; `warning` shows it as one. A click on it runs `on_click`, or
    // without one brings the window to the front.
    void notify(const QString& title, const QString& text, bool warning = false, std::function<void()> on_click = {});

private:
    void rebuild_menu();
    // Shows a notification whose click brings the window to the front (icon: QSystemTrayIcon::MessageIcon).
    void show_message(const QString& title, const QString& text, int kind, int msecs);

    MainWindow* window_;
    const vaultik::jobs::JobStore& store_;
    QSystemTrayIcon* icon_ = nullptr;
    QMenu* menu_ = nullptr;  // deleted with the tray (a tray icon does not own its menu)
    QMenu* run_menu_ = nullptr;
    QAction* open_ = nullptr;
    QAction* settings_ = nullptr;
    QAction* report_ = nullptr;
    std::function<void()> click_;  // what a click on the newest notification does
    QTimer timer_;
    bool first_check_ = true;
    bool attention_ = false;
    std::map<std::string, std::uint64_t> seen_;      // job id -> start of the newest run already reported
    std::map<std::string, QDate> overdue_reported_;  // job id -> the day it was last reported overdue
};
