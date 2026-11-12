#include "main_window.h"

#include <algorithm>
#include <set>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "cloud_dialog.h"
#include "updates.h"
#include "common.h"
#include "compare_dialog.h"
#include "computers_dialog.h"
#include "drive_watcher.h"
#include "event_runs.h"
#include "find_page.h"
#include "hello.h"
#include "job_card.h"
#include "job_dialog.h"
#include "key_dialog.h"
#include "overview.h"
#include "report_dialog.h"
#include "grew_dialog.h"
#include "restore_dialog.h"
#include "image_dialogs.h"
#include "app_settings.h"
#include "runner.h"
#include "settings_dialog.h"
#include "theme.h"
#include "tray.h"
#include "versions_view.h"
#include "vaultik/jobs/forecast.hpp"
#include "vaultik/jobs/admin_jobs.hpp"
#include "vaultik/jobs/alerts.hpp"
#include "vaultik/jobs/report.hpp"
#include "vaultik/platform/acl.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/backends.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/repo/lock.hpp"

using namespace vaultik;

namespace {

QString status_text(jobs::RunStatus s) {
    switch (s) {
        case jobs::RunStatus::Success: return QObject::tr("Completed");
        case jobs::RunStatus::Partial: return QObject::tr("Completed, some files skipped");
        case jobs::RunStatus::Failed: return QObject::tr("Failed");
        case jobs::RunStatus::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

// "today 14:02", "yesterday 02:00", "Mon 3 Nov 14:02".
QString when_text(std::uint64_t filetime) {
    const QDateTime dt = to_datetime(filetime);
    if (!dt.isValid()) return {};
    const QLocale locale;
    const QDate today = QDate::currentDate();
    const QString time = locale.toString(dt.time(), QLocale::ShortFormat);
    if (dt.date() == today) return QObject::tr("today %1").arg(time);
    if (dt.date() == today.addDays(-1)) return QObject::tr("yesterday %1").arg(time);
    return locale.toString(dt, QStringLiteral("ddd d MMM ")) + time;
}

// The schedule in words, in the frontend's language (jobs::describe is the engine's English).
QString schedule_text(const jobs::Schedule& s) {
    const QString time = QStringLiteral("%1:%2").arg(s.hour, 2, 10, QLatin1Char('0')).arg(s.minute, 2, 10, QLatin1Char('0'));
    switch (s.kind) {
        case jobs::Schedule::Kind::Manual:
            return QObject::tr("Manual");
        case jobs::Schedule::Kind::Hourly: {
            const QString minute = QStringLiteral(":%1").arg(s.minute, 2, 10, QLatin1Char('0'));
            return s.every_hours == 1 ? QObject::tr("Every hour at %1").arg(minute)
                                      : QObject::tr("Every %1 hours at %2").arg(s.every_hours).arg(minute);
        }
        case jobs::Schedule::Kind::Daily:
            return QObject::tr("Daily at %1").arg(time);
        case jobs::Schedule::Kind::Weekly: {
            if ((s.weekdays & 0x7F) == 0x7F) return QObject::tr("Every day at %1").arg(time);
            if ((s.weekdays & 0x7F) == 0x1F) return QObject::tr("Weekdays at %1").arg(time);
            const QString names[] = {QObject::tr("Mon"), QObject::tr("Tue"), QObject::tr("Wed"), QObject::tr("Thu"),
                                     QObject::tr("Fri"), QObject::tr("Sat"), QObject::tr("Sun")};
            QStringList days;
            for (int d = 0; d < 7; ++d)
                if (s.weekdays & (1u << d)) days << names[d];
            return QObject::tr("%1 at %2").arg(days.join(QStringLiteral(", ")), time);
        }
    }
    return QObject::tr("Manual");
}

// When Task Scheduler starts the job next (local time); invalid for a manual job. Mirrors the
// triggers in jobs/scheduler.cpp: an hourly job repeats from 2026-01-01 00:<minute>.
QDateTime next_run(const jobs::Schedule& s, const QDateTime& now) {
    switch (s.kind) {
        case jobs::Schedule::Kind::Manual:
            return {};
        case jobs::Schedule::Kind::Hourly: {
            const QDateTime start(QDate(2026, 1, 1), QTime(0, s.minute));
            const qint64 step = static_cast<qint64>(s.every_hours) * 3600;
            const qint64 since = start.secsTo(now);
            if (since < 0 || step <= 0) return start;
            return start.addSecs((since / step + 1) * step);
        }
        case jobs::Schedule::Kind::Daily: {
            const QDateTime t(now.date(), QTime(s.hour, s.minute));
            return t > now ? t : t.addDays(1);
        }
        case jobs::Schedule::Kind::Weekly:
            for (int i = 0; i <= 7; ++i) {
                const QDate day = now.date().addDays(i);
                if ((s.weekdays & (1u << (day.dayOfWeek() - 1))) == 0) continue;
                const QDateTime t(day, QTime(s.hour, s.minute));
                if (t > now) return t;
            }
            return {};
    }
    return {};
}

QString location_text(const std::string& repository) {
    const CloudKind kind = cloud_kind(repository);
    if (kind == CloudKind::None) return QDir::toNativeSeparators(qs(repository));
    if (kind == CloudKind::S3) {
        try {
            const S3Location l = parse_s3_location(repository);
            return QObject::tr("%1 (cloud)").arg(qs(l.bucket + (l.folder.empty() ? "" : "/" + l.folder)));
        } catch (const Error&) {
            return qs(repository);
        }
    }
    try {
        return qs(cloud_kind_name(kind)) + ": " + qs(cloud_folder(repository));
    } catch (const Error&) {
        return qs(repository);
    }
}

QFrame* card() {
    auto* f = new QFrame;
    f->setProperty("role", "card");
    return f;
}

}  // namespace

MainWindow::MainWindow(bool in_tray) : started_in_tray_(in_tray) {
    setWindowTitle(os::process_elevated() ? tr("Vaultik (Administrator)") : QStringLiteral("Vaultik"));
    setWindowIcon(app_icon());
    resize(1180, 760);
    setMinimumSize(900, 560);

    stack_ = new QStackedWidget;
    stack_->addWidget(build_empty_page());
    stack_->addWidget(build_job_page());
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("Content"));
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(28, 22, 28, 6);
    cl->addWidget(stack_);
    // Status messages live under the content, not in a window-wide status bar, so the sidebar
    // reaches the bottom of the window.
    status_ = new QStatusBar;
    status_->setSizeGripEnabled(false);
    cl->addWidget(status_);

    auto* central = new QWidget;
    auto* hl = new QHBoxLayout(central);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);
    hl->addWidget(build_sidebar());
    hl->addWidget(content, 1);
    setCentralWidget(central);

