#pragma once

#include <QDialog>

#include <map>

#include "cloud_dialog.h"
#include "vaultik/jobs/job.hpp"

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QTimeEdit;

// Creates or edits a backup job. On accept it creates the repository if needed (or checks the
// password against an existing one), saves the job and its password, and updates the schedule.
class JobDialog : public QDialog {
    Q_OBJECT

public:
    JobDialog(const vaultik::jobs::JobStore& store, const vaultik::jobs::Job* existing, QWidget* parent = nullptr);
    ~JobDialog() override;

    const vaultik::jobs::Job& job() const { return job_; }

    void accept() override;

private:
    QWidget* build_general();
    QWidget* build_sources();
    // Windows: the disks a disk image job backs up, instead of folders.
    QWidget* build_disks();
    void fill_disks(const std::vector<std::string>& checked);
    void update_kind();
    QWidget* build_schedule();
    QWidget* build_resources();
    QWidget* build_retention();
    QWidget* build_machines();
    QWidget* build_containers();
    void load(const vaultik::jobs::Job& job);
    vaultik::jobs::Job collect() const;
    void apply_preset(int index);
    void update_schedule_fields();
    vaultik::RetentionPolicy collect_retention() const;
    void update_retention_fields();
    bool prepare_repository(const vaultik::jobs::Job& job, const std::string& password);

    const vaultik::jobs::JobStore& store_;
    bool missing_password_ = false;  // editing a job with no password saved here (imported without it)
    bool editing_ = false;
    vaultik::jobs::Job job_;
    CloudTarget cloud_;  // the cloud destination's keys, when the destination is cloud storage

    QLineEdit* name_ = nullptr;
    QLineEdit* repository_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* password2_ = nullptr;
    QLineEdit* copy_repository_ = nullptr;  // the second copy (empty: none)
    CloudTarget copy_cloud_;
    QListWidget* sources_ = nullptr;
    QComboBox* kind_ = nullptr;      // folders and files, or whole disks (Windows)
    QWidget* files_box_ = nullptr;
    QWidget* disks_box_ = nullptr;
    QListWidget* disks_ = nullptr;   // checkable; data: the disk's id ("system" for Windows' own)
    QListWidget* excludes_ = nullptr;
    std::vector<QCheckBox*> preset_boxes_;  // in the order of jobs::exclusion_presets()
    QLabel* estimate_ = nullptr;
    // Counts what the sources hold (with the exclusions) and shows a rough first-backup time.
    void estimate_size();
    QCheckBox* vss_ = nullptr;    // Windows only
    QCheckBox* skip_sql_ = nullptr;  // Windows only
    QCheckBox* admin_ = nullptr;  // Windows only

    // Virtual machines (Windows): WSL distributions and Hyper-V machines, each a checkable item.
    QListWidget* wsl_list_ = nullptr;
    QComboBox* wsl_running_ = nullptr;
    QListWidget* hyperv_list_ = nullptr;
    QLabel* hyperv_note_ = nullptr;
    QLineEdit* hyperv_staging_ = nullptr;
    bool hyperv_listed_ = false;
    // Asks Hyper-V for its machines (once, when the tab is first shown, or again on request).
    void list_hyperv();
    void add_machine_item(QListWidget* list, const QString& name, const QString& detail, bool checked);

    // Docker volumes (checkable items) and database dumps.
    QListWidget* docker_list_ = nullptr;
    QLabel* docker_note_ = nullptr;
    QLineEdit* docker_image_ = nullptr;
    bool docker_listed_ = false;
    // Asks Docker for its volumes (once, when the tab is first shown, or again on request).
    void list_docker();
    QListWidget* db_list_ = nullptr;
    std::vector<vaultik::DatabaseDump> databases_;
    // Passwords typed for dumps, by dump id, stored when the job is saved (empty: remove it).
    std::map<std::string, QString> db_passwords_;
    void fill_databases();
    // Adds a dump (index -1) or edits one.
    void edit_database(int index);

    QComboBox* schedule_kind_ = nullptr;
    QTimeEdit* time_ = nullptr;
    QSpinBox* every_hours_ = nullptr;
    QSpinBox* minute_ = nullptr;
    QCheckBox* days_[7] = {};
    QWidget* time_row_ = nullptr;
    QWidget* hourly_row_ = nullptr;
    QWidget* days_row_ = nullptr;
    QCheckBox* paused_ = nullptr;
    QCheckBox* on_connect_ = nullptr;
    QLabel* on_connect_note_ = nullptr;
    QCheckBox* on_idle_ = nullptr;
    QCheckBox* on_lock_ = nullptr;
    QCheckBox* on_sign_out_ = nullptr;
    QCheckBox* on_update_ = nullptr;
    QSpinBox* event_gap_ = nullptr;
    QCheckBox* continuous_ = nullptr;
    QSpinBox* continuous_minutes_ = nullptr;
    QLabel* events_note_ = nullptr;
    void update_on_connect();
    // Health checks after scheduled backups.
    QCheckBox* check_weekly_ = nullptr;
    QSpinBox* check_percent_ = nullptr;
    QCheckBox* check_monthly_ = nullptr;
    QCheckBox* check_restore_ = nullptr;
    QCheckBox* check_rot_ = nullptr;
    QCheckBox* skip_battery_ = nullptr;
    QCheckBox* skip_unchanged_ = nullptr;
    QCheckBox* check_every_file_ = nullptr;
    QCheckBox* record_apps_ = nullptr;
    QComboBox* repair_data_ = nullptr;
    QCheckBox* wake_ = nullptr;
    QCheckBox* skip_metered_ = nullptr;
    QLineEdit* before_command_ = nullptr;
    QLineEdit* after_command_ = nullptr;
    bool check_monthly_touched_ = false;  // otherwise a new job follows its destination's default

    QComboBox* preset_ = nullptr;
    QComboBox* cpu_ = nullptr;
    QComboBox* io_ = nullptr;
    QComboBox* ram_ = nullptr;
    QSpinBox* cpu_limit_ = nullptr;
    QSpinBox* max_memory_ = nullptr;
    QSpinBox* read_limit_ = nullptr;
    QSpinBox* write_limit_ = nullptr;
    QGroupBox* work_box_ = nullptr;  // working hours (checkable)
    QTimeEdit* work_start_ = nullptr;
    QTimeEdit* work_end_ = nullptr;
    QSpinBox* work_read_ = nullptr;
    QSpinBox* work_write_ = nullptr;

    // Old backups: a preset, its one setting, the custom rules, and a size limit.
    QComboBox* keep_mode_ = nullptr;
    QStackedWidget* keep_pages_ = nullptr;
    QSpinBox* keep_count_ = nullptr;
    QSpinBox* keep_days_ = nullptr;
    QCheckBox* size_limit_ = nullptr;
    QSpinBox* size_limit_gb_ = nullptr;
    QLabel* keep_summary_ = nullptr;
    QSpinBox* keep_within_ = nullptr;
    QSpinBox* keep_last_ = nullptr;
    QSpinBox* keep_hourly_ = nullptr;
    QSpinBox* keep_daily_ = nullptr;
    QSpinBox* keep_weekly_ = nullptr;
    QSpinBox* keep_monthly_ = nullptr;
    QSpinBox* keep_yearly_ = nullptr;
};
