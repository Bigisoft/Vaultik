#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "vaultik/jobs/job.hpp"

class QProcess;
class QTimer;
class QWidget;
class FolderWatch;

// Backups the tray starts on its own, each as a separate, windowless engine process:
// - continuous backups: a job's folders are watched, and a run starts `continuous_minutes` after
//   the first change since the last one (changes in excluded folders, the backup's own folder and
//   Vaultik's settings do not count);
// - at sign-out or shutdown (Windows): jobs that back up then hold Windows up, with a reason it
//   shows, until their runs end; then sign-out or shutdown goes on (the user can also go on
//   without waiting). The runs themselves skip a job backed up shortly before (see
//   jobs::event_backup_due);
// - when another computer sharing a backup asked for one (jobs::BackupRequest): every 15 minutes
//   the repositories of the jobs on folders, drives and network shares are looked at, and every
//   hour those in cloud storage too (each look there costs a few billed requests and, for the
//   cloud drives, counts against their rate limits; not over a metered connection). A look opens
//   the repository without its index and reads one small file.
class EventRuns : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    // `window` is the top-level window Windows asks before the session ends.
    EventRuns(QWidget* window, QObject* parent = nullptr);
    ~EventRuns() override;

    // The saved jobs changed: watch the folders of the continuous ones.
    void set_jobs(const std::vector<vaultik::jobs::Job>& jobs);

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

    // Looks for requests from other computers now (also done on a timer).
    void poll_requests();

signals:
    // Something to tell the user (a notification).
    void notify(const QString& title, const QString& text, bool warning);
    // A run started here ended (its result is in the job's history).
    void run_finished(const QString& job_id);

private:
    std::vector<vaultik::jobs::Job> jobs_;  // all saved jobs, for requests
    QTimer* requests_ = nullptr;
    int request_ticks_ = 0;
    QProcess* request_poll_ = nullptr;

    struct Run {
        std::string job;
        std::uint32_t pid = 0;
        bool sign_out = false;
    };
    struct Watched {
        vaultik::jobs::Job job;
        std::vector<std::unique_ptr<FolderWatch>> watches;
        QTimer* timer = nullptr;  // the wait after the first change
        bool changed_while_running = false;
    };

    void changed(const std::string& job_id);
    // (Re)makes the watches of a job's folders.
    void watch(Watched& w);
    void rewatch();
    bool start(const vaultik::jobs::Job& job, const QString& flag, bool sign_out);
    bool running(const std::string& job_id) const;
    void poll();
    bool session_ending(std::intptr_t flags);

    QWidget* window_;
    std::map<std::string, Watched> watched_;
    std::vector<Run> runs_;
    QTimer* poll_ = nullptr;
    bool blocking_ = false;  // holding up the end of the session
    QTimer* rewatch_ = nullptr;
};