    runner_ = new Runner(this);
    connect(runner_, &Runner::progress, this, [this](const QJsonObject& ev) {
        const QString type = ev.value("event").toString();
        if (type == "check") {
            const bool damage = ev.value("errors").toInteger() > 0;
            const bool rot = ev.value("rot_files").toInteger() > 0;
            check_result_ = damage ? tr("The health check found problems: %1").arg(ev.value("message").toString())
                            : rot  ? tr("The health check found files on this PC that changed by themselves: %1")
                                        .arg(ev.value("message").toString())
                                   : tr("Health check: no problems.");
            log_->appendPlainText(check_result_);
            return;
        }
        if (type == "restore_test") {
            const bool bad = !ev.value("problems").toArray().isEmpty();
            check_result_ = bad ? tr("The test restore found problems: %1").arg(ev.value("message").toString())
                                : tr("Test restore: %1").arg(ev.value("message").toString());
            log_->appendPlainText(check_result_);
            return;
        }
        if (type == "repair_progress") {
            running_label_->setText(tr("Repairing the backup..."));
            const QString current = ev.value("current").toString();
            static const QRegularExpression packs(QStringLiteral("^pack (\\d+)/(\\d+)$"));
            const QRegularExpressionMatch m = packs.match(current);
            if (m.hasMatch()) {
                bar_->setRange(0, m.captured(2).toInt());
                bar_->setValue(m.captured(1).toInt());
                progress_text_->setText(tr("Checking stored data: part %1 of %2").arg(m.captured(1), m.captured(2)));
            } else {
                bar_->setRange(0, 0);
                progress_text_->setText(current == "writing the new index" ? tr("Writing the new index")
                                                                            : tr("Checking every version"));
            }
            current_file_->clear();
            return;
        }
        if (type == "test_progress") {
            running_label_->setText(tr("Testing a restore..."));
            bar_->setRange(0, 0);
            progress_text_->setText(tr("Restoring a few files into a temporary folder and checking them"));
            current_file_->setText(ev.value("current").toString());
            return;
        }
        if (type == "check_progress") {
            running_label_->setText(tr("Checking the backup..."));
            // "pack 3/120" while the stored data is read, then the versions' structure.
            const QString current = ev.value("current").toString();
            static const QRegularExpression packs(QStringLiteral("^pack (\\d+)/(\\d+)$"));
            const QRegularExpressionMatch m = packs.match(current);
            if (m.hasMatch()) {
                bar_->setRange(0, m.captured(2).toInt());
                bar_->setValue(m.captured(1).toInt());
                progress_text_->setText(tr("Reading stored data back: part %1 of %2").arg(m.captured(1), m.captured(2)));
                current_file_->clear();
            } else {
                bar_->setRange(0, 0);
                progress_text_->setText(tr("Checking every version's folders and files"));
                current_file_->setText(current);
            }
            return;
        }
        const double secs = std::max(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started_).count(), 0.001);
        const auto bytes = static_cast<std::uint64_t>(ev.value("bytes").toInteger());
        if (ev.value("image").toBool()) {
            // A disk image: how far through the disks it is.
            const double total = ev.value("total").toDouble();
            bar_->setRange(0, 1000);
            bar_->setValue(total > 0 ? static_cast<int>(ev.value("done").toDouble() * 1000 / total) : 0);
            progress_text_->setText(tr("Disk image: %1 read at %2/s, %3 new")
                                        .arg(format_bytes(bytes))
                                        .arg(format_bytes(static_cast<std::uint64_t>(static_cast<double>(bytes) / secs)))
                                        .arg(format_bytes(static_cast<std::uint64_t>(ev.value("new_bytes").toInteger()))));
            current_file_->setText(ev.value("current").toString());
            return;
        }
        progress_text_->setText(tr("%1 read at %2/s, %3 new  ·  %4")
                                    .arg(format_bytes(bytes))
                                    .arg(format_bytes(static_cast<std::uint64_t>(static_cast<double>(bytes) / secs)))
                                    .arg(format_bytes(static_cast<std::uint64_t>(ev.value("new_bytes").toInteger())))
                                    .arg(count_of(static_cast<std::uint64_t>(ev.value("files").toInteger()), tr("file"),
                                                  tr("files"))));
        current_file_->setText(ev.value("current").toString());
    });
    connect(runner_, &Runner::warning, this, [this](const QString& msg) { log_->appendPlainText(msg); });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        const std::string id = running_job_;
        running_job_.clear();
        const bool check = check_run_;
        const bool test = test_run_;
        const bool repair = repair_run_;
        check_run_ = false;
        test_run_ = false;
        repair_run_ = false;
        set_running(false);
        QString msg;
        if (repair) {
            msg = result == "success"   ? tr("Repair: %1").arg(ev.value("message").toString())
                  : result == "cancelled" ? tr("Repair cancelled; nothing was changed yet.")
                                          : tr("Repair failed: %1").arg(ev.value("message").toString());
            log_->appendPlainText(msg);
            status_->showMessage(msg, 30000);
            repos_.erase(id);  // the index and maybe the versions changed
            reload_jobs(current_job() ? current_job()->id : id);
            return;
        }
        if (test) {
            msg = result == "success"   ? tr("Test restore: %1").arg(ev.value("message").toString())
                  : result == "cancelled" ? tr("Test restore cancelled.")
                                          : tr("Test restore failed: %1").arg(ev.value("message").toString());
            log_->appendPlainText(msg);
            status_->showMessage(msg, 15000);
            reload_jobs(current_job() ? current_job()->id : id);
            return;
        }
        if (check) {
            msg = result == "success"   ? tr("Health check: no problems.")
                  : result == "cancelled" ? tr("Health check cancelled.")
                                          : tr("Health check failed: %1").arg(ev.value("message").toString());
            log_->appendPlainText(msg);
            status_->showMessage(msg, 15000);
            reload_jobs(current_job() ? current_job()->id : id);
            return;
        }
        if (result == "success" && ev.value("image").toBool()) {
            msg = tr("Disk image saved: %1 new, %2 stored")
                      .arg(format_bytes(static_cast<std::uint64_t>(ev.value("new_bytes").toInteger())))
                      .arg(format_bytes(static_cast<std::uint64_t>(ev.value("stored_bytes").toInteger())));
        } else if (result == "success" || result == "partial") {
            msg = tr("Backup finished: %1, %2 new, %3 stored")
                      .arg(count_of(static_cast<std::uint64_t>(ev.value("files").toInteger()), tr("file"), tr("files")))
                      .arg(format_bytes(static_cast<std::uint64_t>(ev.value("new_bytes").toInteger())))
                      .arg(format_bytes(static_cast<std::uint64_t>(ev.value("stored_bytes").toInteger())));
            if (result == "partial") msg += " " + tr("(%1 skipped, see Log)").arg(ev.value("errors").toInteger());
        } else if (result == "cancelled") {
            msg = tr("Backup cancelled; no version was saved.");
        } else {
            msg = tr("Backup failed: %1").arg(ev.value("message").toString());
            log_->appendPlainText(msg);
        }
        if (!check_result_.isEmpty()) msg += "  " + check_result_;
        check_result_.clear();
        const QString alert = ev.value("alert").toString();
        if (!alert.isEmpty()) {
            log_->appendPlainText(tr("WARNING: %1").arg(alert));
            // Plain text throughout: the alert names files, and a file name must never become markup.
            QMessageBox box(QMessageBox::Warning, tr("Files may be encrypted by ransomware"), QString(), QMessageBox::Ok,
                            this);
            box.setTextFormat(Qt::PlainText);
            box.setText(tr("The backup found many files that changed in a way ransomware changes them.") + "\n\n" +
                        alert + "\n\n" +
                        tr("If you did not change these files yourself, disconnect the backup drive, do not trust "
                           "the newest version, and restore from the pinned one. Old versions were not removed "
                           "this time."));
            box.exec();
        }
        status_->showMessage(msg, 15000);
        repos_.erase(id);  // a new version: reload the list when it is shown
        reload_jobs(current_job() ? current_job()->id : id);
    });

    connect(list_, &QListWidget::currentRowChanged, this, [this] { show_job(); });
    connect(ThemeNotifier::instance(), &ThemeNotifier::changed, this, &MainWindow::refresh_icons);

    // Backups that start when their drive is connected. Windows tells top-level windows about new
    // drives, so this one needs its native window even while it stays hidden in the tray.
    (void)winId();
    connect(new DriveWatcher(this), &DriveWatcher::connected, this, &MainWindow::drives_connected);
    // Continuous backups, and backups when the user signs out or shuts down.
    events_ = new EventRuns(this, this);
    connect(events_, &EventRuns::notify, this, [this](const QString& title, const QString& text, bool warning) {
        if (tray_ != nullptr) tray_->notify(title, text, warning);
        status_->showMessage(title + "  " + text, 15000);
    });
    connect(events_, &EventRuns::run_finished, this, [this] { refresh_sidebar(); });
    // Versions open in Explorer: a button to close them, and they close when Vaultik quits.
    mounts_button_ = new QPushButton;
    mounts_button_->setProperty("role", "flat");
    mounts_button_->setToolTip(tr("Close the versions shown as folders in File Explorer"));
    connect(mounts_button_, &QPushButton::clicked, this, &MainWindow::close_mounts);
    status_->addPermanentWidget(mounts_button_);
    mounts_button_->hide();
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::close_mounts);

    connect_timer_ = new QTimer(this);
    connect_timer_->setInterval(3000);
    connect(connect_timer_, &QTimer::timeout, this, &MainWindow::watch_connect_runs);

    // Scheduled runs happen outside this window; pick up their results.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        // Not under a dialog: the code that opened it may hold on to the jobs or versions.
        if (!runner_->running() && QApplication::activeModalWidget() == nullptr) refresh_sidebar();
        check_monthly();
    });
    timer->start(30000);

    // The daily update check: a few minutes after starting, then looked at every hour.
    const auto check_updates = [this] {
        if (!update_check_due()) return;
        check_for_update(this, [this](const UpdateResult& r) {
            const QString before = known_update().version;
            remember_update_check(r);
            if (!r.ok || !r.newer || r.version == before) return;  // tell about each version once
            const QString title = tr("Vaultik %1 is available").arg(r.version);
            const QString text = tr("Settings shows where to get it.");
            if (tray_ != nullptr) tray_->notify(title, text);
            status_->showMessage(title + "  " + text, 60000);
        });
    };
    QTimer::singleShot(3 * 60 * 1000, this, check_updates);
    auto* update_timer = new QTimer(this);
    connect(update_timer, &QTimer::timeout, this, check_updates);
    update_timer->start(60 * 60 * 1000);

    refresh_icons();
    reload_jobs();
    update_tray();
}

void MainWindow::update_tray() {
    const AppSettings settings = AppSettings::load();
    // Started hidden, the icon is the only way back to the window, so it is shown regardless.
    const bool want = (settings.tray_icon || (started_in_tray_ && !isVisible())) && Tray::available();
    if (want && tray_ == nullptr) {
        tray_ = new Tray(this, store_);
    } else if (!want && tray_ != nullptr) {
        delete tray_;
        tray_ = nullptr;
    } else if (tray_ != nullptr) {
        tray_->check();  // the icon may change colour with the settings
    }
    // Closing the window keeps Vaultik running only when it has a tray icon to come back from.
    QApplication::setQuitOnLastWindowClosed(!(tray_ != nullptr && settings.close_to_tray));
}

void MainWindow::bring_to_front() {
    if (isMinimized()) showNormal();
    show();
    raise();
    activateWindow();
}

void MainWindow::toggle_visible() {
    if (isVisible() && !isMinimized()) {
        hide();
        relock();
    } else {
        bring_to_front();
    }
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange || !isMinimized() || tray_ == nullptr) return;
    if (AppSettings::load().minimize_to_tray) {
        // Hide once the minimize has finished, so the window comes back in its normal state.
        QTimer::singleShot(0, this, [this] {
            hide();
            relock();
            setWindowState(windowState() & ~Qt::WindowMinimized);
        });
    }
}

void MainWindow::drives_connected(const QStringList& roots) {
    const std::vector<jobs::Job> list = store_.list();  // relocated to the drives' current letters
    for (const QString& root : roots) {
        const os::VolumeLocation loc = os::volume_of(u8(root));
        if (loc.volume.empty()) continue;
        for (const jobs::Job& job : list) {
            if (job.repository_volume != loc.volume || !jobs::connect_backup_due(store_, job)) continue;
            const bool busy = std::any_of(connect_runs_.begin(), connect_runs_.end(),
                                          [&](const ConnectRun& r) { return r.job == job.id; }) ||
                              running_job_ == job.id;
            if (busy) continue;
            // A separate, windowless engine process, so it goes on if this window is closed.
            QString program = Runner::engine_path();
#ifdef Q_OS_WIN
            program = QFileInfo(program).dir().filePath(QStringLiteral("vaultik-task.exe"));
#endif
            qint64 pid = 0;
            const std::uint64_t started = os::now();
            if (!Runner::start_detached(program, {QStringLiteral("run-job"), qs(job.id), QStringLiteral("--on-connect")},
                                        &pid))
                continue;
            connect_runs_.push_back({job.id, static_cast<std::uint32_t>(pid), root, started});
            const QString title = tr("Backing up %1").arg(qs(job.name));
            const QString text = tr("%1 was connected. Leave it plugged in until Vaultik says it is done.").arg(root);
            if (tray_ != nullptr) tray_->notify(title, text);
            status_->showMessage(title + "  " + text, 15000);
        }
    }
    if (!connect_runs_.empty() && !connect_timer_->isActive()) connect_timer_->start();
}

void MainWindow::watch_connect_runs() {
    for (auto it = connect_runs_.begin(); it != connect_runs_.end();) {
        if (os::process_alive(it->pid)) {
            ++it;
            continue;
        }
        const ConnectRun run = *it;
        it = connect_runs_.erase(it);
        const auto job = store_.find(run.job);
        if (!job) continue;
        // The backup this process saved (its health check, if one was due, ran before it ended).
        const auto history = jobs::combined_history(store_, run.job, 20);
        const jobs::RunRecord* r = nullptr;
        for (auto h = history.rbegin(); h != history.rend() && r == nullptr; ++h)
            if (h->kind == jobs::RunKind::Backup && h->on_connect && h->started >= run.started) r = &*h;
        QString title, text;
        bool warn = false;
        if (r == nullptr) {
            continue;  // it found nothing to do (already backing up, or just backed up)
        } else if (!r->alert.empty()) {
            title = tr("%1: files may be encrypted by ransomware").arg(qs(job->name));
            text = tr("Unplug %1 now and open Vaultik. %2").arg(run.drive, qs(r->alert).left(200));
            warn = true;
        } else if (r->status == jobs::RunStatus::Success) {
            title = tr("%1 is backed up").arg(qs(job->name));
            text = tr("You can unplug %1 now.").arg(run.drive);
        } else if (r->status == jobs::RunStatus::Partial) {
            title = tr("%1 is backed up, %2 skipped").arg(qs(job->name), count_of(r->errors, tr("file"), tr("files")));
            text = tr("You can unplug %1 now. Open Vaultik to see what could not be read.").arg(run.drive);
            warn = true;
        } else if (r->status == jobs::RunStatus::Failed) {
            title = tr("Backup failed: %1").arg(qs(job->name));
            text = qs(r->message).left(200);
            warn = true;
        } else {
            title = tr("Backup of %1 stopped").arg(qs(job->name));
            text = tr("No new version was saved. You can unplug %1.").arg(run.drive);
        }
        if (tray_ != nullptr) tray_->notify(title, text, warn);
        status_->showMessage(title + "  " + text, 30000);
        refresh_sidebar();
    }
    if (connect_runs_.empty()) connect_timer_->stop();
}

