#pragma once

#include <QMainWindow>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "repo_handle.h"
#include "vaultik/jobs/job.hpp"

class FindPage;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class Runner;
class VersionsView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

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
    void load_snapshots(bool force);
    void show_versions();
    void new_job();
    void edit_job();
    void delete_job();
    void run_job();
    void open_restore(int snapshot_index, const QString& select_path = {});
    void restore_from_folder();
    void open_keys();
    void set_running(bool running);
    // Pin, note and delete on the open repository of the current job.
    void change_version(int index, bool pin, bool note);
    void delete_version(int index);

    vaultik::jobs::JobStore store_;
    std::vector<vaultik::jobs::Job> jobs_;

    // Sidebar
    QListWidget* list_ = nullptr;
    QPushButton* new_job_ = nullptr;
    QPushButton* open_backup_ = nullptr;
    QPushButton* keys_ = nullptr;

    // Job page
    QStackedWidget* stack_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* subtitle_ = nullptr;
    QLabel* status_icon_ = nullptr;
    QLabel* status_text_ = nullptr;
    QPushButton* run_ = nullptr;
    QPushButton* restore_ = nullptr;
    QToolButton* more_ = nullptr;
    QWidget* progress_box_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* progress_text_ = nullptr;
    QLabel* current_file_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    VersionsView* versions_ = nullptr;
    FindPage* find_ = nullptr;
    QTreeWidget* history_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    // Empty page
    QLabel* empty_icon_ = nullptr;
    QPushButton* empty_create_ = nullptr;
    QPushButton* empty_restore_ = nullptr;

    Runner* runner_ = nullptr;
    std::string running_job_;
    std::chrono::steady_clock::time_point run_started_{};

    // Opened repositories per job id, with the job's versions (newest first).
    struct OpenRepo {
        std::shared_ptr<vaultik::Repository> repo;
        std::vector<SnapshotEntry> snapshots;
    };
    std::map<std::string, OpenRepo> repos_;
    std::string loading_job_;
};
