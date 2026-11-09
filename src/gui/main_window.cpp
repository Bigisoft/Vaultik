#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common.h"
#include "job_dialog.h"
#include "restore_dialog.h"
#include "runner.h"
#include "vaultik/jobs/scheduler.hpp"

using namespace vaultik;

namespace {

QIcon status_icon(const QStyle* st, jobs::RunStatus s) {
    switch (s) {
        case jobs::RunStatus::Success: return st->standardIcon(QStyle::SP_DialogApplyButton);
        case jobs::RunStatus::Partial: return st->standardIcon(QStyle::SP_MessageBoxWarning);
        case jobs::RunStatus::Failed: return st->standardIcon(QStyle::SP_MessageBoxCritical);
        case jobs::RunStatus::Cancelled: return st->standardIcon(QStyle::SP_BrowserStop);
    }
    return {};
}

QString status_text(jobs::RunStatus s) {
    switch (s) {
        case jobs::RunStatus::Success: return QObject::tr("Completed");
        case jobs::RunStatus::Partial: return QObject::tr("Completed, some files skipped");
        case jobs::RunStatus::Failed: return QObject::tr("Failed");
        case jobs::RunStatus::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

QString resources_text(const ResourcePolicy& r) {
    QStringList parts;
    parts << QObject::tr("CPU %1, disk %2, memory %3")
                 .arg(vaultik::to_string(r.cpu), vaultik::to_string(r.io), vaultik::to_string(r.memory));
    if (r.cpu_limit_percent) parts << QObject::tr("CPU at most %1%").arg(r.cpu_limit_percent);
    if (r.max_memory) parts << QObject::tr("memory at most %1").arg(format_bytes(r.max_memory));
    if (r.read_rate) parts << QObject::tr("read at most %1/s").arg(format_bytes(r.read_rate));
    if (r.write_rate) parts << QObject::tr("write at most %1/s").arg(format_bytes(r.write_rate));
    return parts.join(", ");
}

}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("Vaultik"));
    resize(1100, 700);

    auto* toolbar = addToolBar(tr("Main"));
    toolbar->setMovable(false);
    toolbar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    QAction* add = toolbar->addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder), tr("New backup job"));
    connect(add, &QAction::triggered, this, &MainWindow::new_job);
    QAction* open_backup =
        toolbar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), tr("Restore from a backup folder..."));
    open_backup->setToolTip(tr("Open any Vaultik backup with its password, e.g. one made on another computer"));
    connect(open_backup, &QAction::triggered, this, &MainWindow::restore_from_folder);
    QAction* refresh = toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), tr("Refresh"));
    connect(refresh, &QAction::triggered, this, [this] {
        repos_.clear();
        reload_jobs(current_job() ? current_job()->id : std::string());
    });

    list_ = new QListWidget;
    list_->setMinimumWidth(240);
    list_->setIconSize(QSize(20, 20));
    stack_ = new QStackedWidget;
    stack_->addWidget(build_empty_page());
    stack_->addWidget(build_job_page());

    auto* split = new QSplitter;
    split->addWidget(list_);
    split->addWidget(stack_);
    split->setStretchFactor(1, 1);
    setCentralWidget(split);

    runner_ = new Runner(this);
    connect(runner_, &Runner::progress, this, [this](const QJsonObject& ev) {
        const double secs = std::max(
            std::chrono::duration<double>(std::chrono::steady_clock::now() - run_started_).count(), 0.001);
        const auto bytes = static_cast<std::uint64_t>(ev.value("bytes").toInteger());
        progress_text_->setText(tr("%1 files, %2 read (%3/s), %4 new")
                                    .arg(ev.value("files").toInteger())
                                    .arg(format_bytes(bytes))
                                    .arg(format_bytes(static_cast<std::uint64_t>(static_cast<double>(bytes) / secs)))
                                    .arg(format_bytes(static_cast<std::uint64_t>(ev.value("new_bytes").toInteger()))));
        current_file_->setText(ev.value("current").toString());
    });
    connect(runner_, &Runner::warning, this, [this](const QString& msg) { log_->appendPlainText(msg); });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        const std::string id = running_job_;
        running_job_.clear();
        set_running(false);
        QString msg;
        if (result == "success" || result == "partial") {
            msg = tr("Backup finished: %1 files, %2 new, %3 stored")
                      .arg(ev.value("files").toInteger())
                      .arg(format_bytes(static_cast<std::uint64_t>(ev.value("new_bytes").toInteger())))
                      .arg(format_bytes(static_cast<std::uint64_t>(ev.value("stored_bytes").toInteger())));
            if (result == "partial") msg += " " + tr("(%1 skipped, see Log)").arg(ev.value("errors").toInteger());
        } else if (result == "cancelled") {
            msg = tr("Backup cancelled; no snapshot was saved.");
        } else {
            msg = tr("Backup failed: %1").arg(ev.value("message").toString());
            log_->appendPlainText(msg);
        }
        statusBar()->showMessage(msg);
        repos_.erase(id);  // new snapshot: reload the list when it is shown
        reload_jobs(current_job() ? current_job()->id : id);
    });

    connect(list_, &QListWidget::currentRowChanged, this, [this] { show_job(); });

    // Scheduled runs happen outside this window; pick up their results.
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        if (!runner_->running()) refresh_history();
    });
    timer->start(20000);

    reload_jobs();
}