void MainWindow::compare_versions(int a, int b) {
    const jobs::Job* job = current_job();
    if (!job) return;
    const auto it = repos_.find(job->id);
    if (it == repos_.end()) return;
    const auto& snaps = it->second.snapshots;
    if (a < 0 || b < 0 || a >= static_cast<int>(snaps.size()) || b >= static_cast<int>(snaps.size()) || a == b) return;
    // Copies: the open backups may change (a finished run, a relock) while the work goes on.
    const SnapshotEntry x = snaps[static_cast<std::size_t>(a)];
    const SnapshotEntry y = snaps[static_cast<std::size_t>(b)];
    const SnapshotEntry older = x.snapshot.time <= y.snapshot.time ? x : y;
    const SnapshotEntry newer = x.snapshot.time <= y.snapshot.time ? y : x;
    const std::shared_ptr<Repository> repo = it->second.repo;
    DiffResult diff;
    const QString error = run_busy(this, tr("Comparing the versions..."), [&](const std::function<bool()>& cancelled) {
        diff = diff_snapshots(*repo, older.snapshot, newer.snapshot, 100000, cancelled);
    });
    if (!error.isEmpty()) {
        if (error != tr("Cancelled.")) plain_warning(this, tr("Compare versions"), error);
        return;
    }
    CompareDialog dialog(repo, older, newer, std::move(diff), this);
    dialog.exec();
}

bool MainWindow::locked() const {
    return !unlocked_ && AppSettings::load().require_hello;
}

bool MainWindow::unlock() {
    if (!locked()) return true;
    bring_to_front();
    switch (hello::verify(this, tr("Confirm it is you to open Vaultik's backups"))) {
        case hello::Result::Verified:
            unlocked_ = true;
            return true;
        case hello::Result::Unavailable:
            plain_warning(this, tr("Windows Hello"),
                          tr("Windows Hello is not available now, so the backups stay locked. Set it up again in "
                             "Windows Settings, or turn off \"Ask for Windows Hello\" in Vaultik's Settings."));
            return false;
        default:
            return false;
    }
}

void MainWindow::relock() {
    // Whatever unlocked_ says: backups may have been opened before the option was turned on.
    if (!AppSettings::load().require_hello) return;
    unlocked_ = false;
    ++lock_generation_;  // a backup still being opened is dropped when it arrives
    close_mounts();
    repos_.clear();  // the opened repositories (and their keys) go
    find_->set_source(nullptr, {});
    versions_->set_message(tr("Vaultik is locked. Open Versions or Find a file to unlock it with Windows Hello."));
    refresh_overview();
}

bool MainWindow::ensure_password(const jobs::Job& job) {
    try {
        std::string saved = store_.password(job.id);
        const bool have = !saved.empty();
        secure_wipe(saved.data(), saved.size());
        if (have) return true;
    } catch (const Error&) {
    }
    for (;;) {
        bool ok = false;
        QString pw = QInputDialog::getText(
            this, tr("Password for %1").arg(qs(job.name)),
            tr("Enter the password of this backup. Vaultik saves it for your account on this computer."),
            QLineEdit::Password, QString(), &ok);
        if (!ok || pw.isEmpty()) return false;
        std::string bytes = u8(pw);
        pw.fill(QChar(0));
        QString error;
        try {
            // Check it against the repository before saving it.
            const CloudCredentials cloud = store_.cloud_credentials(job);
            error = run_busy(this, tr("Checking the password..."), [&](const std::function<bool()>&) {
                Repository::open(open_backend(job.repository, &cloud), bytes);
            });
            if (error.isEmpty()) store_.set_password(job.id, bytes);
        } catch (const Error& e) {
            error = QString::fromUtf8(e.what());
        }
        secure_wipe(bytes.data(), bytes.size());
        if (error.isEmpty()) return true;
        plain_warning(this, tr("Password for %1").arg(qs(job.name)), error);
    }
}

void MainWindow::explore_version(int index) {
    const jobs::Job* current = current_job();
    if (!current) return;
    const jobs::Job copy = *current;  // ensure_password may show a dialog
    const jobs::Job* job = &copy;
    if (!ensure_password(copy)) return;
    const auto it = repos_.find(job->id);
    if (it == repos_.end() || index < 0 || index >= static_cast<int>(it->second.snapshots.size())) return;
    const std::string snapshot = id_hex(it->second.snapshots[static_cast<std::size_t>(index)].id);
    // Already open: show it again.
    for (const Mount& m : mounts_) {
        if (m.job == job->id && m.snapshot == snapshot) {
            if (!m.folder.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m.folder));
            return;
        }
    }
    auto* process = new QProcess(this);
    mounts_.push_back({process, job->id, snapshot, QString()});
    update_mounts_button();
    status_->showMessage(tr("Opening the version in File Explorer..."), 10000);

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
        while (process->canReadLine()) {
            const QJsonObject ev = QJsonDocument::fromJson(process->readLine()).object();
            if (ev.value("event").toString() != "mounted") continue;
            for (Mount& m : mounts_) {
                if (m.process != process) continue;
                m.folder = ev.value("folder").toString();
                // A job with one source opens right at it (the version keeps the full path,
                // "D:\Pictures" as "D\Pictures"); otherwise at the top.
                QString open = m.folder;
                for (const jobs::Job& j : jobs_) {
                    if (j.id != m.job || j.sources.size() != 1) continue;
                    QString inside = m.folder;
                    for (const std::string& part : os::path_components(os::absolute_path(j.sources.front())))
                        inside += QDir::separator() + qs(part);
                    if (QFileInfo(inside).isDir()) open = inside;
                }
                QDesktopServices::openUrl(QUrl::fromLocalFile(open));
            }
            status_->clearMessage();
            update_mounts_button();
        }
    });
    connect(process, &QProcess::finished, this, [this, process] {
        const auto m = std::find_if(mounts_.begin(), mounts_.end(), [process](const Mount& x) { return x.process == process; });
        const bool was_open = m != mounts_.end() && !m->folder.isEmpty();
        if (m != mounts_.end()) mounts_.erase(m);
        update_mounts_button();
        const QString error = QString::fromUtf8(process->readAllStandardError()).trimmed().remove(QStringLiteral("error: "));
        process->deleteLater();
        if (was_open || error.isEmpty()) return;
        if (error.contains(QStringLiteral("Projected File System"))) {
            // An optional Windows feature; turning it on needs administrator approval.
            const auto answer = QMessageBox::question(
                this, tr("Open in Explorer"),
                tr("Showing a version as a folder uses the Windows Projected File System, which is turned off on "
                   "this computer. Turn it on now? Windows asks for administrator approval."));
            if (answer != QMessageBox::Yes) return;
            try {
                os::run_command_elevated({"powershell.exe", "-NoProfile", "-Command",
                                          "Enable-WindowsOptionalFeature -Online -FeatureName Client-ProjFS -NoRestart"});
                QMessageBox::information(this, tr("Open in Explorer"),
                                         tr("When Windows has finished, choose Open in Explorer again. If it still "
                                            "does not work, restart the computer."));
            } catch (const Error& e) {
                plain_warning(this, tr("Open in Explorer"), QString::fromUtf8(e.what()));
            }
            return;
        }
        plain_warning(this, tr("Open in Explorer"), error);
    });
    QString program = Runner::engine_path();
    process->setProcessEnvironment(Runner::engine_environment());
    process->start(program, {QStringLiteral("mount"), QStringLiteral("--job"), qs(job->id), qs(snapshot),
                             QStringLiteral("--json")});
}

void MainWindow::close_mounts() {
    // Each one removes its folder; give them a moment, so nothing is left behind.
    // A copy of the list: each one that ends removes itself from mounts_ meanwhile.
    std::vector<QPointer<QProcess>> processes;
    for (const Mount& m : mounts_) processes.emplace_back(m.process);
    for (const auto& p : processes)
        if (p) p->write("cancel\n");
    for (const auto& p : processes)
        if (p) p->waitForFinished(5000);
}

void MainWindow::update_mounts_button() {
    int open = 0;
    for (const Mount& m : mounts_)
        if (!m.folder.isEmpty()) ++open;
    mounts_button_->setVisible(open > 0);
    mounts_button_->setText(open == 1 ? tr("1 version open in Explorer: Close")
                                      : tr("%1 versions open in Explorer: Close").arg(open));
}

void MainWindow::show_history_for(const QString& local_path) {
    bring_to_front();
    const std::string path = os::absolute_path(u8(QDir::toNativeSeparators(local_path)));
    const std::string folded = os::fold_case(path);
    // The job whose source holds the path (the longest source wins when several do).
    int best = -1;
    std::size_t best_len = 0;
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        for (const std::string& s : jobs_[i].sources) {
            const std::string src = os::fold_case(os::absolute_path(s));
            const bool inside = folded == src || (folded.starts_with(src) && (src.ends_with(std::string(1, os::kSeparator)) ||
                                                                              folded[src.size()] == os::kSeparator));
            if (inside && src.size() > best_len) {
                best = static_cast<int>(i);
                best_len = src.size();
            }
        }
    }
    if (best < 0) {
        plain_warning(this, tr("Earlier versions"),
                      tr("No backup job here includes %1, so Vaultik has no earlier versions of it.")
                          .arg(QDir::toNativeSeparators(local_path)));
        return;
    }
    list_->setCurrentRow(best);
    // The path as the backup stores it: "D:\Pictures\a.jpg" is "D/Pictures/a.jpg".
    QStringList parts;
    for (const std::string& c : os::path_components(path)) parts << qs(c);
    tabs_->setCurrentWidget(find_);
    // Loading first: giving the Find page its versions clears what it shows. When the backup
    // still has to be opened, the page keeps the path until it is.
    load_snapshots(false);
    find_->show_path(parts.join(QLatin1Char('/')));
}

void MainWindow::run_job_now(const std::string& id) {
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].id != id) continue;
        list_->setCurrentRow(static_cast<int>(i));
        bring_to_front();
        if (!runner_->running()) run_job();
        return;
    }
}

void MainWindow::open_settings() {
    bring_to_front();
    const bool hello_before = AppSettings::load().require_hello;
    SettingsDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    // Turning Windows Hello on asked for it just now, so this window counts as unlocked.
    if (!hello_before && AppSettings::load().require_hello) unlocked_ = true;
    update_tray();
    if (tray_ != nullptr) tray_->check();
}

void MainWindow::open_report(const std::string& month) {
    bring_to_front();
    ReportDialog dialog(store_, month, this);
    dialog.exec();
}

