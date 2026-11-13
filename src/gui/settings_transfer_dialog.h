#pragma once

#include <QDialog>

#include <memory>

#include "vaultik/jobs/settings_transfer.hpp"

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

// "Export settings and plans": the frontend's settings, the alert settings and every backup job
// in one file; passwords and cloud keys only when asked for, and then the file is encrypted with
// a password typed here.
class ExportSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExportSettingsDialog(const vaultik::jobs::JobStore& store, QWidget* parent = nullptr);

    void accept() override;

private:
    void update_state();

    const vaultik::jobs::JobStore& store_;
    QLineEdit* file_ = nullptr;
    QCheckBox* secrets_ = nullptr;
    QLineEdit* password_ = nullptr;
    QLineEdit* password2_ = nullptr;
    QPushButton* ok_ = nullptr;
};

// "Import settings and plans": the jobs in a file, to choose from; a job whose id is taken here is
// imported under a new id or replaces the job here. Imported jobs keep their schedule but start
// paused, so nothing runs before the user checked their folders on this computer.
class ImportSettingsDialog : public QDialog {
    Q_OBJECT

public:
    ImportSettingsDialog(const vaultik::jobs::JobStore& store, std::shared_ptr<vaultik::jobs::SettingsBundle> bundle,
                         QWidget* parent = nullptr);

    void accept() override;
    const vaultik::jobs::ImportResult& result() const { return result_; }

    // Asks for the file (and its password when it is encrypted) and reads it; null when the user
    // gave up.
    static std::shared_ptr<vaultik::jobs::SettingsBundle> open_file(QWidget* parent);

private:
    const vaultik::jobs::JobStore& store_;
    std::shared_ptr<vaultik::jobs::SettingsBundle> bundle_;
    QListWidget* jobs_ = nullptr;
    QRadioButton* rename_ = nullptr;
    QRadioButton* replace_ = nullptr;
    QCheckBox* app_ = nullptr;
    QCheckBox* alerts_ = nullptr;
    vaultik::jobs::ImportResult result_;
};
