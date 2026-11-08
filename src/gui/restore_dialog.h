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
class Runner;

// Browse a snapshot and restore a file, a folder or everything, to the original location or to
// another folder. The restore itself runs in the engine process at normal priority without the
// job's backup limits: when files are needed back, speed matters most.
class RestoreDialog : public QDialog {
    Q_OBJECT

public:
    RestoreDialog(const vaultik::jobs::Job& job, std::shared_ptr<vaultik::Repository> repo,
                  std::vector<SnapshotEntry> snapshots, int selected, QWidget* parent = nullptr);

    void reject() override;

private:
    void show_snapshot(int index);
    void expand(QTreeWidgetItem* item);
    void update_target_state();
    void start_restore();
    QString selected_path() const;

    vaultik::jobs::Job job_;
    std::shared_ptr<vaultik::Repository> repo_;
    std::vector<SnapshotEntry> snapshots_;

    QComboBox* snapshot_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QRadioButton* to_original_ = nullptr;
    QRadioButton* to_folder_ = nullptr;
    QLineEdit* target_ = nullptr;
    QPushButton* browse_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* restore_ = nullptr;
    Runner* runner_ = nullptr;
    int warnings_ = 0;
};
