#include "tray.h"

#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

#include "app_settings.h"
#include "common.h"
#include "main_window.h"
#include "theme.h"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;

namespace {

constexpr std::uint64_t kTicksPerDay = 864000000000ull;  // FILETIME

QIcon tray_icon(bool attention) {
    const Theme& t = theme();
    return attention ? icon(QStringLiteral("shield_error_20_filled"), t.dark ? t.warning : QColor(0xC4, 0x2B, 0x1C))
                     : icon(QStringLiteral("shield_checkmark_20_filled"), t.accent);
}

}  // namespace

bool Tray::available() {
    return QSystemTrayIcon::isSystemTrayAvailable();
}

Tray::Tray(MainWindow* window, const jobs::JobStore& store) : QObject(window), window_(window), store_(store) {
    // Built once; only the list of jobs is refilled (clearing a menu would not delete submenus).
    menu_ = new QMenu;
    open_ = menu_->addAction(tr("Open Vaultik"), window_, &MainWindow::bring_to_front);
    run_menu_ = menu_->addMenu(tr("Back up now"));  // owned by menu_
    menu_->addSeparator();
    settings_ = menu_->addAction(tr("Settings..."), window_, &MainWindow::open_settings);
    menu_->addAction(tr("Quit Vaultik"), window_, &MainWindow::quit_app);
    icon_ = new QSystemTrayIcon(this);
    icon_->setContextMenu(menu_);
    connect(icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        const bool single = AppSettings::load().single_click_toggle;
        if (reason == QSystemTrayIcon::Trigger && single) window_->toggle_visible();
        else if (reason == QSystemTrayIcon::DoubleClick && !single) window_->bring_to_front();
    });
    connect(icon_, &QSystemTrayIcon::messageClicked, this, [this] { window_->bring_to_front(); });
    connect(&timer_, &QTimer::timeout, this, &Tray::check);
    timer_.start(60000);
    refresh_icons();
    icon_->show();
    check();
}

Tray::~Tray() {
    icon_->hide();
    delete menu_;
}

void Tray::refresh_icons() {
    icon_->setIcon(tray_icon(attention_));
    open_->setIcon(icon(QStringLiteral("window_20_regular")));
    run_menu_->setIcon(icon(QStringLiteral("play_20_regular")));
    settings_->setIcon(icon(QStringLiteral("settings_20_regular")));
}

void Tray::rebuild_menu() {
    run_menu_->clear();  // deletes the job actions it owns
    const std::vector<jobs::Job> list = store_.list();
    for (const jobs::Job& job : list) {
        const std::string id = job.id;
        run_menu_->addAction(qs(job.name), window_, [this, id] { window_->run_job_now(id); });
    }
    run_menu_->setEnabled(!list.empty());
}

void Tray::explain_hidden() {
    icon_->showMessage(tr("Vaultik is still running"),
                       tr("It keeps an eye on your backups from here. Use Quit in this icon's menu to end it."),
                       QSystemTrayIcon::Information, 8000);
}

void Tray::notify(const QString& title, const QString& text, bool warning) {
    icon_->showMessage(title, text, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 10000);
}