void MainWindow::check_monthly() {
    const std::string month = jobs::previous_month(jobs::month_of(os::now()));
    if (month == monthly_done_ || tray_ == nullptr) return;  // the notice needs the tray icon
    monthly_done_ = month;
    try {
        if (!jobs::report_claimed(store_.dir(), "notice", month)) {
            const jobs::MonthlyReport report = jobs::report_for(store_, month);
            // Of several Vaultik windows (other sessions of the account), one tells.
            if (report.any_runs() && jobs::claim_report(store_.dir(), "notice", month)) {
                const QString text =
                    report.attention.empty()
                        ? tr("All backups are fine. Click to see what they did last month.")
                        : tr("Things that need attention: %1. Click to see the report.")
                              .arg(report.attention.size());
                tray_->notify(tr("Your monthly backup report"), text, !report.attention.empty(),
                              [this, month] { open_report(month); });
            }
        }
        // The summary to the phone: sent by the command line, which waits for the network.
        const jobs::AlertSettings alerts = jobs::load_alerts(store_.dir());
        if (alerts.monthly_summary && alerts.any() && !jobs::report_claimed(store_.dir(), "alert", month))
            Runner::start_detached(Runner::engine_path(), {QStringLiteral("report"), QStringLiteral("--deliver")});
    } catch (const std::exception&) {
        // the report is a help; try again after the next start
    }
}

void MainWindow::quit_app() {
    quitting_ = true;
    if (close()) {
        QApplication::quit();
    } else {
        quitting_ = false;  // a running backup was kept
    }
}

QWidget* MainWindow::build_sidebar() {
    auto* side = new QWidget;
    side->setObjectName(QStringLiteral("Sidebar"));
    side->setFixedWidth(290);
    auto* l = new QVBoxLayout(side);
    l->setContentsMargins(14, 18, 14, 14);
    l->setSpacing(6);

    auto* brand = new QLabel(QStringLiteral("Vaultik"));
    brand->setProperty("role", "brand");
    brand->setContentsMargins(6, 0, 0, 6);
    l->addWidget(brand);

    auto* section = new QLabel(tr("Backup jobs"));
    section->setProperty("role", "section");
    section->setContentsMargins(6, 6, 0, 2);
    l->addWidget(section);

    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("JobList"));
    list_->setItemDelegate(new JobCardDelegate(list_));  // owned by the list
    list_->setMouseTracking(true);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    l->addWidget(list_, 1);

    new_job_ = new QPushButton(tr("New backup job"));
    new_job_->setProperty("role", "primary");
    connect(new_job_, &QPushButton::clicked, this, &MainWindow::new_job);
    l->addWidget(new_job_);
    l->addSpacing(6);

    open_backup_ = new QPushButton(tr("Restore from another backup..."));
    open_backup_->setProperty("role", "flat");
    open_backup_->setToolTip(tr("Open any Vaultik backup with its password, e.g. one made on another computer"));
    connect(open_backup_, &QPushButton::clicked, this, &MainWindow::restore_from_folder);
    l->addWidget(open_backup_);

    keys_ = new QPushButton(tr("Password and recovery key..."));
    keys_->setProperty("role", "flat");
    keys_->setToolTip(tr("Change a backup's password, or set a new one with the recovery key"));
    connect(keys_, &QPushButton::clicked, this, &MainWindow::open_keys);
    l->addWidget(keys_);

    report_ = new QPushButton(tr("Monthly report..."));
    report_->setProperty("role", "flat");
    report_->setToolTip(tr("What every backup job did in a month, and what needs attention"));
    connect(report_, &QPushButton::clicked, this, [this] { open_report(); });
    l->addWidget(report_);
    computers_ = new QPushButton(tr("All computers..."));
    computers_->setProperty("role", "flat");
    computers_->setToolTip(tr("How the backups of every computer saving to the same places as your jobs are doing"));
    connect(computers_, &QPushButton::clicked, this, &MainWindow::show_computers);
    l->addWidget(computers_);

    settings_ = new QPushButton(tr("Settings..."));
    settings_->setProperty("role", "flat");
    settings_->setToolTip(tr("Notifications, and starting Vaultik in the notification area"));
    connect(settings_, &QPushButton::clicked, this, &MainWindow::open_settings);
    l->addWidget(settings_);
    return side;
}

QWidget* MainWindow::build_empty_page() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addStretch(2);
    empty_icon_ = new QLabel;
    empty_icon_->setAlignment(Qt::AlignCenter);
    layout->addWidget(empty_icon_);
    auto* heading = new QLabel(tr("Protect your files"));
    heading->setProperty("role", "title");
    heading->setAlignment(Qt::AlignCenter);
    layout->addWidget(heading);
    auto* text = new QLabel(tr("A backup job says what to back up, where to, and when. Backups are deduplicated, "
                               "compressed and encrypted, and you can go back to any version."));
    text->setProperty("role", "muted");
    text->setAlignment(Qt::AlignCenter);
    text->setWordWrap(true);
    text->setMaximumWidth(520);
    auto* text_row = new QHBoxLayout;
    text_row->addStretch();
    text_row->addWidget(text);
    text_row->addStretch();
    layout->addLayout(text_row);
    layout->addSpacing(12);

    empty_create_ = new QPushButton(tr("Create a backup job"));
    empty_create_->setProperty("role", "primary");
    connect(empty_create_, &QPushButton::clicked, this, &MainWindow::new_job);
    empty_restore_ = new QPushButton(tr("Restore from an existing backup..."));
    connect(empty_restore_, &QPushButton::clicked, this, &MainWindow::restore_from_folder);
    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(empty_create_);
    row->addWidget(empty_restore_);
    row->addStretch();
    layout->addLayout(row);
    layout->addStretch(3);
    return page;
}

QWidget* MainWindow::build_job_page() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    // Header: name, what goes where, the last result; actions on the right.
    title_ = new QLabel;
    title_->setProperty("role", "title");
    subtitle_ = new QLabel;
    subtitle_->setProperty("role", "muted");
    subtitle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_icon_ = new QLabel;
    status_text_ = new QLabel;
    status_text_->setProperty("role", "muted");
    auto* status_row = new QHBoxLayout;
    status_row->setSpacing(6);
    status_row->addWidget(status_icon_);
    status_row->addWidget(status_text_, 1);

    auto* names = new QVBoxLayout;
    names->setSpacing(2);
    names->addWidget(title_);
    names->addWidget(subtitle_);
    names->addSpacing(4);
    names->addLayout(status_row);

    restore_ = new QPushButton(tr("Restore..."));
    run_ = new QPushButton(tr("Back up now"));
    run_->setProperty("role", "primary");
    more_ = new QToolButton;
    more_->setToolTip(tr("More"));
    more_->setAccessibleName(tr("More actions"));
    more_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(more_);  // owned by the button
    menu->addAction(tr("Edit job..."), this, &MainWindow::edit_job);
    menu->addAction(tr("Check the backup now"), this, [this] { check_job(false); })
        ->setToolTip(tr("Read part of the stored data back and check every version"));
    menu->addAction(tr("Full health check..."), this, [this] { check_job(true); });
    menu->addAction(tr("Test a restore"), this, &MainWindow::test_restore)
        ->setToolTip(tr("Restore a few files into a temporary folder and check them"));
    menu->addAction(tr("Repair the backup..."), this, &MainWindow::repair_backup)
        ->setToolTip(tr("When a check found damage: rebuild the index and keep what is intact"));
    menu->setToolTipsVisible(true);
    menu->addAction(tr("Create recovery drive..."), this, [this] {
        RecoveryDriveDialog dialog(jobs_, this);
        dialog.exec();
    })->setToolTip(tr("A USB stick or ISO file that starts a computer and restores a disk image onto it"));
    menu->addAction(tr("Refresh"), this, [this] {
        repos_.clear();
        reload_jobs(current_job() ? current_job()->id : std::string());
    });
    menu->addSeparator();
    menu->addAction(tr("Delete job..."), this, &MainWindow::delete_job);
    more_->setMenu(menu);
    connect(run_, &QPushButton::clicked, this, &MainWindow::run_job);
    connect(restore_, &QPushButton::clicked, this, [this] { open_restore(0); });

    auto* header = new QHBoxLayout;
    header->addLayout(names, 1);
    header->addWidget(restore_, 0, Qt::AlignTop);
    header->addWidget(run_, 0, Qt::AlignTop);
    header->addWidget(more_, 0, Qt::AlignTop);
    layout->addLayout(header);

    // Progress of a running backup.
    QFrame* progress_card = card();
    progress_box_ = progress_card;
    auto* pl = new QVBoxLayout(progress_card);
    pl->setContentsMargins(14, 12, 14, 12);
    running_label_ = new QLabel(tr("Backing up..."));
    running_label_->setProperty("role", "heading");
    bar_ = new QProgressBar;
    bar_->setRange(0, 0);
    bar_->setTextVisible(false);
    progress_text_ = new QLabel;
    current_file_ = new QLabel;
    current_file_->setTextFormat(Qt::PlainText);
    current_file_->setProperty("role", "faint");
    current_file_->setMinimumWidth(10);
    // The heading with a Pause button on the right.
    pause_ = new QPushButton(tr("Pause"));
    pause_->setProperty("role", "flat");
    pause_->setToolTip(tr("Pause the backup; it goes on where it stopped when you resume it"));
    connect(pause_, &QPushButton::clicked, this, [this] {
        paused_ = !paused_;
        runner_->set_paused(paused_);
        pause_->setText(paused_ ? tr("Resume") : tr("Pause"));
        pause_->setIcon(icon(paused_ ? QStringLiteral("play_20_regular") : QStringLiteral("stop_20_regular")));
        running_label_->setText(paused_ ? tr("Paused") : check_run_ ? tr("Checking the backup...") : tr("Backing up..."));
        bar_->setEnabled(!paused_);
    });
    auto* head = new QHBoxLayout;
    head->addWidget(running_label_, 1);
    head->addWidget(pause_);
    pl->addLayout(head);
    pl->addWidget(bar_);
    pl->addWidget(progress_text_);
    pl->addWidget(current_file_);
    progress_box_->hide();
    layout->addWidget(progress_box_);

    tabs_ = new QTabWidget;
    tabs_->setDocumentMode(true);

    overview_ = new OverviewView;
    connect(overview_, &OverviewView::check_requested, this, [this] { check_job(false); });
    connect(overview_, &OverviewView::test_requested, this, &MainWindow::test_restore);
    tabs_->addTab(overview_, tr("Overview"));

    versions_ = new VersionsView;
    connect(overview_, &OverviewView::versions_requested, this, [this] { tabs_->setCurrentWidget(versions_); });
    connect(overview_, &OverviewView::grew_requested, this, &MainWindow::show_grew);
    connect(versions_, &VersionsView::restore_requested, this, [this](int i) { open_restore(i); });
    connect(versions_, &VersionsView::pin_requested, this, [this](int i, bool pin) { change_version(i, pin, false); });
    connect(versions_, &VersionsView::note_requested, this, [this](int i) { change_version(i, false, true); });
    connect(versions_, &VersionsView::delete_requested, this, &MainWindow::delete_version);
    connect(versions_, &VersionsView::explore_requested, this, &MainWindow::explore_version);
    connect(versions_, &VersionsView::compare_requested, this, &MainWindow::compare_versions);
    tabs_->addTab(versions_, tr("Versions"));

    find_ = new FindPage;
    connect(find_, &FindPage::restore_requested, this,
            [this](int i, const QString& path) { open_restore(i, path); });
    tabs_->addTab(find_, tr("Find a file"));

    history_ = new QTreeWidget;
    history_->setRootIsDecorated(false);
    history_->setIndentation(0);
    history_->setHeaderLabels({tr("Started"), tr("What"), tr("Result"), tr("Files"), tr("Size"), tr("New"),
                               tr("Stored"), tr("Duration"), tr("Details")});
    history_->header()->setStretchLastSection(true);
    auto* activity = new QWidget;
    auto* al = new QVBoxLayout(activity);
    al->setContentsMargins(0, 8, 0, 0);
    al->addWidget(history_);
    tabs_->addTab(activity, tr("Activity"));

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    auto* log_page = new QWidget;
    auto* ll = new QVBoxLayout(log_page);
    ll->setContentsMargins(0, 8, 0, 0);
    ll->addWidget(log_);
    tabs_->addTab(log_page, tr("Log"));

    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (i <= 2) load_snapshots(false);  // Overview, Versions, Find a file
    });
    layout->addWidget(tabs_, 1);
    return page;
}

