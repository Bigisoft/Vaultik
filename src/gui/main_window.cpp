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
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
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
#include "common.h"
#include "compare_dialog.h"
#include "drive_watcher.h"
#include "find_page.h"
#include "job_card.h"
#include "job_dialog.h"
#include "key_dialog.h"
#include "overview.h"
#include "restore_dialog.h"
#include "app_settings.h"
#include "runner.h"
#include "settings_dialog.h"
#include "theme.h"
#include "tray.h"
#include "versions_view.h"
#include "vaultik/jobs/admin_jobs.hpp"
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
    setWindowTitle(QStringLiteral("Vaultik"));
    setWindowIcon(app_icon());
    resize(1180, 760);
    setMinimumSize(900, 560);

    stack_ = new QStackedWidget;
    stack_->addWidget(build_empty_page());
    stack_->addWidget(build_job_page());
    auto* content = new QWidget;
    content->setObjectName(QStringLiteral("Content"));
    auto* cl = new QVBoxLayout(content);
    cl->setContentsMargins(28, 22, 28, 18);
    cl->addWidget(stack_);

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
            check_result_ = damage ? tr("The health check found problems: %1").arg(ev.value("message").toString())
                                   : tr("Health check: no problems.");
            log_->appendPlainText(check_result_);
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
        check_run_ = false;
        set_running(false);
        QString msg;
        if (check) {
            msg = result == "success"   ? tr("Health check: no problems.")
                  : result == "cancelled" ? tr("Health check cancelled.")
                                          : tr("Health check failed: %1").arg(ev.value("message").toString());
            log_->appendPlainText(msg);
            statusBar()->showMessage(msg, 15000);
            reload_jobs(current_job() ? current_job()->id : id);
            return;
        }
        if (result == "success" || result == "partial") {
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
        statusBar()->showMessage(msg, 15000);
        repos_.erase(id);  // a new version: reload the list when it is shown
        reload_jobs(current_job() ? current_job()->id : id);
    });

    connect(list_, &QListWidget::currentRowChanged, this, [this] { show_job(); });
    connect(ThemeNotifier::instance(), &ThemeNotifier::changed, this, &MainWindow::refresh_icons);

    // Backups that start when their drive is connected. Windows tells top-level windows about new
    // drives, so this one needs its native window even while it stays hidden in the tray.
    (void)winId();
    connect(new DriveWatcher(this), &DriveWatcher::connected, this, &MainWindow::drives_connected);
    // Versions open in Explorer: a button to close them, and they close when Vaultik quits.
    mounts_button_ = new QPushButton;
    mounts_button_->setProperty("role", "flat");
    mounts_button_->setToolTip(tr("Close the versions shown as folders in File Explorer"));
    connect(mounts_button_, &QPushButton::clicked, this, &MainWindow::close_mounts);
    statusBar()->addPermanentWidget(mounts_button_);
    mounts_button_->hide();
    connect(qApp, &QCoreApplication::aboutToQuit, this, &MainWindow::close_mounts);

    connect_timer_ = new QTimer(this);
    connect_timer_->setInterval(3000);
    connect(connect_timer_, &QTimer::timeout, this, &MainWindow::watch_connect_runs);

    // Scheduled runs happen outside this window; pick up their results.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!runner_->running()) refresh_sidebar();
    });
    timer->start(30000);

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
            if (!QProcess::startDetached(program, {QStringLiteral("run-job"), qs(job.id), QStringLiteral("--on-connect")},
                                         QString(), &pid))
                continue;
            connect_runs_.push_back({job.id, static_cast<std::uint32_t>(pid), root, started});
            const QString title = tr("Backing up %1").arg(qs(job.name));
            const QString text = tr("%1 was connected. Leave it plugged in until Vaultik says it is done.").arg(root);
            if (tray_ != nullptr) tray_->notify(title, text);
            statusBar()->showMessage(title + "  " + text, 15000);
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
        statusBar()->showMessage(title + "  " + text, 30000);
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
    const SnapshotEntry& x = snaps[static_cast<std::size_t>(a)];
    const SnapshotEntry& y = snaps[static_cast<std::size_t>(b)];
    const SnapshotEntry& older = x.snapshot.time <= y.snapshot.time ? x : y;
    const SnapshotEntry& newer = x.snapshot.time <= y.snapshot.time ? y : x;
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
    const jobs::Job* job = current_job();
    if (!job || !ensure_password(*job)) return;
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
    statusBar()->showMessage(tr("Opening the version in File Explorer..."), 10000);

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
            statusBar()->clearMessage();
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
    process->start(program, {QStringLiteral("mount"), QStringLiteral("--job"), qs(job->id), qs(snapshot),
                             QStringLiteral("--json")});
}