void Tray::check() {
    const AppSettings settings = AppSettings::load();
    const std::vector<jobs::Job> list = store_.list();
    const std::uint64_t now = vaultik::os::now();
    QStringList problems;
    for (const jobs::Job& job : list) {
        const auto records = jobs::combined_history(store_, job.id, 100);  // a week of hourly runs, so the last check is in it
        const QString name = qs(job.name);

        // Runs that ended badly since the last look (at the first look: only the latest run).
        std::uint64_t& seen = seen_[job.id];
        for (const jobs::RunRecord& r : records) {
            if (r.started <= seen) continue;
            const bool fresh = !first_check_ || &r == jobs::last_backup(records) || &r == jobs::last_check(records) ||
                               &r == jobs::last_copy(records);
            if (r.kind == jobs::RunKind::Copy) {
                if (fresh && settings.notify_failures && r.status == jobs::RunStatus::Failed)
                    icon_->showMessage(tr("Second copy of %1 failed").arg(name), qs(r.message).left(200),
                                       QSystemTrayIcon::Warning, 15000);
            } else if (r.kind != jobs::RunKind::Backup) {
                // A check that could not run is not news: the backup before it failed the same way.
                if (fresh && settings.notify_failures && jobs::found_damage(r))
                    icon_->showMessage(tr("Health check of %1 found problems").arg(name),
                                       tr("Some backed-up data could not be read back. Open Vaultik to see "
                                          "what is affected, and back up again soon."),
                                       QSystemTrayIcon::Critical, 15000);
            } else if (fresh && !r.alert.empty() && !r.on_connect) {  // (a connected-drive run says it itself)
                // Always shown: this is the one that must not be missed.
                icon_->showMessage(tr("%1: files may be encrypted by ransomware").arg(name),
                                   qs(r.alert).left(250), QSystemTrayIcon::Critical, 30000);
            } else if (fresh && settings.notify_failures && !r.drive.empty() && r.status != jobs::RunStatus::Failed) {
                // Saved, but the drive it is on says it is failing: the time to act is now.
                icon_->showMessage(tr("The backup drive of %1 reports a problem").arg(name),
                                   tr("%1. Copy the backup to another drive soon, before it fails.").arg(qs(r.drive)),
                                   QSystemTrayIcon::Warning, 30000);
            } else if (fresh && settings.notify_failures && r.status == jobs::RunStatus::Failed) {
                icon_->showMessage(tr("Backup failed: %1").arg(name), qs(r.message).left(200), QSystemTrayIcon::Critical,
                                   15000);
            } else if (fresh && settings.notify_failures && r.status == jobs::RunStatus::Partial && !r.on_connect) {
                // (a run for a connected drive reports when it ends, with what was skipped)
                icon_->showMessage(tr("Backup of %1 skipped files").arg(name),
                                   tr("%1 could not be read. Open Vaultik to see which.")
                                       .arg(count_of(r.errors, tr("file"), tr("files"))),
                                   QSystemTrayIcon::Warning, 15000);
            }
        }
        if (!records.empty()) seen = std::max(seen, records.back().started);
        const jobs::RunRecord* backup = jobs::last_backup(records);
        const jobs::RunRecord* checked = jobs::last_check(records);
        const jobs::RunRecord* copied = jobs::last_copy(records);
        if ((backup && (backup->status == jobs::RunStatus::Failed || !backup->alert.empty())) ||
            (checked && jobs::found_damage(*checked)) || (copied && copied->status == jobs::RunStatus::Failed))
            problems << name;

        // Too long without a saved version; reported once a day.
        const int days = overdue_after_days(job, settings.remind_after_days);
        const std::uint64_t saved = last_saved(store_, job);
        if (days > 0 && saved > 0 && now > saved && (now - saved) / kTicksPerDay >= static_cast<std::uint64_t>(days)) {
            problems << name;
            if (settings.notify_overdue && overdue_reported_[job.id] != QDate::currentDate()) {
                overdue_reported_[job.id] = QDate::currentDate();
                const auto ago = static_cast<int>((now - saved) / kTicksPerDay);
                icon_->showMessage(tr("%1 has not been backed up for %2").arg(name, count_of(ago, tr("day"), tr("days"))),
                                   job.schedule.kind == jobs::Schedule::Kind::Manual
                                       ? tr("Open Vaultik and choose Back up now.")
                                       : tr("Its scheduled backups are not running. Is the destination connected?"),
                                   QSystemTrayIcon::Warning, 15000);
            }
        }
    }
    first_check_ = false;
    problems.removeDuplicates();
    attention_ = !problems.isEmpty() && settings.tray_status;
    icon_->setIcon(tray_icon(attention_));
    icon_->setToolTip(!problems.isEmpty() ? tr("Vaultik: %1 need attention").arg(problems.join(", "))
                      : list.empty() ? tr("Vaultik: no backup jobs yet")
                                     : tr("Vaultik: all backups are fine"));
    rebuild_menu();
}