void MainWindow::refresh_icons() {
    const Theme& t = theme();
    new_job_->setIcon(icon(QStringLiteral("add_20_regular"), t.accent_text));
    open_backup_->setIcon(icon(QStringLiteral("folder_open_20_regular")));
    keys_->setIcon(icon(QStringLiteral("key_20_regular")));
    computers_->setIcon(icon(QStringLiteral("window_20_regular")));
    settings_->setIcon(icon(QStringLiteral("settings_20_regular")));
    report_->setIcon(icon(QStringLiteral("document_20_regular")));
    if (tray_ != nullptr) tray_->refresh_icons();
    restore_->setIcon(icon(QStringLiteral("arrow_counterclockwise_20_regular")));
    more_->setIcon(icon(QStringLiteral("more_horizontal_20_regular")));
    empty_icon_->setPixmap(icon(QStringLiteral("shield_checkmark_20_regular"), t.accent).pixmap(64, 64));
    empty_create_->setIcon(icon(QStringLiteral("add_20_regular"), t.accent_text));
    empty_restore_->setIcon(icon(QStringLiteral("folder_open_20_regular")));
    versions_->refresh_icons();
    find_->refresh_icons();
    set_running(runner_ != nullptr && runner_->running());
    if (!jobs_.empty()) {
        refresh_history();
        list_->viewport()->update();
    }
}

void MainWindow::reload_jobs(const std::string& select_id) {
    jobs_ = store_.list();
    if (events_ != nullptr) events_->set_jobs(jobs_);
    const QSignalBlocker block(list_);
    list_->clear();
    int select = jobs_.empty() ? -1 : 0;
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        auto* item = new QListWidgetItem(list_);  // owned by the list
        fill_card(item, jobs_[i]);
        if (jobs_[i].id == select_id) select = static_cast<int>(i);
    }
    list_->setCurrentRow(select);
    show_job();
}

void MainWindow::fill_card(QListWidgetItem* item, const jobs::Job& j) {
    item->setText(qs(j.name));
    const auto h = jobs::combined_history(store_, j.id, 100);
    const jobs::RunRecord* backup = jobs::last_backup(h);
    const jobs::RunRecord* check = jobs::last_check(h);
    if (backup && !backup->alert.empty()) {
        item->setData(JobCardDelegate::StatusRole, static_cast<int>(jobs::RunStatus::Failed));
        item->setData(JobCardDelegate::StatusTextRole, tr("Files may be encrypted, %1").arg(when_text(backup->started)));
    } else if (check && jobs::found_damage(*check)) {
        item->setData(JobCardDelegate::StatusRole, static_cast<int>(jobs::RunStatus::Failed));
        item->setData(JobCardDelegate::StatusTextRole, tr("Health check found problems, %1").arg(when_text(check->started)));
    } else if (!backup) {
        item->setData(JobCardDelegate::StatusRole, -1);
        item->setData(JobCardDelegate::StatusTextRole, tr("Not backed up yet"));
    } else {
        item->setData(JobCardDelegate::StatusRole, static_cast<int>(backup->status));
        item->setData(JobCardDelegate::StatusTextRole, status_text(backup->status) + ", " + when_text(backup->started));
    }
    item->setData(JobCardDelegate::DetailRole, schedule_text(j.schedule) + "  ·  " + location_text(j.repository));
}

void MainWindow::refresh_sidebar() {
    // Runs made by the scheduler show up here without disturbing what is open on the right.
    std::vector<jobs::Job> now = store_.list();
    const bool same = now.size() == jobs_.size() &&
                      std::equal(now.begin(), now.end(), jobs_.begin(),
                                 [](const jobs::Job& a, const jobs::Job& b) { return a.id == b.id; });
    if (!same) {
        reload_jobs(current_job() ? current_job()->id : std::string());
        return;
    }
    for (std::size_t i = 0; i < now.size(); ++i) fill_card(list_->item(static_cast<int>(i)), now[i]);
    jobs_ = std::move(now);
    if (events_ != nullptr) events_->set_jobs(jobs_);
    refresh_history();
}

const jobs::Job* MainWindow::current_job() const {
    const int row = list_->currentRow();
    return row >= 0 && row < static_cast<int>(jobs_.size()) ? &jobs_[static_cast<std::size_t>(row)] : nullptr;
}

void MainWindow::show_job() {
    const jobs::Job* job = current_job();
    stack_->setCurrentIndex(job ? 1 : 0);
    if (!job) return;
    title_->setText(qs(job->name));
    QStringList sources;
    for (const auto& s : job->sources) sources << QDir::toNativeSeparators(qs(s));
    subtitle_->setText(tr("%1  to  %2").arg(sources.join(", "), location_text(job->repository)));
    subtitle_->setToolTip(qs(job->repository));
    set_running(runner_->running());
    refresh_history();
    if (tabs_->currentIndex() <= 2) load_snapshots(false);
}

void MainWindow::refresh_history() {
    const jobs::Job* job = current_job();
    if (!job) return;
    const auto records = jobs::combined_history(store_, job->id, 200);
    history_->clear();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const jobs::RunRecord& r = *it;
        const bool backup = r.kind == jobs::RunKind::Backup;
        auto* item = new QTreeWidgetItem(history_);
        item->setText(0, format_time(r.started));
        item->setText(1, backup                                 ? tr("Backup")
                         : r.kind == jobs::RunKind::FullCheck ? tr("Full check")
                         : r.kind == jobs::RunKind::Copy      ? tr("Second copy")
                         : r.kind == jobs::RunKind::RestoreTest ? tr("Test restore")
                                                              : tr("Health check"));
        item->setIcon(2, status_icon(r.status));
        if (backup) {
            item->setText(2, r.alert.empty() ? status_text(r.status) : tr("Warning: files may be encrypted"));
            if (!r.alert.empty()) item->setIcon(2, status_icon(jobs::RunStatus::Failed));
            item->setText(3, QLocale().toString(static_cast<qulonglong>(r.files)));
            item->setText(4, format_bytes(r.bytes));
            item->setText(5, format_bytes(r.new_bytes));
            item->setText(6, format_bytes(r.stored_bytes));
        } else {
            // For a check: how much was read back.
            item->setText(2, jobs::found_damage(r) ? tr("Problems found")
                             : r.rot_files > 0     ? tr("Files on this PC changed by themselves")
                                                   : status_text(r.status));
            if (r.rot_files > 0 && !jobs::found_damage(r)) item->setIcon(2, status_icon(jobs::RunStatus::Partial));
            item->setText(4, format_bytes(r.bytes));
            item->setToolTip(4, tr("Read back and checked"));
        }
        if (r.finished > r.started) item->setText(7, format_duration(static_cast<double>(r.finished - r.started) / 1e7));
        item->setText(8, qs(r.alert.empty() ? r.message : r.alert + (r.message.empty() ? "" : "; " + r.message)));
        for (int c = 3; c < 8; ++c) item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    }
    for (int c = 0; c < 8; ++c) history_->resizeColumnToContents(c);

    QString next = schedule_text(job->schedule);
    const jobs::RunRecord* backup = jobs::last_backup(records);
    const jobs::RunRecord* check = jobs::last_check(records);
    QString text;
    if (!backup) {
        status_icon_->clear();
        text = tr("Not backed up yet");
    } else {
        status_icon_->setPixmap(status_icon(backup->status).pixmap(16, 16));
        text = tr("Last backup %1: %2").arg(when_text(backup->started), status_text(backup->status).toLower());
    }
    if (check) {
        const bool damage = jobs::found_damage(*check);
        if (damage || check->rot_files > 0) status_icon_->setPixmap(status_icon(jobs::RunStatus::Failed).pixmap(16, 16));
        text += "  ·  " + tr("Checked %1: %2")
                              .arg(when_text(check->started),
                                   damage                                        ? tr("problems found")
                                   : check->rot_files > 0                        ? tr("files on this PC changed by themselves")
                                   : check->status == jobs::RunStatus::Success ? tr("no problems")
                                                                                 : status_text(check->status).toLower());
    }
    status_text_->setText(text + "  ·  " + tr("Schedule: %1").arg(next));
    refresh_overview();
}

void MainWindow::load_snapshots(bool force) {
    const jobs::Job* job = current_job();
    if (!job) return;
    if (locked()) {
        // The start page does not ask; Versions and Find a file do, as they show the backup.
        if (tabs_->currentWidget() == overview_ || !unlock()) {
            versions_->set_message(tr("Vaultik is locked. Open Versions or Find a file to unlock it with Windows Hello."));
            return;
        }
        job = current_job();  // the prompt ran the event loop
        if (!job) return;
    }
    const std::string id = job->id;
    if (!force && repos_.contains(id)) {
        show_versions();
        return;
    }
    if (loading_job_ == id) return;
    if (!ensure_password(*job)) {
        versions_->set_message(tr("This backup needs its password. Choose Refresh in the ... menu to enter it."));
        return;
    }
    job = current_job();  // the password dialog ran the event loop
    if (!job || job->id != id) return;
    loading_job_ = id;
    versions_->set_message(tr("Opening the backup..."));
    find_->set_source(nullptr, {});
    std::string pw;
    CloudCredentials cloud;
    try {
        pw = store_.password(id);
        cloud = store_.cloud_credentials(*job);
    } catch (const Error& e) {
        versions_->set_message(QString::fromUtf8(e.what()));
        loading_job_.clear();
        return;
    }
    RepoHandle::open_async(this, job->repository, std::move(pw), std::move(cloud),
                           [this, id, generation = lock_generation_](std::shared_ptr<Repository> repo,
                                                                     std::vector<SnapshotEntry> list, QString error) {
                               loading_job_.clear();
                               // Locked again while it opened: it is not kept.
                               if (generation != lock_generation_) return;
                               const jobs::Job* cur = current_job();
                               if (!error.isEmpty()) {
                                   if (cur && cur->id == id) versions_->set_message(error);
                                   return;
                               }
                               // This job's versions; a repository without job tags shows them all.
                               std::vector<SnapshotEntry> mine;
                               const std::string tag = "job:" + id;
                               for (const SnapshotEntry& e : list)
                                   if (std::find(e.snapshot.tags.begin(), e.snapshot.tags.end(), tag) != e.snapshot.tags.end())
                                       mine.push_back(e);
                               if (mine.empty()) mine = std::move(list);
                               repos_[id] = OpenRepo{std::move(repo), std::move(mine)};
                               if (cur && cur->id == id) show_versions();
                           });
}