void MainWindow::close_mounts() {
    // Each one removes its folder; give them a moment, so nothing is left behind.
    for (const Mount& m : mounts_) m.process->write("cancel\n");
    for (const Mount& m : mounts_) m.process->waitForFinished(5000);
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
    SettingsDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;
    update_tray();
    if (tray_ != nullptr) tray_->check();
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
    menu->setToolTipsVisible(true);
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
    tabs_->addTab(overview_, tr("Overview"));

    versions_ = new VersionsView;
    connect(overview_, &OverviewView::versions_requested, this, [this] { tabs_->setCurrentWidget(versions_); });
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
        if (i == 0 || i == 1) load_snapshots(false);
    });
    layout->addWidget(tabs_, 1);
    return page;
}

void MainWindow::refresh_icons() {
    const Theme& t = theme();
    new_job_->setIcon(icon(QStringLiteral("add_20_regular"), t.accent_text));
    open_backup_->setIcon(icon(QStringLiteral("folder_open_20_regular")));
    keys_->setIcon(icon(QStringLiteral("key_20_regular")));
    settings_->setIcon(icon(QStringLiteral("settings_20_regular")));
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
    item->setData(JobCardDelegate::DetailRole, qs(jobs::describe(j.schedule)) + "  ·  " + location_text(j.repository));
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
    if (tabs_->currentIndex() <= 1) load_snapshots(false);
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
        item->setText(1, backup ? tr("Backup") : r.kind == jobs::RunKind::FullCheck ? tr("Full check") : tr("Health check"));
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
            item->setText(2, jobs::found_damage(r) ? tr("Problems found") : status_text(r.status));
            item->setText(4, format_bytes(r.bytes));
            item->setToolTip(4, tr("Read back and checked"));
        }
        if (r.finished > r.started) item->setText(7, format_duration(static_cast<double>(r.finished - r.started) / 1e7));
        item->setText(8, qs(r.alert.empty() ? r.message : r.alert + (r.message.empty() ? "" : "; " + r.message)));
        for (int c = 3; c < 8; ++c) item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    }
    for (int c = 0; c < 8; ++c) history_->resizeColumnToContents(c);

    QString next = qs(jobs::describe(job->schedule));
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
        if (damage) status_icon_->setPixmap(status_icon(jobs::RunStatus::Failed).pixmap(16, 16));
        text += "  ·  " + tr("Checked %1: %2")
                              .arg(when_text(check->started),
                                   damage                                        ? tr("problems found")
                                   : check->status == jobs::RunStatus::Success ? tr("no problems")
                                                                                 : status_text(check->status).toLower());
    }
    status_text_->setText(text + "  ·  " + tr("Schedule: %1").arg(next));
    refresh_overview();
}

