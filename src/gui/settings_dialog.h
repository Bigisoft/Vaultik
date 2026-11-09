#pragma once

#include <QDialog>

class QCheckBox;
class QSpinBox;

// Vaultik's own preferences: starting in the tray, and which notifications to show.
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    void accept() override;

private:
    QCheckBox* start_ = nullptr;
    QCheckBox* failures_ = nullptr;
    QCheckBox* overdue_ = nullptr;
    QSpinBox* remind_ = nullptr;
};
