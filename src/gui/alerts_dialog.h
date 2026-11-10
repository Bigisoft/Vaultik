#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;

// Alerts for when nobody is at the computer (see vaultik/jobs/alerts.hpp): where they go and for
// what. The tokens are stored sealed for this account; a field left empty keeps a stored token.
class AlertsDialog : public QDialog {
    Q_OBJECT
public:
    explicit AlertsDialog(QWidget* parent = nullptr);
    void accept() override;

private:
    bool save();  // false (with a message) when a field is not valid
    void send_test();

    QLineEdit* ntfy_url_ = nullptr;
    QLineEdit* ntfy_token_ = nullptr;
    QLineEdit* ntfy_email_ = nullptr;
    QLineEdit* telegram_token_ = nullptr;
    QLineEdit* telegram_chat_ = nullptr;
    QLineEdit* webhook_ = nullptr;
    QCheckBox* on_failure_ = nullptr;
    QCheckBox* on_damage_ = nullptr;
    QCheckBox* on_success_ = nullptr;
};
