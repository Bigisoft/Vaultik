#include "settings_dialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "app_settings.h"
#include "theme.h"

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Settings"));
    resize(560, 0);
    const AppSettings s = AppSettings::load();

    auto* tray_title = new QLabel(tr("Notification area"));
    tray_title->setProperty("role", "heading");
    start_ = new QCheckBox(tr("Start Vaultik in the notification area when I sign in"));
    start_->setChecked(starts_with_windows());
    auto* tray_note = new QLabel(tr("It watches every job, including scheduled backups, and tells you when something "
                                    "needs attention. Closing the window keeps it there; Quit in its menu ends it. "
                                    "Backups themselves never need Vaultik to be open."));
    tray_note->setWordWrap(true);
    tray_note->setProperty("role", "faint");

    auto* notify_title = new QLabel(tr("Notifications"));
    notify_title->setProperty("role", "heading");
    failures_ = new QCheckBox(tr("When a backup fails or skips files"));
    failures_->setChecked(s.notify_failures);
    overdue_ = new QCheckBox(tr("When a job has not backed up for too long"));
    overdue_->setChecked(s.notify_overdue);
    remind_ = new QSpinBox;
    remind_->setRange(0, 365);
    remind_->setSpecialValueText(tr("never"));
    remind_->setSuffix(tr(" days"));
    remind_->setValue(s.remind_after_days);
    auto* remind_row = new QHBoxLayout;
    remind_row->setContentsMargins(26, 0, 0, 0);
    remind_row->addWidget(new QLabel(tr("Jobs without a schedule: after")));
    remind_row->addWidget(remind_);
    remind_row->addStretch();
    auto* overdue_note = new QLabel(tr("Scheduled jobs count as overdue after about twice their interval: two days for "
                                       "a daily job, a day for an hourly one."));
    overdue_note->setWordWrap(true);
    overdue_note->setProperty("role", "faint");
    overdue_note->setContentsMargins(26, 0, 0, 0);
    connect(overdue_, &QCheckBox::toggled, remind_, &QWidget::setEnabled);
    remind_->setEnabled(s.notify_overdue);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 14);
    layout->addWidget(tray_title);
    layout->addWidget(start_);
    layout->addWidget(tray_note);
    layout->addSpacing(14);
    layout->addWidget(notify_title);
    layout->addWidget(failures_);
    layout->addWidget(overdue_);
    layout->addLayout(remind_row);
    layout->addWidget(overdue_note);
    layout->addSpacing(10);
    layout->addWidget(buttons);
}

void SettingsDialog::accept() {
    AppSettings s;
    s.notify_failures = failures_->isChecked();
    s.notify_overdue = overdue_->isChecked();
    s.remind_after_days = remind_->value();
    s.save();
    if (start_->isChecked() != starts_with_windows()) set_start_with_windows(start_->isChecked());
    QDialog::accept();
}
