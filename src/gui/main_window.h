#pragma once

#include <QMainWindow>
#include <QStringList>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "repo_handle.h"
#include "vaultik/jobs/drive_history.hpp"
#include "vaultik/jobs/job.hpp"
#include "vaultik/platform/drive_health.hpp"

class EventRuns;
class FindPage;
class QAction;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QProgressBar;
class QProcess;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class Runner;
class Tray;
class VersionsView;
class OverviewView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // With `in_tray`, the window starts hidden behind the tray icon (Vaultik started at sign-in).
    explicit MainWindow(bool in_tray = false);

    // Shows the window in front (from the tray, or when Vaultik is started a second time).
    void bring_to_front();
    // Hides the window to the tray, or shows it (a click on the tray icon).
    void toggle_visible();
    // Selects the job and starts its backup (from the tray menu).
    void run_job_now(const std::string& id);
    // "Earlier versions in Vaultik" from File Explorer: the history of a file or folder on this
    // computer, in the job that backs it up.
    void show_history_for(const QString& local_path);
    void open_settings();
    // The monthly report of `month` ("YYYY-MM"; empty: last month).
    void open_report(const std::string& month = {});
    // Ends Vaultik, also when it lives in the tray.
    void quit_app();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QWidget* build_sidebar();
    QWidget* build_job_page();
    QWidget* build_empty_page();
    void refresh_icons();
    void reload_jobs(const std::string& select_id = {});
    void fill_card(QListWidgetItem* item, const vaultik::jobs::Job& job);
    void refresh_sidebar();
    const vaultik::jobs::Job* current_job() const;
    void show_job();
    void refresh_history();
    // The start page, from the job's history and schedule and, once open, its repository.
    void refresh_overview();
    void load_snapshots(bool force);
    void show_versions();
    void new_job();
    void edit_job();
    void delete_job();
    // Pauses or resumes the selected job's schedule (saved like an edit of the job).
    void set_job_paused(bool paused);
    void run_job();
    // A health check of the current job now: its weekly share of the data, or all of it.
    void check_job(bool full);
    void test_restore();
    // Which folders added the most new data (GrewDialog), for the current job.
    void show_grew();
    void repair_backup();
    void start_run(const QStringList& args, bool check);
    void open_restore(int snapshot_index, const QString& select_path = {});
    void restore_from_folder();
    void open_keys();
    // "All computers": every computer backing up into the destinations of the user's jobs.
    void show_computers();
    void set_running(bool running);
    // Pin, note and delete on the open repository of the current job.
    void change_version(int index, bool pin, bool note);
    void delete_version(int index);
    // Creates or removes the tray icon to match the settings.
    void update_tray();
    // Once a month: a notification leading to last month's report, and its summary to the alert
    // channels when they ask for one (by "vaultik report --deliver").
    void check_monthly();
    std::string monthly_done_;  // the month whose report was looked at in this process

    vaultik::jobs::JobStore store_;
    std::vector<vaultik::jobs::Job> jobs_;

    // Sidebar
    QListWidget* list_ = nullptr;
    QPushButton* new_job_ = nullptr;
    QPushButton* open_backup_ = nullptr;
    QPushButton* keys_ = nullptr;
    QPushButton* computers_ = nullptr;
    QPushButton* settings_ = nullptr;
    QPushButton* report_ = nullptr;

    Tray* tray_ = nullptr;  // child of this window, while the tray is on
    bool started_in_tray_ = false;
    bool quitting_ = false;

    // Job page
    QStackedWidget* stack_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* status_icon_ = nullptr;
    QLabel* status_text_ = nullptr;
    QPushButton* run_ = nullptr;
    QPushButton* restore_ = nullptr;
    QToolButton* more_ = nullptr;
    QAction* pause_job_ = nullptr;  // "Pause schedule" or "Resume schedule" in the More menu
    QWidget* progress_box_ = nullptr;
    QLabel* running_label_ = nullptr;
    QPushButton* pause_ = nullptr;
    bool paused_ = false;
    QProgressBar* bar_ = nullptr;
    QLabel* progress_text_ = nullptr;
    QLabel* current_file_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    OverviewView* overview_ = nullptr;
    VersionsView* versions_ = nullptr;
    FindPage* find_ = nullptr;
    QTreeWidget* history_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    // Empty page
    QLabel* empty_icon_ = nullptr;
    QPushButton* empty_create_ = nullptr;
    QPushButton* empty_restore_ = nullptr;

    // Backups started because their drive was connected: separate processes, watched until they
    // end so the user hears when the drive can be unplugged.
    struct ConnectRun {
        std::string job;
        std::uint32_t pid = 0;
        QString drive;
        std::uint64_t started = 0;
    };
    void drives_connected(const QStringList& roots);
    void watch_connect_runs();
    std::vector<ConnectRun> connect_runs_;
    QTimer* connect_timer_ = nullptr;
    EventRuns* events_ = nullptr;

    // Versions open in File Explorer: one "vaultik mount" process each, closed on request or quit.
    struct Mount {
        QProcess* process = nullptr;
        std::string job;
        std::string snapshot;  // id (hex)
        QString folder;        // empty until it is ready
    };
    // With "Ask for Windows Hello" on: asks for it unless already unlocked; false when not verified.
    bool unlock();
    bool locked() const;
    // Hidden again: forgets the opened backups and closes Explorer views, so the next look asks again.
    void relock();
    bool unlocked_ = false;
    int lock_generation_ = 0;  // counts relocks, to drop backups opened before one
    void explore_version(int index);
    void compare_versions(int a, int b);
    // Makes sure the job's password is saved (on another computer a portable copy cannot read the
    // one saved here); asks for it when not. False when the user cancelled.
    bool ensure_password(const vaultik::jobs::Job& job);
    void close_mounts();
    void update_mounts_button();
    std::vector<Mount> mounts_;

    // The drives of a job (its destination, second copy and sources) with their health, looked
    // up at most every 10 minutes per job (it takes a moment).
    enum class DriveRole { Backup, Copy, Source };
    struct JobDrive {
        DriveRole role;
        vaultik::os::DriveHealth health;
        std::vector<vaultik::jobs::DriveChange> worse;  // how it got worse lately (stored readings)
    };
    struct DriveCache {
        std::chrono::steady_clock::time_point when{};
        std::vector<JobDrive> drives;
    };
    const std::vector<JobDrive>& drives_of(const vaultik::jobs::Job& job);
    std::map<std::string, DriveCache> drive_cache_;
    QPushButton* mounts_button_ = nullptr;
    class QStatusBar* status_ = nullptr;  // status messages, under the content

    Runner* runner_ = nullptr;
    std::string running_job_;
    std::chrono::steady_clock::time_point run_started_{};
    bool check_run_ = false;  // the running job is a health check on its own (check-job)
    bool test_run_ = false;   // ... or a test restore on its own (test-restore)
    bool repair_run_ = false; // ... or a repair (repair --job)
    QString check_result_;    // what the health check after a backup found

    // Opened repositories per job id, with the job's versions (newest first).
    struct OpenRepo {
        std::shared_ptr<vaultik::Repository> repo;
        std::vector<SnapshotEntry> snapshots;
    };
    std::map<std::string, OpenRepo> repos_;
    std::string loading_job_;
};
