#pragma once

#include <QDialog>

#include "vaultik/jobs/job.hpp"

class QCheckBox;
class QComboBox;
class QGroupBox;
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

    const vaultik::jobs::Job& job() const { return job_; }

    void accept() override;

private:
    QWidget* build_general();
    QWidget* build_sources();
    QWidget* build_schedule();
    QWidget* build_resources();
    QWidget* build_retention();
    void load(const vaultik::jobs::Job& job);
    vaultik::jobs::Job collect() const;
    void apply_preset(int index);
    void update_schedule_fields();
    bool prepare_repository(const vaultik::jobs::Job& job, const std::string& password);

    const vaultik::jobs::JobStore& store_;
    bool editing_ = false;
    vaultik::jobs::Job job_;

    QLineEdit* name_ = nullptr;
    QLineEdit* repository_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* password2_ = nullptr;
    QListWidget* sources_ = nullptr;
    QListWidget* excludes_ = nullptr;

    QComboBox* schedule_kind_ = nullptr;
    QTimeEdit* time_ = nullptr;
    QSpinBox* every_hours_ = nullptr;
    QSpinBox* minute_ = nullptr;
    QCheckBox* days_[7] = {};
    QWidget* time_row_ = nullptr;
    QWidget* hourly_row_ = nullptr;
    QWidget* days_row_ = nullptr;

    QComboBox* preset_ = nullptr;
    QComboBox* cpu_ = nullptr;
    QComboBox* io_ = nullptr;
    QComboBox* ram_ = nullptr;
    QSpinBox* cpu_limit_ = nullptr;
    QSpinBox* max_memory_ = nullptr;
    QSpinBox* read_limit_ = nullptr;
    QSpinBox* write_limit_ = nullptr;

    QGroupBox* retention_ = nullptr;  // checkable: off keeps every backup
    QSpinBox* keep_last_ = nullptr;
    QSpinBox* keep_hourly_ = nullptr;
    QSpinBox* keep_daily_ = nullptr;
    QSpinBox* keep_weekly_ = nullptr;
    QSpinBox* keep_monthly_ = nullptr;
    QSpinBox* keep_yearly_ = nullptr;
};