QWidget* MainWindow::build_empty_page() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->addStretch();
    auto* label = new QLabel(tr("<h2>No backup jobs yet</h2><p>A job says what to back up, where to and when."
                                "<br>Backups are deduplicated, compressed and encrypted.</p>"));
    label->setAlignment(Qt::AlignCenter);
    layout->addWidget(label);
    auto* button = new QPushButton(tr("Create a backup job"));
    connect(button, &QPushButton::clicked, this, &MainWindow::new_job);
    auto* row = new QHBoxLayout;
    row->addStretch();
    row->addWidget(button);
    row->addStretch();
    layout->addLayout(row);
    layout->addStretch();
    return page;
}

QWidget* MainWindow::build_job_page() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    title_ = new QLabel;
    QFont f = title_->font();
    f.setPointSizeF(f.pointSizeF() * 1.5);
    f.setBold(true);
    title_->setFont(f);
    layout->addWidget(title_);

    details_ = new QLabel;
    details_->setWordWrap(true);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(details_);
    last_run_ = new QLabel;
    layout->addWidget(last_run_);

    run_ = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), tr("Back up now"));
    edit_ = new QPushButton(tr("Edit..."));
    restore_ = new QPushButton(style()->standardIcon(QStyle::SP_ArrowBack), tr("Restore..."));
    delete_ = new QPushButton(tr("Delete"));
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(run_);
    buttons->addWidget(restore_);
    buttons->addWidget(edit_);
    buttons->addStretch();
    buttons->addWidget(delete_);
    layout->addLayout(buttons);
    connect(run_, &QPushButton::clicked, this, &MainWindow::run_job);
    connect(edit_, &QPushButton::clicked, this, &MainWindow::edit_job);
    connect(delete_, &QPushButton::clicked, this, &MainWindow::delete_job);
    connect(restore_, &QPushButton::clicked, this, [this] { open_restore(0); });

    progress_box_ = new QWidget;
    auto* pl = new QVBoxLayout(progress_box_);
    pl->setContentsMargins(0, 0, 0, 0);
    bar_ = new QProgressBar;
    bar_->setRange(0, 0);
    progress_text_ = new QLabel;
    current_file_ = new QLabel;
    current_file_->setTextFormat(Qt::PlainText);
    current_file_->setForegroundRole(QPalette::PlaceholderText);
    current_file_->setMinimumWidth(10);
    pl->addWidget(bar_);
    pl->addWidget(progress_text_);
    pl->addWidget(current_file_);
    progress_box_->hide();
    layout->addWidget(progress_box_);

    tabs_ = new QTabWidget;
    history_ = new QTreeWidget;
    history_->setRootIsDecorated(false);
    history_->setHeaderLabels({tr("Started"), tr("Result"), tr("Files"), tr("Size"), tr("New"), tr("Stored"),
                               tr("Duration"), tr("Details")});
    history_->header()->setStretchLastSection(true);
    tabs_->addTab(history_, tr("History"));

    auto* snap_page = new QWidget;
    auto* sl = new QVBoxLayout(snap_page);
    sl->setContentsMargins(0, 0, 0, 0);
    snapshots_ = new QTreeWidget;
    snapshots_->setRootIsDecorated(false);
    snapshots_->setHeaderLabels({tr("Time"), tr("ID"), tr("Files"), tr("Size"), tr("Added"), tr("Host"), tr("Paths")});
    snapshots_->header()->setStretchLastSection(true);
    snapshots_status_ = new QLabel;
    sl->addWidget(snapshots_, 1);
    sl->addWidget(snapshots_status_);
    connect(snapshots_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem* item) { open_restore(snapshots_->indexOfTopLevelItem(item)); });
    tabs_->addTab(snap_page, tr("Snapshots"));

    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(5000);
    tabs_->addTab(log_, tr("Log"));
    connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
        if (i == 1) load_snapshots(false);
    });
    layout->addWidget(tabs_, 1);
    return page;
}