void MainWindow::show_versions() {
    const jobs::Job* job = current_job();
    if (!job) return;
    auto it = repos_.find(job->id);
    if (it == repos_.end()) return;
    const OpenRepo& r = it->second;

    // What the job's cleanup will remove next time (a size limit can remove more).
    std::set<Id> leaving;
    if (!job->retention.no_rules()) {
        for (const ForgetDecision& d : plan_forget(records_of(r.snapshots), job->retention))
            if (!d.keep) leaving.insert(d.entry.id);
    }
    versions_->set_versions(r.snapshots, leaving, retention_summary(job->retention));
    find_->set_source(r.repo, r.snapshots);
    refresh_overview();
}

// How a drive got worse lately, in words: "3 more reallocated sectors since 02.10.2026".
static QString change_text(const jobs::DriveChange& c) {
    const QString since = QLocale().toString(to_datetime(c.since).date(), QLocale::ShortFormat);
    const qulonglong more = c.to - c.from;
    switch (c.what) {
        case jobs::DriveChange::What::Health:
            return QObject::tr("health fell from %1% to %2% since %3").arg(c.from).arg(c.to).arg(since);
        case jobs::DriveChange::What::Reallocated:
            return QObject::tr("%1 more reallocated sectors since %2").arg(more).arg(since);
        case jobs::DriveChange::What::Pending:
            return QObject::tr("%1 more pending sectors since %2").arg(more).arg(since);
        case jobs::DriveChange::What::Uncorrectable:
            return QObject::tr("%1 more uncorrectable sectors since %2").arg(more).arg(since);
        case jobs::DriveChange::What::ReadErrors:
            return QObject::tr("%1 more unrecoverable read errors since %2").arg(more).arg(since);
    }
    return {};
}

const std::vector<MainWindow::JobDrive>& MainWindow::drives_of(const jobs::Job& job) {
    DriveCache& cache = drive_cache_[job.id];
    const auto now = std::chrono::steady_clock::now();
    if (cache.when != std::chrono::steady_clock::time_point{} && now - cache.when < std::chrono::minutes(10))
        return cache.drives;
    cache.when = now;
    cache.drives.clear();
    const auto add = [&](DriveRole role, const std::string& path) {
        if (path.empty() || is_cloud_location(path)) return;
        for (os::DriveHealth& h : os::drive_health(path)) {
            const bool seen = std::any_of(cache.drives.begin(), cache.drives.end(), [&](const JobDrive& d) {
                return d.role == role && d.health.disk == h.disk;
            });
            if (!seen) cache.drives.push_back({role, std::move(h), {}});
        }
    };
    add(DriveRole::Backup, job.repository);
    add(DriveRole::Copy, job.second_copy.repository);
    for (const std::string& s : job.sources) add(DriveRole::Source, s);
    // Keep a record of each drive's health, and compare with it.
    std::vector<os::DriveHealth> looked;
    for (const JobDrive& jd : cache.drives) looked.push_back(jd.health);
    jobs::record_drives(looked);
    for (JobDrive& jd : cache.drives) {
        const jobs::DriveReading reading = jobs::reading_of(jd.health, os::now());
        jd.worse = jobs::drive_trend(jobs::drive_history(reading.key), reading);
    }
    return cache.drives;
}

void MainWindow::refresh_overview() {
    const jobs::Job* job = current_job();
    if (!job) return;
    const auto records = jobs::combined_history(store_, job->id, 500);
    const jobs::RunRecord* backup = jobs::last_backup(records);
    const jobs::RunRecord* check = jobs::last_check(records);
    OverviewData d;

    // Last backup.
    if (backup) {
        d.has_backup = true;
        d.backup_status = backup->status;
        d.backup_when = when_text(backup->started);
        d.backup_when[0] = d.backup_when[0].toUpper();
        if (backup->status == jobs::RunStatus::Failed || backup->status == jobs::RunStatus::Cancelled)
            d.backup_detail = status_text(backup->status) + (backup->message.empty() ? QString() : ": " + qs(backup->message));
        else if (backup->snapshot.empty())  // "only when something changed", and nothing had
            d.backup_detail = tr("Nothing changed, so no new version was needed");
        else
            d.backup_detail = tr("%1, %2 new").arg(count_of(backup->files, tr("file"), tr("files")),
                                                   format_bytes(backup->new_bytes)) +
                              (backup->status == jobs::RunStatus::Partial
                                   ? "\n" + tr("%1 could not be read").arg(count_of(backup->errors, tr("item"), tr("items")))
                                   : QString());
    }

    // Next backup.
    const QDateTime next = next_run(job->schedule, QDateTime::currentDateTime());
    if (next.isValid()) {
        d.next_run = when_text(from_datetime(next));
        if (next.date() == QDate::currentDate().addDays(1))
            d.next_run = tr("tomorrow %1").arg(QLocale().toString(next.time(), QLocale::ShortFormat));
        d.next_run[0] = d.next_run[0].toUpper();
        d.next_detail = schedule_text(job->schedule) + ", " + tr("if the computer is on (otherwise right after)");
    } else {
        d.next_run = tr("When you start it");
        d.next_detail = tr("This job has no schedule.");
    }
    if (job->backup_on_connect) d.next_detail += "\n" + tr("Also when its drive is connected.");
    if (job->schedule.on_idle) d.next_detail += "\n" + tr("Also when the computer is idle.");
    if (job->schedule.on_lock) d.next_detail += "\n" + tr("Also when you lock the computer.");
    if (job->schedule.on_sign_out) d.next_detail += "\n" + tr("Also when you sign out or shut down.");
    if (job->schedule.on_update) d.next_detail += "\n" + tr("Also when Windows starts installing updates.");
    if (job->continuous)
        d.next_detail += "\n" + tr("Also %1 min after files change.").arg(job->continuous_minutes);

    // Versions and size, once the repository is open.
    const auto it = repos_.find(job->id);
    if (it != repos_.end() && !it->second.snapshots.empty()) {
        const auto& snaps = it->second.snapshots;  // newest first
        d.versions = count_of(snaps.size(), tr("version"), tr("versions"));
        d.versions_detail = tr("From %1 to %2").arg(QLocale().toString(to_datetime(snaps.back().snapshot.time).date(), QLocale::ShortFormat),
                                                    QLocale().toString(to_datetime(snaps.front().snapshot.time).date(), QLocale::ShortFormat));
        const SnapshotStats& s = snaps.front().snapshot.stats;
        d.size = tr("Newest: %1 in %2").arg(format_bytes(s.bytes), count_of(s.files, tr("file"), tr("files")));
        std::uint64_t stored = 0;
        it->second.repo->index().for_each([&](const Id&, const BlobLocation& loc) { stored += loc.length; });
        d.size_detail = tr("Stored at the destination: %1").arg(format_bytes(stored));
    } else if (it != repos_.end()) {
        d.versions = tr("No versions yet");
    } else {
        d.versions = tr("...");
        d.versions_detail = tr("Opening the backup...");
    }
    if (!job->second_copy.repository.empty()) {
        const jobs::RunRecord* copy = jobs::last_copy(records);
        d.size_detail += (d.size_detail.isEmpty() ? QString() : QStringLiteral("\n")) +
                         (copy == nullptr ? tr("Second copy: not made yet")
                                          : tr("Second copy: %1, %2").arg(when_text(copy->started), status_text(copy->status).toLower()));
    }
    std::optional<std::uint64_t> free_now;
    if (!is_cloud_location(job->repository)) {
        const QStorageInfo disk(qs(job->repository));
        if (disk.isValid() && disk.isReady()) {
            free_now = static_cast<std::uint64_t>(disk.bytesAvailable());
            d.size_detail += (d.size_detail.isEmpty() ? "" : "\n") +
                             tr("%1 free on %2").arg(format_bytes(*free_now), QDir::toNativeSeparators(disk.rootPath()));
        }
    }
    // When the drive fills up at the rate the backup grows.
    const jobs::FillForecast fill = jobs::forecast_for(records, os::now(), free_now);
    if (const QString text = fill_forecast_text(fill); !text.isEmpty())
        d.size_detail += (d.size_detail.isEmpty() ? "" : "\n") + text;

    // Health check.
    if (check) {
        d.has_check = true;
        d.check_ok = !jobs::found_damage(*check) && check->status != jobs::RunStatus::Failed;
        d.check_when = (check->kind == jobs::RunKind::FullCheck ? tr("Full check %1") : tr("Checked %1"))
                           .arg(when_text(check->started));
        d.check_detail = qs(check->message);
    }
    const jobs::RunRecord* test = jobs::last_restore_test(records);
    if (test != nullptr && test->status != jobs::RunStatus::Cancelled) {
        d.test_ok = !(test->status == jobs::RunStatus::Failed && test->errors > 0);
        d.test_text = tr("Test restore %1: %2").arg(when_text(test->started), qs(test->message));
    }

    // What needs attention, most serious first.
    const AppSettings settings = AppSettings::load();
    const int days = overdue_after_days(*job, settings.remind_after_days);
    const std::uint64_t saved = last_saved(store_, *job);
    const std::uint64_t now = os::now();
    constexpr std::uint64_t kDay = 864'000'000'000ull;
    if (backup && !backup->alert.empty()) {
        d.warning = tr("The last backup found files that may be encrypted by ransomware: %1").arg(qs(backup->alert));
    } else if (check && jobs::found_damage(*check)) {
        d.warning = tr("The last health check found damaged or missing data. Check the destination drive, then "
                       "choose Repair the backup (in the ... menu) to keep what is intact, and back up again. %1")
                        .arg(qs(check->message));
    } else if (check && check->rot_files > 0) {
        d.warning = tr("The last health check found %1 on this PC whose content changed although nothing wrote to "
                       "them (their size and dates are the same): %2. A failing drive or bad memory does this. The "
                       "backup holds the earlier good copy: restore them from the version of %3 (Restore, or "
                       "right-click a file in it for its versions), then check the drive (Drives below).")
                        .arg(count_of(check->rot_files, tr("file"), tr("files")), qs(check->rot),
                             format_time(check->rot_version));
    } else if (test != nullptr && !d.test_ok) {
        d.warning = tr("The last test restore found files that did not come back right: %1. Check the backup "
                       "drive, and back up again soon.").arg(qs(test->message));
    } else if (backup && backup->status == jobs::RunStatus::Failed) {
        d.warning = tr("The last backup failed: %1").arg(qs(backup->message));
    } else if (const jobs::RunRecord* copy = jobs::last_copy(records); copy && copy->status == jobs::RunStatus::Failed) {
        d.warning = tr("The last second copy failed: %1").arg(qs(copy->message));
    } else if (days > 0 && saved > 0 && now > saved && (now - saved) / kDay >= static_cast<std::uint64_t>(days)) {
        d.warning = tr("No version was saved for %1.").arg(count_of((now - saved) / kDay, tr("day"), tr("days")));
    } else if (jobs::fill_warning(fill)) {
        d.warning = tr("The backup drive is full %1 at the rate the backup grows. Keep fewer versions (Edit the "
                       "job, Old backups) or move the backup to a larger drive. What grew shows which folders "
                       "added the most.")
                        .arg(fill_time_words(fill.days));
        d.offer_grew = true;
    }

    // The drives involved, and their health.
    const std::vector<JobDrive>& drives = drives_of(*job);
    std::set<int> source_disks;
    for (const JobDrive& jd : drives)
        if (jd.role == DriveRole::Source) source_disks.insert(jd.health.disk);
    for (const JobDrive& jd : drives) {
        const os::DriveHealth& h = jd.health;
        OverviewData::Drive dr;
        dr.role = jd.role == DriveRole::Backup ? tr("Backup drive") : jd.role == DriveRole::Copy ? tr("Second copy") : tr("Backed up from");
        QStringList kind;
        if (!h.bus.empty()) kind << qs(h.bus);
        if (!h.media.empty()) kind << qs(h.media);
        dr.name = (h.model.empty() ? tr("Disk %1").arg(h.disk) : qs(h.model)) + " (" + kind.join(' ') +
                  (kind.isEmpty() ? QString() : QStringLiteral(", ")) + format_bytes(h.size) + ")";
        QStringList notes;
        const bool bad = h.status == os::DriveHealth::Status::Warning || h.status == os::DriveHealth::Status::Unhealthy;
        switch (h.status) {
            case os::DriveHealth::Status::Healthy: notes << tr("healthy"); break;
            case os::DriveHealth::Status::Warning: notes << tr("warning"); break;
            case os::DriveHealth::Status::Unhealthy: notes << tr("failing"); break;
            case os::DriveHealth::Status::Unknown: notes << tr("health unknown"); break;
        }
        dr.health = h.health_percent().value_or(-1);
        if (dr.health < 0)
            dr.health_unknown = os::process_elevated()
                                    ? tr("This drive does not report what its health could be worked out from.")
                                    : tr("Start Vaultik as administrator (Settings) to see the health of hard disks "
                                         "and SATA SSDs.");
        if (!h.condition.empty()) notes << qs(h.condition);
        for (const std::string& issue : h.smart_issues)
            if (issue != h.condition) notes << qs(issue);
        if (h.read_errors_uncorrected.value_or(0) > 0 && h.smart.empty())  // (else among the issues)
            notes << tr("%1 unrecoverable read errors").arg(*h.read_errors_uncorrected);
        if (h.temperature) notes << tr("%1 °C").arg(*h.temperature);
        for (const jobs::DriveChange& c : jd.worse) notes << change_text(c);
        const bool worse = !jd.worse.empty();
        dr.problem = bad || !h.condition.empty() || h.read_errors_uncorrected.value_or(0) > 0 || h.worn_out() || worse;
        // A backup on the same disk as what it backs up is lost with it.
        if (jd.role != DriveRole::Source && source_disks.contains(h.disk)) {
            notes << tr("the same drive as the backed-up files, so one drive failure loses both");
            dr.problem = true;
        }
        dr.status = notes.join(QStringLiteral(", "));
        if (dr.problem && d.warning.isEmpty() && (bad || !h.condition.empty() || h.worn_out() || worse)) {
            if (jd.role != DriveRole::Source)
                d.warning = tr("The drive with the backup reports a problem (%1: %2). Copy the backup to another drive "
                               "soon, before it fails.").arg(dr.name, dr.status);
            else
                d.warning = tr("A drive with the backed-up files reports a problem (%1: %2). Make sure the backup is "
                               "up to date, and replace the drive soon.").arg(dr.name, dr.status);
        }
        d.drives.push_back(std::move(dr));
    }

    // Backup days.
    for (const jobs::RunRecord& r : records) {
        if (r.kind != jobs::RunKind::Backup || r.status == jobs::RunStatus::Cancelled) continue;
        const QDate day = to_datetime(r.started).date();
        const OverviewData::Day kind = r.status == jobs::RunStatus::Success   ? OverviewData::Day::Saved
                                       : r.status == jobs::RunStatus::Partial ? OverviewData::Day::Partial
                                                                              : OverviewData::Day::Failed;
        auto& cur = d.days[day];
        // The best result of the day: a saved version beats a failure.
        if (cur == OverviewData::Day::None || static_cast<int>(kind) < static_cast<int>(cur)) cur = kind;
    }
    if (it != repos_.end())  // versions made outside this job's history (e.g. from the command line)
        for (const SnapshotEntry& e : it->second.snapshots) {
            auto& cur = d.days[to_datetime(e.snapshot.time).date()];
            if (cur == OverviewData::Day::None || cur == OverviewData::Day::Failed) cur = OverviewData::Day::Saved;
        }
    overview_->set_data(d);
}

