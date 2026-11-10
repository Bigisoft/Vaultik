#pragma once

#include <QDialog>

class QCheckBox;
class QComboBox;
class QSpinBox;

// Vaultik's own preferences: starting with Windows, the notification area icon and how the window
// hides into it, and which notifications to show.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    void accept() override;

private:
    void update_enabled();

    QCheckBox* start_ = nullptr;
    QCheckBox* start_hidden_ = nullptr;
    QCheckBox* admin_ = nullptr;
    bool admin_was_ = false;
    QCheckBox* tray_icon_ = nullptr;
    QCheckBox* tray_status_ = nullptr;
    QCheckBox* close_to_tray_ = nullptr;
    QCheckBox* minimize_to_tray_ = nullptr;
    QCheckBox* single_click_ = nullptr;
    QCheckBox* failures_ = nullptr;
    QCheckBox* overdue_ = nullptr;
    QSpinBox* remind_ = nullptr;
    QCheckBox* hello_ = nullptr;
    QComboBox* language_ = nullptr;
};