void MainWindow::reload_jobs(const std::string& select_id) {
    jobs_ = store_.list();
    const QSignalBlocker block(list_);
    list_->clear();
    int select = jobs_.empty() ? -1 : 0;
    for (std::size_t i = 0; i < jobs_.size(); ++i) {
        const jobs::Job& j = jobs_[i];
        auto* item = new QListWidgetItem(qs(j.name) + "\n" + qs(jobs::describe(j.schedule)));
        const auto h = store_.history(j.id, 1);
        if (!h.empty()) item->setIcon(status_icon(style(), h.back().status));
        list_->addItem(item);
        if (j.id == select_id) select = static_cast<int>(i);
    }
    list_->setCurrentRow(select);
    show_job();
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
    for (const auto& s : job->sources) sources << qs(s).toHtmlEscaped();
    QString details = tr("<b>Backs up:</b> %1<br><b>To:</b> %2<br><b>Schedule:</b> %3<br><b>Resources:</b> %4")
                          .arg(sources.join(", "), qs(job->repository).toHtmlEscaped(),
                               qs(jobs::describe(job->schedule)), resources_text(job->resources));
    if (!job->excludes.empty()) {
        QStringList ex;
        for (const auto& e : job->excludes) ex << qs(e).toHtmlEscaped();
        details += tr("<br><b>Skips:</b> %1").arg(ex.join(", "));
    }
    details += tr("<br><b>Old backups:</b> %1")
                   .arg(job->retention.empty() ? tr("all kept")
                                               : tr("keeping %1").arg(qs(describe(job->retention))));
    details_->setText(details);
    set_running(runner_->running());
    refresh_history();
    if (tabs_->currentIndex() == 1) load_snapshots(false);
}

void MainWindow::refresh_history() {
    const jobs::Job* job = current_job();
    if (!job) return;
    const auto records = store_.history(job->id, 200);
    history_->clear();
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const jobs::RunRecord& r = *it;
        auto* item = new QTreeWidgetItem(history_);
        item->setText(0, format_time(r.started));
        item->setIcon(1, status_icon(style(), r.status));
        item->setText(1, status_text(r.status));
        item->setText(2, QString::number(r.files));
        item->setText(3, format_bytes(r.bytes));
        item->setText(4, format_bytes(r.new_bytes));
        item->setText(5, format_bytes(r.stored_bytes));
        if (r.finished > r.started) item->setText(6, format_duration(static_cast<double>(r.finished - r.started) / 1e7));
        item->setText(7, qs(r.message));
    }
    for (int c = 0; c < 7; ++c) history_->resizeColumnToContents(c);
    if (records.empty()) {
        last_run_->setText(tr("Not run yet."));
    } else {
        const jobs::RunRecord& r = records.back();
        last_run_->setText(tr("Last run: %1, %2").arg(format_time(r.started), status_text(r.status).toLower()));
    }
}

