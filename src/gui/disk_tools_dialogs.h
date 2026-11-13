#pragma once

#include <QDialog>

#include <vector>

#include "vaultik/image/disk.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class Runner;

// "Set up a new disk": a blank or unwanted disk gets a new partition table (GPT or MBR), one
// partition over the whole disk, a file system with a name, and a drive letter (see
// image/disk_tools.hpp). Never the disk the system or Vaultik runs from; the user types
// ERASE DISK <n> first. Runs with administrator rights in a window of its own.
class NewDiskDialog : public QDialog {
    Q_OBJECT

public:
    explicit NewDiskDialog(QWidget* parent = nullptr);

private:
    void fill_disks();
    void update_state();
    void start();
    const vaultik::image::DiskInfo* chosen() const;

    std::vector<vaultik::image::DiskInfo> disks_;
    QComboBox* disk_ = nullptr;
    QComboBox* style_ = nullptr;
    QComboBox* fs_ = nullptr;
    QLineEdit* label_ = nullptr;
    QComboBox* letter_ = nullptr;
    QLabel* warning_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QPushButton* start_ = nullptr;
    QLabel* status_ = nullptr;
};

// "Drive cleanser": erases a whole disk or one volume so its data cannot be read back (one pass
// of zeros, one of random data, or three passes), or overwrites only the free space of a drive.
// Erasing runs with administrator rights in a window of its own, after the typed confirmation;
// the free space is filled here, with progress, and can be cancelled.
class DriveCleanserDialog : public QDialog {
    Q_OBJECT

public:
    explicit DriveCleanserDialog(QWidget* parent = nullptr);

    void reject() override;

private:
    void fill_targets();
    void update_state();
    void start();
    const vaultik::image::DiskInfo* chosen_disk() const;
    QString confirmation() const;

    std::vector<vaultik::image::DiskInfo> disks_;
    QRadioButton* whole_disk_ = nullptr;
    QRadioButton* volume_ = nullptr;
    QRadioButton* free_space_ = nullptr;
    QComboBox* disk_ = nullptr;
    QComboBox* volumes_ = nullptr;
    QLineEdit* folder_ = nullptr;
    QComboBox* method_ = nullptr;
    QCheckBox* verify_ = nullptr;
    QLabel* ssd_note_ = nullptr;
    QLabel* warning_ = nullptr;
    QLineEdit* confirm_ = nullptr;
    QPushButton* start_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    Runner* runner_ = nullptr;
};