void MainWindow::change_version(int index, bool pin, bool note) {
    const jobs::Job* job = current_job();
    if (!job) return;
    const std::string job_id = job->id;
    auto it = repos_.find(job_id);
    if (it == repos_.end() || index < 0 || index >= static_cast<int>(it->second.snapshots.size())) return;
    // A copy: the open backups may change while the dialogs below are open.
    const SnapshotEntry entry = it->second.snapshots[static_cast<std::size_t>(index)];
    Snapshot updated = entry.snapshot;
    if (note) {
        bool ok = false;
        const QString text = QInputDialog::getText(this, tr("Note"), tr("Describe this version:"), QLineEdit::Normal,
                                                   qs(entry.snapshot.note), &ok);
        if (!ok) return;
        updated.note = u8(text.simplified().left(500));
    } else {
        updated.pinned = pin;
    }
    const std::shared_ptr<Repository> repo = it->second.repo;
    const Id id = entry.id;
    const QString error = run_busy(this, tr("Saving..."), [&](const std::function<bool()>&) {
        // Shared: a cleanup (exclusive) cannot remove the version while it is rewritten.
        RepoLock lock(*repo, RepoLock::Mode::Shared, note ? "note" : "pin", std::chrono::seconds(30));
        repo->update_snapshot(id, updated);
    });
    if (!error.isEmpty()) {
        plain_warning(this, windowTitle(), error);
        return;
    }
    // Found again by id: the list may have been reloaded or dropped meanwhile.
    if (const auto again = repos_.find(job_id); again != repos_.end())
        for (SnapshotEntry& e : again->second.snapshots)
            if (e.id == id) e.snapshot = updated;
    show_versions();
}

void MainWindow::delete_version(int index) {
    const jobs::Job* job = current_job();
    if (!job) return;
    const std::string job_id = job->id;
    auto it = repos_.find(job_id);
    if (it == repos_.end() || index < 0 || index >= static_cast<int>(it->second.snapshots.size())) return;
    const SnapshotEntry entry = it->second.snapshots[static_cast<std::size_t>(index)];
    if (entry.snapshot.pinned) return;
    const auto answer = QMessageBox::question(
        this, windowTitle(),
        tr("Delete the version from %1? Files only it holds can no longer be restored. Its space is freed at "
           "the next cleanup.")
            .arg(QLocale().toString(to_datetime(entry.snapshot.time), QLocale::LongFormat)));
    if (answer != QMessageBox::Yes) return;
    it = repos_.find(job_id);  // the question ran the event loop
    if (it == repos_.end()) return;
    const std::shared_ptr<Repository> repo = it->second.repo;
    const QString error = run_busy(this, tr("Deleting the version..."), [&](const std::function<bool()>&) {
        RepoLock lock(*repo, RepoLock::Mode::Exclusive, "delete a version", std::chrono::seconds(10));
        ForgetOptions fo;
        fo.ids = {entry.id};
        run_forget(*repo, fo);
    });
    if (!error.isEmpty()) {
        plain_warning(this, windowTitle(), error);
        return;
    }
    it = repos_.find(job_id);  // the work ran the event loop
    if (it != repos_.end()) {
        auto& list = it->second.snapshots;
        list.erase(std::remove_if(list.begin(), list.end(), [&](const SnapshotEntry& e) { return e.id == entry.id; }),
                   list.end());
    }
    show_versions();
    status_->showMessage(tr("Version deleted."), 8000);
}

void MainWindow::restore_from_folder() {
    if (!unlock()) return;
    // A folder (USB drive, disk, network share) or cloud storage.
    QMessageBox where(QMessageBox::Question, tr("Restore from a backup"), tr("Where is the backup?"),
                      QMessageBox::Cancel, this);
    QPushButton* folder_button = where.addButton(tr("In a folder..."), QMessageBox::AcceptRole);
    QPushButton* cloud_button = where.addButton(tr("In cloud storage..."), QMessageBox::AcceptRole);
    where.exec();

    QString location;
    CloudTarget cloud;
    if (where.clickedButton() == folder_button) {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the backup folder"));
        if (dir.isEmpty()) return;
        if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("config")))) {
            QMessageBox::warning(this, windowTitle(),
                                 tr("%1 does not hold a Vaultik backup.").arg(QDir::toNativeSeparators(dir)));
            return;
        }
        location = QDir::toNativeSeparators(dir);
    } else if (where.clickedButton() == cloud_button) {
        CloudDialog dialog(CloudTarget{}, this);
        if (dialog.exec() != QDialog::Accepted) return;
        cloud = dialog.target();
        location = cloud.location;
    } else {
        return;
    }

    bool ok = false;
    QString password = QInputDialog::getText(this, tr("Backup password"), tr("Password of the backup in %1:").arg(location),
                                             QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty()) {
        cloud.wipe();
        return;
    }

    jobs::Job source;
    source.name = u8(location);
    source.repository = source.name;
    const CloudCredentials keys = cloud.credentials();
    cloud.wipe();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    status_->showMessage(tr("Opening the backup..."));
    RepoHandle::open_async(
        this, source.repository, u8(password), keys,
        [this, source, password, keys, generation = lock_generation_](std::shared_ptr<Repository> repo,
                                                                      std::vector<SnapshotEntry> list, QString error) mutable {
            QApplication::restoreOverrideCursor();
            status_->clearMessage();
            if (generation != lock_generation_) {
                // Locked again while it opened: nothing of it is shown.
                password.fill(QChar(0));
                return;
            }
            if (!error.isEmpty()) {
                QMessageBox::warning(this, windowTitle(), error);
            } else if (list.empty()) {
                QMessageBox::information(this, windowTitle(), tr("This backup has no versions to restore from."));
            } else {
                RestoreDialog dialog(source, std::move(repo), std::move(list), 0, this, password, keys);
                dialog.exec();
            }
            password.fill(QChar(0));
        });
}

