#pragma once

#include <QDialog>

#include <memory>
#include <vector>

#include "repo_handle.h"
#include "vaultik/jobs/job.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTreeWidget;
class QTreeWidgetItem;
class PreviewWidget;
class Runner;

// Browse a snapshot and restore a file, a folder or everything, to the original location or to
// another folder. The restore itself runs in the engine process at normal priority without the
// job's backup limits: when files are needed back, speed matters most.
class RestoreDialog : public QDialog {
    Q_OBJECT

public:
    // A `job` without an id stands for a repository opened by location (job.repository),
    // `password` and, for cloud storage, `cloud` keys, rather than a saved job.
    // `select_path`, when given, opens the folders down to that item and selects it.
    RestoreDialog(const vaultik::jobs::Job& job, std::shared_ptr<vaultik::Repository> repo,
                  std::vector<SnapshotEntry> snapshots, int selected, QWidget* parent = nullptr,
                  QString password = {}, vaultik::CloudCredentials cloud = {}, const QString& select_path = {});
    ~RestoreDialog() override;

    void reject() override;

private:
    void show_snapshot(int index);
    void expand(QTreeWidgetItem* item);
    void update_target_state();
    void start_restore();
    QString selected_path() const;
    void select_path(const QString& path);
    void preview_selected();
    void show_versions_of_selected();

    vaultik::jobs::Job job_;
    std::shared_ptr<vaultik::Repository> repo_;
    std::vector<SnapshotEntry> snapshots_;
    QString password_;
    vaultik::CloudCredentials cloud_;

    QComboBox* snapshot_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    PreviewWidget* preview_ = nullptr;
    QRadioButton* to_original_ = nullptr;
    QRadioButton* to_folder_ = nullptr;
    QLineEdit* target_ = nullptr;
    QPushButton* browse_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
    QCheckBox* permissions_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* restore_ = nullptr;
    Runner* runner_ = nullptr;
    int warnings_ = 0;
};
