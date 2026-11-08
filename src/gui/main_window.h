#pragma once

#include <QMainWindow>

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "repo_handle.h"
#include "vaultik/jobs/job.hpp"

class QAction;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTreeWidget;
class Runner;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    QWidget* build_job_page();
    QWidget* build_empty_page();
    void reload_jobs(const std::string& select_id = {});
    const vaultik::jobs::Job* current_job() const;
    void show_job();
    void refresh_history();
    void load_snapshots(bool force);
    void new_job();
    void edit_job();
    void delete_job();
    void run_job();
    void open_restore(int snapshot_index);
    void set_running(bool running);

    vaultik::jobs::JobStore store_;
    std::vector<vaultik::jobs::Job> jobs_;

    QListWidget* list_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* details_ = nullptr;
    QLabel* last_run_ = nullptr;
    QPushButton* run_ = nullptr;
    QPushButton* edit_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* delete_ = nullptr;
    QWidget* progress_box_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* progress_text_ = nullptr;
    QLabel* current_file_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QTreeWidget* history_ = nullptr;
    QTreeWidget* snapshots_ = nullptr;
    QLabel* snapshots_status_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    Runner* runner_ = nullptr;
    std::string running_job_;
    std::chrono::steady_clock::time_point run_started_{};

    // Opened repositories per job id, with their snapshot lists (newest first).
    struct OpenRepo {
        std::shared_ptr<vaultik::Repository> repo;
        std::vector<SnapshotEntry> snapshots;
    };
    std::map<std::string, OpenRepo> repos_;
    std::string loading_job_;
};