void MainWindow::load_snapshots(bool force) {
    const jobs::Job* job = current_job();
    if (!job) return;
    const std::string id = job->id;
    auto fill = [this](const std::vector<SnapshotEntry>& list) {
        snapshots_->clear();
        for (const auto& e : list) {
            const Snapshot& s = e.snapshot;
            auto* item = new QTreeWidgetItem(snapshots_);
            item->setText(0, format_time(s.time));
            item->setText(1, qs(id_hex(e.id).substr(0, 8)));
            item->setText(2, QString::number(s.stats.files));
            item->setText(3, format_bytes(s.stats.bytes));
            item->setText(4, format_bytes(s.stats.stored_bytes));
            item->setText(5, qs(s.hostname));
            QStringList paths;
            for (const auto& p : s.paths) paths << qs(p);
            item->setText(6, paths.join(", "));
        }
        for (int c = 0; c < 6; ++c) snapshots_->resizeColumnToContents(c);
        snapshots_status_->setText(list.empty() ? tr("No snapshots yet.")
                                                : tr("%1 snapshot(s). Double-click one to restore from it.")
                                                      .arg(list.size()));
    };
    if (!force) {
        auto it = repos_.find(id);
        if (it != repos_.end()) {
            fill(it->second.snapshots);
            return;
        }
    }
    if (loading_job_ == id) return;
    loading_job_ = id;
    snapshots_->clear();
    snapshots_status_->setText(tr("Opening the repository..."));
    std::string pw;
    try {
        pw = store_.password(id);
    } catch (const Error& e) {
        snapshots_status_->setText(QString::fromUtf8(e.what()));
        loading_job_.clear();
        return;
    }
    RepoHandle::open_async(this, job->repository, std::move(pw),
                           [this, id, fill](std::shared_ptr<Repository> repo, std::vector<SnapshotEntry> list,
                                            QString error) {
                               loading_job_.clear();
                               const jobs::Job* cur = current_job();
                               if (!error.isEmpty()) {
                                   if (cur && cur->id == id) snapshots_status_->setText(error);
                                   return;
                               }
                               repos_[id] = OpenRepo{std::move(repo), std::move(list)};
                               if (cur && cur->id == id) fill(repos_[id].snapshots);
                           });
}

void MainWindow::restore_from_folder() {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the backup folder"));
    if (dir.isEmpty()) return;
    if (!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("config")))) {
        QMessageBox::warning(this, windowTitle(), tr("%1 does not hold a Vaultik backup.").arg(QDir::toNativeSeparators(dir)));
        return;
    }
    bool ok = false;
    QString password = QInputDialog::getText(this, tr("Backup password"),
                                             tr("Password of the backup in %1:").arg(QDir::toNativeSeparators(dir)),
                                             QLineEdit::Password, QString(), &ok);
    if (!ok || password.isEmpty()) return;

    jobs::Job source;
    source.name = u8(QDir::toNativeSeparators(dir));
    source.repository = source.name;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    statusBar()->showMessage(tr("Opening the backup..."));
    RepoHandle::open_async(
        this, source.repository, u8(password),
        [this, source, password](std::shared_ptr<Repository> repo, std::vector<SnapshotEntry> list,
                                 QString error) mutable {
            QApplication::restoreOverrideCursor();
            statusBar()->clearMessage();
            if (!error.isEmpty()) {
                QMessageBox::warning(this, windowTitle(), error);
            } else if (list.empty()) {
                QMessageBox::information(this, windowTitle(), tr("This backup has no snapshots to restore from."));
            } else {
                RestoreDialog dialog(source, std::move(repo), std::move(list), 0, this, password);
                dialog.exec();
            }
            password.fill(QChar(0));
        });
}

void MainWindow::open_restore(int snapshot_index) {
    const jobs::Job* job = current_job();
    if (!job) return;
    auto it = repos_.find(job->id);
    if (it == repos_.end()) {
        // Open the repository first, then come back here.
        tabs_->setCurrentIndex(1);
        load_snapshots(false);
        const std::string id = job->id;
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer, id, snapshot_index] {
            if (loading_job_ == id) return;
            timer->deleteLater();
            if (repos_.contains(id) && current_job() && current_job()->id == id) open_restore(snapshot_index);
        });
        timer->start(200);
        return;
    }
    if (it->second.snapshots.empty()) {
        QMessageBox::information(this, windowTitle(), tr("This job has no snapshots to restore from yet."));
        return;
    }
    RestoreDialog dialog(*job, it->second.repo, it->second.snapshots, std::max(snapshot_index, 0), this);
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
            .arg(qs(job->name), qs(job->repository)));
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
    running_job_ = job->id;
    run_started_ = std::chrono::steady_clock::now();
    log_->appendPlainText(tr("--- %1: backup started").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm")));
    progress_text_->setText(tr("Starting..."));
    current_file_->clear();
    set_running(true);
    runner_->start({"run-job", qs(job->id)});
}

void MainWindow::set_running(bool running) {
    const jobs::Job* job = current_job();
    const bool this_job = running && job && job->id == running_job_;
    progress_box_->setVisible(this_job);
    run_->setEnabled(!running || this_job);
    run_->setText(this_job ? tr("Stop") : tr("Back up now"));
    run_->setIcon(style()->standardIcon(this_job ? QStyle::SP_MediaStop : QStyle::SP_MediaPlay));
    edit_->setEnabled(!this_job);
    delete_->setEnabled(!running);
}

void MainWindow::closeEvent(QCloseEvent* event) {
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
