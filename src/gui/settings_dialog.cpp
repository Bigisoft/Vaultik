#include "settings_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "app_settings.h"
#include "hello.h"
#include "theme.h"

namespace {

QLabel* heading(const QString& text) {
    auto* l = new QLabel(text);
    l->setProperty("role", "heading");
    return l;
}

QLabel* note(const QString& text, int indent = 0) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setProperty("role", "faint");
    l->setContentsMargins(indent, 0, 0, 0);
    return l;
}

// A check box set in under the one it depends on.
QWidget* indented(QWidget* w) {
    auto* box = new QWidget;
    auto* l = new QHBoxLayout(box);
    l->setContentsMargins(26, 0, 0, 0);
    l->addWidget(w);
    l->addStretch();
    return box;
}

}  // namespace

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    resize(580, 0);
    const AppSettings s = AppSettings::load();
    const StartupSetting startup = startup_setting();

    start_ = new QCheckBox(tr("Start Vaultik when I sign in to Windows"));
    start_->setChecked(startup.enabled);
    start_hidden_ = new QCheckBox(tr("Start hidden in the notification area"));
    start_hidden_->setChecked(startup.enabled ? startup.hidden : true);
    const bool portable = vaultik::jobs::is_portable();
    if (portable) {
        // It may run from a drive that is not always there, and Windows would start it at sign-in.
        start_->setEnabled(false);
        start_->setChecked(false);
        start_->setToolTip(tr("Not for a portable copy: install Vaultik on this computer to start it at sign-in."));
    }

    tray_icon_ = new QCheckBox(tr("Show Vaultik's icon in the notification area"));
    tray_icon_->setChecked(s.tray_icon);
    tray_status_ = new QCheckBox(tr("Turn the icon red when a backup fails or is overdue"));
    tray_status_->setChecked(s.tray_status);
    close_to_tray_ = new QCheckBox(tr("Close to the notification area"));
    close_to_tray_->setChecked(s.close_to_tray);
    minimize_to_tray_ = new QCheckBox(tr("Minimize to the notification area"));
    minimize_to_tray_->setChecked(s.minimize_to_tray);
    single_click_ = new QCheckBox(tr("Single-click the icon to show or hide the window"));
    single_click_->setChecked(s.single_click_toggle);

    failures_ = new QCheckBox(tr("When a backup fails or skips files"));
    failures_->setChecked(s.notify_failures);
    overdue_ = new QCheckBox(tr("When a job has not backed up for too long"));
    overdue_->setChecked(s.notify_overdue);
    remind_ = new QSpinBox;
    remind_->setRange(0, 365);
    remind_->setSpecialValueText(tr("never"));
    remind_->setSuffix(tr(" days"));
    remind_->setValue(s.remind_after_days);
    auto* remind_box = new QWidget;
    auto* rl = new QHBoxLayout(remind_box);
    rl->setContentsMargins(26, 0, 0, 0);
    rl->addWidget(new QLabel(tr("Jobs without a schedule: after")));
    rl->addWidget(remind_);
    rl->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 14);
    layout->setSpacing(6);
    layout->addWidget(heading(tr("Startup")));
    layout->addWidget(start_);
    layout->addWidget(indented(start_hidden_));
    layout->addSpacing(12);
    layout->addWidget(heading(tr("Notification area")));
    layout->addWidget(tray_icon_);
    layout->addWidget(indented(tray_status_));
    layout->addWidget(indented(close_to_tray_));
    layout->addWidget(indented(minimize_to_tray_));
    layout->addWidget(indented(single_click_));
    layout->addWidget(note(tr("From the notification area Vaultik watches every job, also the scheduled ones, while you "
                              "are signed in. Backups themselves never need Vaultik to be open."),
                           26));
    layout->addSpacing(12);
    layout->addWidget(heading(tr("Notifications")));
    layout->addWidget(failures_);
    layout->addWidget(overdue_);
    layout->addWidget(remind_box);
    layout->addWidget(note(tr("Scheduled jobs count as overdue after about twice their interval: two days for a daily "
                              "job, a day for an hourly one. With Windows' Do Not Disturb on, notifications go quietly "
                              "to the notification centre."),
                           26));

    // Security: Windows Hello before anyone can open the backups from this window.
    hello_ = new QCheckBox(tr("Ask for Windows Hello before opening backups"));
    hello_->setChecked(s.require_hello);
    const bool hello_ready = hello::available();
    if (!hello_ready && !s.require_hello) {
        hello_->setEnabled(false);
        hello_->setToolTip(tr("Set up Windows Hello (face, fingerprint or PIN) in Windows Settings first."));
    }
    connect(hello_, &QCheckBox::clicked, this, [this](bool on) {
        // Turning it on proves it works, so nobody is locked out by a setting that cannot be met;
        // turning it off needs it too, or anyone at the computer could. (When Windows Hello is gone
        // altogether, turning it off is the way back in.)
        if (!on && !hello::available()) return;
        const QString why = on ? tr("Confirm it is you to protect Vaultik's backups")
                               : tr("Confirm it is you to stop protecting Vaultik's backups");
        if (hello::verify(this, why) != hello::Result::Verified) hello_->setChecked(!on);
    });
    layout->addSpacing(12);
    layout->addWidget(heading(tr("Security")));
    layout->addWidget(hello_);
    layout->addWidget(note(tr("Restoring, browsing, searching and comparing versions then need your face, fingerprint "
                              "or PIN, once until Vaultik is hidden again. Scheduled backups are not affected."),
                           26));
    layout->addSpacing(10);
    layout->addWidget(buttons);

    for (QCheckBox* c : {start_, tray_icon_, overdue_}) connect(c, &QCheckBox::toggled, this, &SettingsDialog::update_enabled);
    update_enabled();
}

void SettingsDialog::update_enabled() {
    start_hidden_->setEnabled(start_->isChecked());
    // Without the icon there is nothing to hide the window into.
    const bool tray = tray_icon_->isChecked();
    for (QCheckBox* c : {tray_status_, close_to_tray_, minimize_to_tray_, single_click_}) c->setEnabled(tray);
    remind_->setEnabled(overdue_->isChecked());
}

void SettingsDialog::accept() {
    AppSettings s;
    s.tray_icon = tray_icon_->isChecked();
    s.tray_status = tray_status_->isChecked();
    s.close_to_tray = close_to_tray_->isChecked();
    s.minimize_to_tray = minimize_to_tray_->isChecked();
    s.single_click_toggle = single_click_->isChecked();
    s.notify_failures = failures_->isChecked();
    s.notify_overdue = overdue_->isChecked();
    s.remind_after_days = remind_->value();
    s.require_hello = hello_->isChecked();
    s.save();

    if (!vaultik::jobs::is_portable()) {  // never touch the installed copy's entry
        StartupSetting startup;
        startup.enabled = start_->isChecked();
        startup.hidden = startup.enabled && start_hidden_->isChecked();
        set_startup_setting(startup);
    }
    QDialog::accept();
}