void MainWindow::show_computers() {
    if (!unlock()) return;
    // One entry per destination, with the jobs saving there.
    struct Group {
        std::string key;
        std::vector<jobs::Job> jobs;
    };
    std::vector<Group> groups;
    for (jobs::Job job : store_.list()) {
        jobs::relocate(job);
        std::string key = job.repository;
        if (cloud_kind(key) == CloudKind::None) {
            key = u8(QDir::cleanPath(QDir::fromNativeSeparators(qs(key))).toLower());
        }
        auto it = std::find_if(groups.begin(), groups.end(), [&key](const Group& g) { return g.key == key; });
        if (it == groups.end()) {
            groups.push_back({key, {}});
            it = groups.end() - 1;
        }
        it->jobs.push_back(std::move(job));
    }
    std::vector<ComputerSource> sources;
    for (const Group& g : groups) {
        ComputerSource s;
        const jobs::Job& first = g.jobs.front();
        QStringList names;
        for (const jobs::Job& j : g.jobs) names << qs(j.name);
        s.title = tr("%1 (%2)").arg(location_text(first.repository), names.join(QStringLiteral(", ")));
        s.repository = first.repository;
        if (!jobs::drive_connected(first)) {
            s.error = tr("The backup drive is not connected.");
            sources.push_back(std::move(s));
            continue;
        }
        // The password of any job saving there; asked for (and saved) when none has one.
        const jobs::Job* with = nullptr;
        for (const jobs::Job& j : g.jobs) {
            try {
                std::string pw = store_.password(j.id);
                if (!pw.empty()) {
                    with = &j;
                    s.password = std::move(pw);
                    break;
                }
            } catch (const Error&) {
            }
        }
        if (with == nullptr && ensure_password(first)) {
            with = &first;
            try {
                s.password = store_.password(first.id);
            } catch (const Error&) {
            }
        }
        if (with == nullptr || s.password.empty()) {
            s.error = tr("This backup needs its password.");
        } else {
            try {
                s.cloud = store_.cloud_credentials(*with);
            } catch (const Error& e) {
                s.error = QString::fromUtf8(e.what());
            }
        }
        if (!s.error.isEmpty()) secure_wipe(s.password.data(), s.password.size());
        sources.push_back(std::move(s));
    }
    ComputersDialog dialog(std::move(sources), this);
    dialog.exec();
}

void MainWindow::open_keys() {
    const jobs::Job* job = current_job();
    KeyDialog dialog(store_, job ? QDir::toNativeSeparators(qs(job->repository)) : QString(), this);
    dialog.exec();
    repos_.clear();  // reopen with the saved password, which may have changed
    if (tabs_->currentIndex() <= 2) load_snapshots(false);
}

void MainWindow::open_restore(int snapshot_index, const QString& select_path) {
    if (!unlock()) return;
    const jobs::Job* job = current_job();
    if (!job) return;
    auto it = repos_.find(job->id);
    if (it == repos_.end()) {
        // Open the backup first, then come back here.
        tabs_->setCurrentIndex(0);
        load_snapshots(false);
        const std::string id = job->id;
        auto* timer = new QTimer(this);  // deletes itself below
        connect(timer, &QTimer::timeout, this, [this, timer, id, snapshot_index, select_path] {
            if (loading_job_ == id) return;
            timer->deleteLater();
            if (repos_.contains(id) && current_job() && current_job()->id == id) open_restore(snapshot_index, select_path);
        });
        timer->start(200);
        return;
    }
    if (it->second.snapshots.empty()) {
        QMessageBox::information(this, windowTitle(), tr("This job has no versions to restore from yet."));
        return;
    }
    // A disk image restores whole disks, not files.
    const std::vector<SnapshotEntry>& all = it->second.snapshots;
    const std::size_t picked = static_cast<std::size_t>(std::clamp(snapshot_index, 0, static_cast<int>(all.size()) - 1));
    if (all[picked].snapshot.image || (!job->image_disks.empty() && select_path.isEmpty())) {
        std::vector<SnapshotEntry> images;
        int selected = 0;
        for (std::size_t i = 0; i < all.size(); ++i) {
            if (!all[i].snapshot.image) continue;
            if (i == picked) selected = static_cast<int>(images.size());
            images.push_back(all[i]);
        }
        if (images.empty()) {
            QMessageBox::information(this, windowTitle(), tr("This job has no disk images to restore from yet."));
            return;
        }
        ImageRestoreDialog dialog(*job, it->second.repo, std::move(images), selected, this);
        dialog.exec();
        return;
    }
    RestoreDialog dialog(*job, it->second.repo, it->second.snapshots, std::max(snapshot_index, 0), this, {}, {},
                         select_path);
    dialog.exec();
}

void MainWindow::new_job() {
    JobDialog dialog(store_, nullptr, this);
    if (dialog.exec() == QDialog::Accepted) reload_jobs(dialog.job().id);
}

void MainWindow::edit_job() {
    const jobs::Job* job = current_job();
    if (!job) return;
    JobDialog dialog(store_, job, this);
    if (dialog.exec() == QDialog::Accepted) {
        repos_.erase(dialog.job().id);
        reload_jobs(dialog.job().id);
    }
}

void MainWindow::delete_job() {
    const jobs::Job* current = current_job();
    if (!current || runner_->running()) return;
    const jobs::Job job = *current;  // the list may change while the question is open
    const auto answer = QMessageBox::question(
        this, windowTitle(),
        tr("Delete the job \"%1\"? Its schedule, stored password and history are removed. The backups "
           "in %2 are kept.")
            .arg(qs(job.name), location_text(job.repository)));
    if (answer != QMessageBox::Yes) return;
    const std::string id = job.id;
    jobs::remove_schedule(id);
    store_.remove(id);
    repos_.erase(id);
    reload_jobs();
}

void MainWindow::run_job() {
    if (runner_->running()) {
        runner_->cancel();
        run_->setEnabled(false);
        progress_text_->setText(tr("Stopping..."));
        return;
    }
    const jobs::Job* job = current_job();
    if (!job) return;
    start_run({"run-job", qs(job->id)}, false);
}

void MainWindow::check_job(bool full) {
    const jobs::Job* current = current_job();
    if (!current || runner_->running()) return;
    const jobs::Job copy = *current;  // the list may change while the question is open
    const jobs::Job* job = &copy;
    if (full && is_cloud_location(job->repository)) {
        const auto answer = QMessageBox::question(
            this, tr("Full health check"),
            tr("A full check downloads everything this backup has stored in %1, which can take long and may "
               "count against your plan's download allowance. Check it now?")
                .arg(location_text(job->repository)));
        if (answer != QMessageBox::Yes) return;
    }
    QStringList args{"check-job", qs(job->id)};
    if (full) args << "--full";
    start_run(args, true);
}

void MainWindow::show_grew() {
    const jobs::Job* current = current_job();
    if (!current) return;
    const auto it = repos_.find(current->id);
    if (it == repos_.end() || !it->second.repo) {
        QMessageBox::information(this, tr("What grew"), tr("The backup is not open yet. Try again in a moment."));
        return;
    }
    // Copies: the open backup may be reloaded while the dialog is open.
    GrewDialog dialog(store_, *current, it->second.repo, it->second.snapshots, this);
    dialog.exec();
    if (dialog.job_changed()) reload_jobs(current->id);
}

void MainWindow::test_restore() {
    const jobs::Job* job = current_job();
    if (!job || runner_->running()) return;
    if (is_cloud_location(job->repository)) {
        const auto answer = QMessageBox::question(
            this, tr("Test restore"),
            tr("A test restore downloads a few files (up to 500 MB) from %1. Test it now?")
                .arg(location_text(job->repository)));
        if (answer != QMessageBox::Yes) return;
    }
    test_run_ = true;
    start_run({"test-restore", qs(job->id)}, true);
    if (running_job_.empty()) test_run_ = false;  // did not start (no password)
}

void MainWindow::repair_backup() {
    const jobs::Job* job = current_job();
    if (!job || runner_->running()) return;
    QMessageBox box(QMessageBox::Question, tr("Repair the backup"), QString(), QMessageBox::Yes | QMessageBox::Cancel,
                    this);
    box.setTextFormat(Qt::PlainText);
    box.setText(tr("Repair reads all of the backup's stored data, checks every part and rebuilds the index from "
                   "what is intact. Versions that point at damaged data are saved again without the files that "
                   "were lost (their note says so); the next backup stores those files again if they are still "
                   "on this PC.") +
                "\n\n" +
                (is_cloud_location(job->repository)
                     ? tr("From cloud storage this downloads everything the backup holds.") + "\n\n"
                     : QString()) +
                tr("Only needed when a health check or a restore found damage. Repair now?"));
    box.setDefaultButton(QMessageBox::Cancel);
    if (box.exec() != QMessageBox::Yes) return;
    repair_run_ = true;
    start_run({"repair", "--job", qs(job->id)}, true);
    if (running_job_.empty()) repair_run_ = false;
}

void MainWindow::start_run(const QStringList& args, bool check) {
    const jobs::Job* current = current_job();
    if (!current) return;
    const jobs::Job copy = *current;  // ensure_password may show a dialog
    const jobs::Job* job = &copy;
    if (!ensure_password(copy) || runner_->running()) return;
    running_job_ = job->id;
    check_run_ = check;
    check_result_.clear();
    run_started_ = std::chrono::steady_clock::now();
    log_->appendPlainText((repair_run_ ? tr("--- %1: repair started")
                           : test_run_ ? tr("--- %1: test restore started")
                           : check     ? tr("--- %1: health check started")
                                       : tr("--- %1: backup started"))
                              .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")));
    running_label_->setText(repair_run_ ? tr("Repairing the backup...")
                            : test_run_ ? tr("Testing a restore...")
                            : check     ? tr("Checking the backup...")
                                        : tr("Backing up..."));
    bar_->setRange(0, 0);
    progress_text_->setText(tr("Starting..."));
    current_file_->clear();
    set_running(true);
    // In Vaultik started as administrator, a job with administrator rights backs up from its
    // approved copy (the one its schedule runs, with checked commands) with those rights.
    // Everything else runs with the user's own rights: the job comes from settings any program
    // running as the user can change (see Runner::engine_environment).
    QStringList run_args = args;
    const bool approved = os::process_elevated() && job->run_as_admin && args.value(0) == QLatin1String("run-job") &&
                          jobs::has_admin_copy(job->id);
    if (approved) run_args << QStringLiteral("--admin");
    runner_->keep_administrator_rights(approved);
    runner_->start(run_args);
}

void MainWindow::set_running(bool running) {
    if (!running) paused_ = false;  // a new run starts unpaused
    pause_->setText(paused_ ? tr("Resume") : tr("Pause"));
    pause_->setIcon(icon(paused_ ? QStringLiteral("play_20_regular") : QStringLiteral("stop_20_regular")));
    bar_->setEnabled(!paused_);
    const jobs::Job* job = current_job();
    const bool this_job = running && job && job->id == running_job_;
    progress_box_->setVisible(this_job);
    run_->setEnabled(!running || this_job);
    run_->setText(this_job ? tr("Stop") : tr("Back up now"));
    run_->setIcon(icon(this_job ? QStringLiteral("stop_20_regular") : QStringLiteral("play_20_regular"),
                       theme().accent_text));
    more_->setEnabled(!this_job);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (tray_ != nullptr && !quitting_ && AppSettings::load().close_to_tray) {
        // Keep watching from the tray; a running backup goes on too.
        hide();
        relock();
        event->ignore();
        QSettings settings;
        if (!settings.value(QStringLiteral("tray/explained"), false).toBool()) {
            settings.setValue(QStringLiteral("tray/explained"), true);
            tray_->explain_hidden();
        }
        return;
    }
    if (runner_->running()) {
        const auto answer = QMessageBox::question(this, windowTitle(), tr("A backup is running. Stop it and quit?"));
        if (answer != QMessageBox::Yes) {
            event->ignore();
            return;
        }
        runner_->cancel();
    }
    event->accept();
}
