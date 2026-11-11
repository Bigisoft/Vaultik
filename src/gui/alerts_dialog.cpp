#include "alerts_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "common.h"
#include "vaultik/jobs/alerts.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;

namespace {

QLabel* note(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

}  // namespace

AlertsDialog::AlertsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Alerts when you are away"));
    const jobs::AlertSettings s = jobs::load_alerts();

    ntfy_url_ = new QLineEdit(qs(s.ntfy_url));
    ntfy_url_->setPlaceholderText(tr("e.g. https://ntfy.sh/a-long-name-nobody-guesses"));
    ntfy_token_ = new QLineEdit;
    ntfy_token_->setEchoMode(QLineEdit::Password);
    ntfy_token_->setPlaceholderText(jobs::alert_secret("ntfy").empty() ? tr("Optional, for a protected topic")
                                                                       : tr("Stored; type to replace"));
    ntfy_email_ = new QLineEdit(qs(s.ntfy_email));
    ntfy_email_->setPlaceholderText(tr("Optional: ntfy also sends the alert to this email address"));
    telegram_token_ = new QLineEdit;
    telegram_token_->setEchoMode(QLineEdit::Password);
    telegram_token_->setPlaceholderText(jobs::alert_secret("telegram").empty()
                                            ? tr("From @BotFather, e.g. 123456:ABC-DEF...")
                                            : tr("Stored; type to replace"));
    telegram_chat_ = new QLineEdit(qs(s.telegram_chat));
    telegram_chat_->setPlaceholderText(tr("The chat id the bot writes to"));
    webhook_ = new QLineEdit(qs(s.webhook_url));
    webhook_->setPlaceholderText(tr("Optional: https://... gets a JSON message"));
    on_failure_ = new QCheckBox(tr("When a backup fails or cannot read some files"));
    on_failure_->setChecked(s.on_failure);
    on_damage_ = new QCheckBox(tr("When a check or test restore finds damage, or the backup drive is failing"));
    on_damage_->setChecked(s.on_damage);
    on_success_ = new QCheckBox(tr("After every backup too"));
    on_success_->setChecked(s.on_success);
    monthly_ = new QCheckBox(tr("Send a monthly summary"));
    monthly_->setToolTip(tr("At the start of each month: how many backups ran last month and how many need attention"));
    monthly_->setChecked(s.monthly_summary);

    auto* form = new QFormLayout;
    form->addRow(tr("ntfy topic URL:"), ntfy_url_);
    form->addRow(tr("ntfy access token:"), ntfy_token_);
    form->addRow(tr("Email (through ntfy):"), ntfy_email_);
    form->addRow(tr("Telegram bot token:"), telegram_token_);
    form->addRow(tr("Telegram chat id:"), telegram_chat_);
    form->addRow(tr("Webhook URL:"), webhook_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    QPushButton* test = buttons->addButton(tr("Save and send a test"), QDialogButtonBox::ActionRole);
    connect(test, &QPushButton::clicked, this, &AlertsDialog::send_test);
    connect(buttons, &QDialogButtonBox::accepted, this, &AlertsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note(tr("Windows notifications only reach you at this computer. Vaultik can also send an alert "
                              "to your phone or another service, from the backup itself, even when Vaultik is not "
                              "open. ntfy is a free push notification app (ntfy.sh, or your own server): install it "
                              "on the phone and subscribe to the same topic name. Use a long topic name, since "
                              "anyone who knows it can read the alerts.")));
    layout->addLayout(form);
    layout->addWidget(on_failure_);
    layout->addWidget(on_damage_);
    layout->addWidget(on_success_);
    layout->addWidget(monthly_);
    layout->addWidget(note(tr("Alerts name the computer and the job, and say what happened; never your files. "
                              "Tokens are stored readable only by your account.")));
    layout->addWidget(buttons);
    resize(620, sizeHint().height());
}

bool AlertsDialog::save() {
    jobs::AlertSettings s;
    s.ntfy_url = u8(ntfy_url_->text().trimmed());
    s.ntfy_email = u8(ntfy_email_->text().trimmed());
    s.telegram_chat = u8(telegram_chat_->text().trimmed());
    s.webhook_url = u8(webhook_->text().trimmed());
    s.on_failure = on_failure_->isChecked();
    s.on_damage = on_damage_->isChecked();
    s.on_success = on_success_->isChecked();
    s.monthly_summary = monthly_->isChecked();
    try {
        jobs::save_alerts(s);
        if (!ntfy_token_->text().isEmpty()) jobs::set_alert_secret("ntfy", u8(ntfy_token_->text().trimmed()));
        if (!telegram_token_->text().isEmpty())
            jobs::set_alert_secret("telegram", u8(telegram_token_->text().trimmed()));
        if (s.ntfy_url.empty()) jobs::set_alert_secret("ntfy", "");
        if (s.telegram_chat.empty()) jobs::set_alert_secret("telegram", "");
    } catch (const std::exception& e) {
        QMessageBox::warning(this, windowTitle(), qs(e.what()));
        return false;
    }
    ntfy_token_->clear();
    telegram_token_->clear();
    return true;
}

void AlertsDialog::send_test() {
    if (!save()) return;
    const jobs::AlertSettings s = jobs::load_alerts();
    if (!s.any()) {
        QMessageBox::information(this, windowTitle(), tr("Fill in at least one way to send alerts first."));
        return;
    }
    jobs::Alert a;
    a.level = jobs::Alert::Level::Info;
    a.event = "test";
    a.job = "test";
    a.title = "Vaultik on " + os::hostname() + ": a test alert";
    a.message = "Alerts reach you here when a backup fails or finds damage.";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<std::string> failed = jobs::send_alert(s, a);
    QApplication::restoreOverrideCursor();
    if (failed.empty()) {
        QMessageBox::information(this, windowTitle(), tr("The test alert was sent."));
        return;
    }
    QStringList lines;
    for (const std::string& f : failed) lines << qs(f);
    QMessageBox box(QMessageBox::Warning, windowTitle(), tr("Some alerts could not be sent:"), QMessageBox::Ok, this);
    box.setTextFormat(Qt::PlainText);
    box.setInformativeText(lines.join('\n'));
    box.exec();
}

void AlertsDialog::accept() {
    if (save()) QDialog::accept();
}