void MainWindow::load_snapshots(bool force) {
    const jobs::Job* job = current_job();
    if (!job) return;
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
                           [this, id](std::shared_ptr<Repository> repo, std::vector<SnapshotEntry> list, QString error) {
                               loading_job_.clear();
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
        d.next_detail = qs(jobs::describe(job->schedule)) + ", " + tr("if the computer is on (otherwise right after)");
    } else {
        d.next_run = tr("When you start it");
        d.next_detail = tr("This job has no schedule.");
    }
    if (job->backup_on_connect) d.next_detail += "\n" + tr("Also when its drive is connected.");

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
    if (!is_cloud_location(job->repository)) {
        const QStorageInfo disk(qs(job->repository));
        if (disk.isValid() && disk.isReady())
            d.size_detail += (d.size_detail.isEmpty() ? "" : "\n") +
                             tr("%1 free on %2").arg(format_bytes(static_cast<std::uint64_t>(disk.bytesAvailable())),
                                                     QDir::toNativeSeparators(disk.rootPath()));
    }

    // Health check.
    if (check) {
        d.has_check = true;
        d.check_ok = !jobs::found_damage(*check) && check->status != jobs::RunStatus::Failed;
        d.check_when = (check->kind == jobs::RunKind::FullCheck ? tr("Full check %1") : tr("Checked %1"))
                           .arg(when_text(check->started));
        d.check_detail = qs(check->message);
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
        d.warning = tr("The last health check found damaged or missing data. Back up again soon, and check the "
                       "destination drive. %1").arg(qs(check->message));
    } else if (backup && backup->status == jobs::RunStatus::Failed) {
        d.warning = tr("The last backup failed: %1").arg(qs(backup->message));
    } else if (days > 0 && saved > 0 && now > saved && (now - saved) / kDay >= static_cast<std::uint64_t>(days)) {
        d.warning = tr("No version was saved for %1.").arg(count_of((now - saved) / kDay, tr("day"), tr("days")));
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
    auto it = repos_.find(job->id);
    if (it == repos_.end() || index < 0 || index >= static_cast<int>(it->second.snapshots.size())) return;
    SnapshotEntry& entry = it->second.snapshots[static_cast<std::size_t>(index)];
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
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    entry.snapshot = updated;
    show_versions();
}

void MainWindow::delete_version(int index) {
    const jobs::Job* job = current_job();
    if (!job) return;
    auto it = repos_.find(job->id);
    if (it == repos_.end() || index < 0 || index >= static_cast<int>(it->second.snapshots.size())) return;
    const SnapshotEntry entry = it->second.snapshots[static_cast<std::size_t>(index)];
    if (entry.snapshot.pinned) return;
    const auto answer = QMessageBox::question(
        this, windowTitle(),
        tr("Delete the version from %1? Files only it holds can no longer be restored. Its space is freed at "
           "the next cleanup.")
            .arg(QLocale().toString(to_datetime(entry.snapshot.time), QLocale::LongFormat)));
    if (answer != QMessageBox::Yes) return;
    const std::shared_ptr<Repository> repo = it->second.repo;
    const QString error = run_busy(this, tr("Deleting the version..."), [&](const std::function<bool()>&) {
        RepoLock lock(*repo, RepoLock::Mode::Exclusive, "delete a version", std::chrono::seconds(10));
        ForgetOptions fo;
        fo.ids = {entry.id};
        run_forget(*repo, fo);
    });
    if (!error.isEmpty()) {
        QMessageBox::warning(this, windowTitle(), error);
        return;
    }
    auto& list = it->second.snapshots;
    list.erase(std::remove_if(list.begin(), list.end(), [&](const SnapshotEntry& e) { return e.id == entry.id; }),
               list.end());
    show_versions();
    statusBar()->showMessage(tr("Version deleted."), 8000);
}

void MainWindow::restore_from_folder() {
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
    statusBar()->showMessage(tr("Opening the backup..."));
    RepoHandle::open_async(
        this, source.repository, u8(password), keys,
        [this, source, password, keys](std::shared_ptr<Repository> repo, std::vector<SnapshotEntry> list,
                                       QString error) mutable {
            QApplication::restoreOverrideCursor();
            statusBar()->clearMessage();
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

void MainWindow::open_keys() {
    const jobs::Job* job = current_job();
    KeyDialog dialog(store_, job ? QDir::toNativeSeparators(qs(job->repository)) : QString(), this);
    dialog.exec();
    repos_.clear();  // reopen with the saved password, which may have changed
    if (tabs_->currentIndex() <= 1) load_snapshots(false);
}

void MainWindow::open_restore(int snapshot_index, const QString& select_path) {
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
    const jobs::Job* job = current_job();
    if (!job || runner_->running()) return;
    const auto answer = QMessageBox::question(
        this, windowTitle(),
        tr("Delete the job \"%1\"? Its schedule, stored password and history are removed. The backups "
           "in %2 are kept.")
            .arg(qs(job->name), location_text(job->repository)));
    if (answer != QMessageBox::Yes) return;
    const std::string id = job->id;
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
    const jobs::Job* job = current_job();
    if (!job || runner_->running()) return;
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

void MainWindow::start_run(const QStringList& args, bool check) {
    const jobs::Job* job = current_job();
    if (!job || !ensure_password(*job)) return;
    running_job_ = job->id;
    check_run_ = check;
    check_result_.clear();
    run_started_ = std::chrono::steady_clock::now();
    log_->appendPlainText((check ? tr("--- %1: health check started") : tr("--- %1: backup started"))
                              .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")));
    running_label_->setText(check ? tr("Checking the backup...") : tr("Backing up..."));
    bar_->setRange(0, 0);
    progress_text_->setText(tr("Starting..."));
    current_file_->clear();
    set_running(true);
    runner_->start(args);
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
