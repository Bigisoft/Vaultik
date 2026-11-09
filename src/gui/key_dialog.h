#pragma once

#include <QDialog>

#include "vaultik/jobs/job.hpp"

class QLineEdit;
class QRadioButton;

// Shows a new recovery key once, with buttons to copy it or save it to a file.
void show_recovery_key(QWidget* parent, const QString& key, const QString& location);

// Changes a backup's password or makes a new recovery key. Unlocks with the current password or
// the recovery key; saved jobs using the backup get the new password.
class KeyDialog : public QDialog {
    Q_OBJECT

public:
    KeyDialog(const vaultik::jobs::JobStore& store, const QString& location, QWidget* parent = nullptr);
    ~KeyDialog() override;

private:
    void change_password();
    void make_recovery_key();
    bool check_location();

    const vaultik::jobs::JobStore& store_;
    QLineEdit* location_ = nullptr;
    QRadioButton* use_password_ = nullptr;
    QRadioButton* use_recovery_ = nullptr;
    QLineEdit* secret_ = nullptr;
    QLineEdit* new_password_ = nullptr;
    QLineEdit* new_password2_ = nullptr;
};
