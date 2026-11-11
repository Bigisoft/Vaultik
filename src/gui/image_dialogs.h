#pragma once

#include <QDialog>

#include <memory>
#include <vector>

#include "repo_handle.h"
#include "vaultik/image/disk.hpp"
#include "vaultik/jobs/job.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class Runner;

// Restores a disk image version: to a new virtual disk file (no administrator rights; File
// Explorer opens it), or onto a disk of this computer (never the one Windows runs from nor the one
// holding the backup; the user types ERASE DISK <n> first), which runs with administrator rights
// in a window of its own. Also opens an image's drives read-only for browsing.
class ImageRestoreDialog : public QDialog {
    Q_OBJECT

public:
    ImageRestoreDialog(const vaultik::jobs::Job& job, std::shared_ptr<vaultik::Repository> repo,
                       std::vector<SnapshotEntry> images, int selected, QWidget* parent = nullptr);

    void reject() override;

private:
    void show_version(int index);
    void fill_targets();
    void update_state();
    void start();
    void browse_files();
    const vaultik::image::DiskLayout* layout() const;
    int target_disk() const;

    vaultik::jobs::Job job_;
    std::shared_ptr<vaultik::Repository> repo_;
    std::vector<SnapshotEntry> images_;
    std::vector<vaultik::image::DiskLayout> layouts_;  // of the version shown
    std::vector<vaultik::image::DiskInfo> disks_;

    QComboBox* version_ = nullptr;
    QComboBox* disk_ = nullptr;
    QLabel* parts_ = nullptr;
    QRadioButton* to_file_ = nullptr;
    QRadioButton* to_disk_ = nullptr;
    QLineEdit* file_ = nullptr;
    QComboBox* target_ = nullptr;
    QLabel* warning_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QCheckBox* boot_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* browse_files_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    Runner* runner_ = nullptr;
};

// "Create recovery drive": Windows PE with Vaultik on a USB stick or as an ISO file, built with
// administrator rights in a window of its own (see image/recovery_media.hpp).
class RecoveryDriveDialog : public QDialog {
    Q_OBJECT

public:
    explicit RecoveryDriveDialog(const std::vector<vaultik::jobs::Job>& jobs, QWidget* parent = nullptr);

private:
    void fill_drives();
    void update_state();
    void start();

    std::vector<std::string> backups_;  // the jobs' destinations, never formatted
    QRadioButton* to_iso_ = nullptr;
    QRadioButton* to_usb_ = nullptr;
    QLineEdit* iso_ = nullptr;
    QComboBox* usb_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QLabel* adk_ = nullptr;
    QPushButton* create_ = nullptr;
    bool adk_found_ = false;
};
