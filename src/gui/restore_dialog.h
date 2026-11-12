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
    // The old account's folders in this version, each into the same kind of folder here.
    void move_here();
    // Archive storage (S3 GLACIER, DEEP_ARCHIVE): asks for a thaw of what restoring the selected
    // item (or the whole version) needs, through the engine's cloud-thaw; asking again reports
    // how far it is.
    void request_thaw();
    void show_archived_note();
    // The engine command line for this job or repository: "--job <id>" or "--repo <location>".
    QStringList repo_args() const;
    // The programs of this version that are missing here, to install again with winget.
    void reinstall_programs();
    // A WSL distribution (WSL/<name>.tar) or Hyper-V machine (Hyper-V/<name>) of this version,
    // imported as a new one: never over an existing one unless the user confirms it.
    void import_machine();
    // After a machine's files were restored: imports them, with administrator approval.
    void finish_hyperv_import();
    // A Docker volume (Docker/<name>.tar) of this version, imported into a new volume.
    void import_volume(const QString& path, const QString& volume);

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
    QPushButton* move_ = nullptr;
    QPushButton* thaw_ = nullptr;
    bool thaw_running_ = false;  // the engine runs cloud-thaw, not a restore
    QPushButton* apps_ = nullptr;
    QPushButton* import_ = nullptr;
    enum class Importing { None, Wsl, HyperV, Docker };
    Importing importing_ = Importing::None;  // what the engine runs, when not a plain restore
    QString import_name_;
    QString import_folder_;
    QString import_source_;  // Hyper-V: the machine's name in the version
    Runner* runner_ = nullptr;
    int warnings_ = 0;
};
