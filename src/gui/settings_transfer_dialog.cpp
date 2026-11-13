#include "settings_transfer_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include "app_settings.h"
#include "common.h"
#include "runner.h"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;

namespace {

QLabel* note(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

}  // namespace

// ---- export ----------------------------------------------------------------------------------

ExportSettingsDialog::ExportSettingsDialog(const jobs::JobStore& store, QWidget* parent) : QDialog(parent), store_(store) {
    setWindowTitle(tr("Export settings and plans"));
    resize(560, 360);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note(tr("Saves Vaultik's settings, the alert settings and every backup job in one file, to set "
                              "up another computer the same way (Import settings and plans there).")));
    auto* form = new QFormLayout;
    const QString name = qs(jobs::default_settings_file_name(os::hostname(), os::now()));
    file_ = new QLineEdit(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + name));
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, windowTitle(), file_->text(),
                                                       tr("Vaultik settings (*.vaultik-settings)"));
        if (!f.isEmpty()) file_->setText(QDir::toNativeSeparators(f));
    });
    auto* file_row = new QHBoxLayout;
    file_row->addWidget(file_, 1);
    file_row->addWidget(browse);
    form->addRow(tr("File:"), file_row);
    layout->addLayout(form);

    secrets_ = new QCheckBox(tr("Include the passwords and cloud storage keys"));
    layout->addWidget(secrets_);
    layout->addWidget(note(tr("Without them the file holds no password or key, and each job asks for its password the "
                              "first time it runs on the other computer. With them, the whole file is encrypted with "
                              "the password below; keep that password apart from the file.")));
    auto* pw_form = new QFormLayout;
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    password2_ = new QLineEdit;
    password2_->setEchoMode(QLineEdit::Password);
    pw_form->addRow(tr("Password for the file:"), password_);
    pw_form->addRow(tr("Repeat:"), password2_);
    layout->addLayout(pw_form);
    layout->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    ok_ = buttons->addButton(tr("Export"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ExportSettingsDialog::reject);
    layout->addWidget(buttons);
    for (QLineEdit* e : {file_, password_, password2_}) connect(e, &QLineEdit::textChanged, this, &ExportSettingsDialog::update_state);
    connect(secrets_, &QCheckBox::toggled, this, &ExportSettingsDialog::update_state);
    update_state();
}

void ExportSettingsDialog::update_state() {
    const bool secrets = secrets_->isChecked();
    password_->setEnabled(secrets);
    password2_->setEnabled(secrets);
    bool ok = !file_->text().trimmed().isEmpty();
    if (secrets) ok = ok && password_->text().size() >= 8 && password_->text() == password2_->text();
    ok_->setEnabled(ok);
    ok_->setToolTip(secrets && password_->text().size() < 8 ? tr("Use a password of at least 8 characters.") : QString());
}

void ExportSettingsDialog::accept() {
    QString path = file_->text().trimmed();
    if (!path.endsWith(QStringLiteral(".vaultik-settings"), Qt::CaseInsensitive)) path += QStringLiteral(".vaultik-settings");
    if (QFileInfo::exists(path) &&
        QMessageBox::question(this, windowTitle(), tr("%1 exists already. Replace it?").arg(path)) != QMessageBox::Yes)
        return;
    const bool secrets = secrets_->isChecked();
    QString typed = secrets ? password_->text() : QString();
    std::string pw = u8(typed);
    typed.fill(QChar(0));
    QString error;
    std::size_t count = 0;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        const jobs::SettingsBundle bundle = jobs::collect_settings(store_, secrets, AppSettings::load().to_json());
        count = bundle.jobs.size();
        Bytes data = jobs::write_settings_file(bundle, pw);
        os::write_file_atomic(u8(QDir::toNativeSeparators(path)), data, 0600);
        secure_wipe(data.data(), data.size());
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
    QApplication::restoreOverrideCursor();
    secure_wipe(pw.data(), pw.size());
    if (!error.isEmpty()) {
        plain_warning(this, windowTitle(), error);
        return;
    }
    password_->clear();  // not kept in the window's memory longer than needed
    password2_->clear();
    QMessageBox::information(this, windowTitle(),
                             tr("Saved %1 backup job(s) and the settings to %2.")
                                 .arg(count)
                                 .arg(QDir::toNativeSeparators(path)));
    QDialog::accept();
}

// ---- import ----------------------------------------------------------------------------------

std::shared_ptr<jobs::SettingsBundle> ImportSettingsDialog::open_file(QWidget* parent) {
    const QString path = QFileDialog::getOpenFileName(parent, tr("Import settings and plans"),
                                                      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                                      tr("Vaultik settings (*.vaultik-settings);;All files (*)"));
    if (path.isEmpty()) return nullptr;
    Bytes data;
    bool encrypted = false;
    try {
        data = os::read_file(u8(QDir::toNativeSeparators(path)), 64u << 20);
        encrypted = jobs::settings_file_encrypted(data);
    } catch (const std::exception& e) {
        plain_warning(parent, tr("Import settings and plans"), QString::fromUtf8(e.what()));
        return nullptr;
    }
    for (;;) {
        std::string pw;
        if (encrypted) {
            bool ok = false;
            QString typed = QInputDialog::getText(parent, tr("Import settings and plans"),
                                                  tr("This file holds passwords and keys and is encrypted. Enter the "
                                                     "password it was saved with:"),
                                                  QLineEdit::Password, QString(), &ok);
            if (!ok) return nullptr;
            pw = u8(typed);
            typed.fill(QChar(0));
        }
        try {
            auto bundle = std::make_shared<jobs::SettingsBundle>(jobs::read_settings_file(data, pw));
            secure_wipe(pw.data(), pw.size());
            return bundle;
        } catch (const AuthError& e) {
            secure_wipe(pw.data(), pw.size());
            plain_warning(parent, tr("Import settings and plans"), QString::fromUtf8(e.what()));
            // asked again
        } catch (const std::exception& e) {
            secure_wipe(pw.data(), pw.size());
            plain_warning(parent, tr("Import settings and plans"), QString::fromUtf8(e.what()));
            return nullptr;
        }
    }
}

ImportSettingsDialog::ImportSettingsDialog(const jobs::JobStore& store, std::shared_ptr<jobs::SettingsBundle> bundle,
                                           QWidget* parent)
    : QDialog(parent), store_(store), bundle_(std::move(bundle)) {
    setWindowTitle(tr("Import settings and plans"));
    resize(620, 480);
    auto* layout = new QVBoxLayout(this);
    const QString from = bundle_->computer.empty() ? tr("another computer") : qs(bundle_->computer);
    layout->addWidget(new QLabel(bundle_->created ? tr("Saved on %1, %2.").arg(from, format_time(bundle_->created))
                                                  : tr("Saved on %1.").arg(from)));
    layout->addWidget(new QLabel(tr("Backup jobs to import:")));
    jobs_ = new QListWidget;
    bool clash = false;
    bool commands = false;
    for (const jobs::Job& j : bundle_->jobs) {
        QString text = qs(j.name) + "   " + QDir::toNativeSeparators(qs(j.repository));
        if (store_.find(j.id)) {
            text += "   " + tr("(a job with this id is here already)");
            clash = true;
        }
        QStringList runs;
        if (!j.before_command.empty()) runs << tr("runs before the backup: %1").arg(qs(j.before_command));
        if (!j.after_command.empty()) runs << tr("runs after the backup: %1").arg(qs(j.after_command));
        if (j.run_as_admin) runs << tr("runs with administrator rights");
        if (!runs.isEmpty()) {
            text += "\n      " + runs.join("\n      ");
            commands = true;
        }
        auto* item = new QListWidgetItem(text, jobs_);
        item->setData(Qt::UserRole, qs(j.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    layout->addWidget(jobs_, 1);
    rename_ = new QRadioButton(tr("A job that is here already: import it as a new job next to it"));
    replace_ = new QRadioButton(tr("A job that is here already: replace it with the one in the file"));
    rename_->setChecked(true);
    rename_->setVisible(clash);
    replace_->setVisible(clash);
    layout->addWidget(rename_);
    layout->addWidget(replace_);
    keep_commands_ = new QCheckBox(tr("Keep the commands and administrator rights shown above (only for a file you "
                                      "made yourself: they run on this computer)"));
    keep_commands_->setChecked(false);
    keep_commands_->setVisible(commands);
    layout->addWidget(keep_commands_);
    if (commands)
        layout->addWidget(note(tr("Without that tick, those jobs are imported without their commands and run without "
                                  "administrator rights.")));
    app_ = new QCheckBox(tr("Also Vaultik's settings (notification area, reminders, updates)"));
    app_->setChecked(!bundle_->app.empty());
    app_->setEnabled(!bundle_->app.empty());
    layout->addWidget(app_);
    QStringList where;
    if (bundle_->alerts) {
        if (!bundle_->alerts->ntfy_url.empty()) where << qs(bundle_->alerts->ntfy_url);
        if (!bundle_->alerts->telegram_chat.empty()) where << tr("Telegram chat %1").arg(qs(bundle_->alerts->telegram_chat));
        if (!bundle_->alerts->webhook_url.empty()) where << qs(bundle_->alerts->webhook_url);
    }
    // Not ticked unless chosen: they say where this computer's alerts go.
    alerts_ = new QCheckBox(where.isEmpty() ? tr("Also the alert settings (ntfy, Telegram, webhook)")
                                            : tr("Also the alert settings, sent to: %1").arg(where.join(", ")));
    alerts_->setChecked(false);
    alerts_->setEnabled(bundle_->alerts.has_value());
    layout->addWidget(alerts_);
    layout->addWidget(note(tr("Imported jobs keep their schedule but start paused, so nothing runs before you checked "
                              "their folders on this computer; then choose Resume schedule (More menu).") +
                           (bundle_->with_secrets ? QString()
                                                  : " " + tr("The file has no passwords: each job asks for its "
                                                             "password the first time it runs or is opened."))));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    buttons->addButton(tr("Import"), QDialogButtonBox::AcceptRole);
    connect(buttons, &QDialogButtonBox::accepted, this, &ImportSettingsDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ImportSettingsDialog::reject);
    layout->addWidget(buttons);
}

void ImportSettingsDialog::accept() {
    jobs::ImportChoice choice;
    for (int i = 0; i < jobs_->count(); ++i)
        if (jobs_->item(i)->checkState() == Qt::Checked) choice.jobs.push_back(u8(jobs_->item(i)->data(Qt::UserRole).toString()));
    choice.replace = replace_->isChecked();
    choice.alerts = alerts_->isChecked();
    choice.keep_commands = keep_commands_->isChecked();
    if (choice.jobs.empty() && !choice.alerts && !app_->isChecked()) {
        QMessageBox::information(this, windowTitle(), tr("Choose at least one job or setting to import."));
        return;
    }
    QString error;
    QStringList schedule_errors;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        if (choice.jobs.empty()) {
            // No job ticked (an empty list would mean all of them): only the settings.
            jobs::SettingsBundle settings_only = *bundle_;
            settings_only.jobs.clear();
            result_ = jobs::import_settings(store_, settings_only, choice);
        } else {
            result_ = jobs::import_settings(store_, *bundle_, choice);
        }
        // Paused: this removes the tasks of a job that was replaced, and sets up none.
        const std::string dir = u8(QDir::toNativeSeparators(QFileInfo(Runner::engine_path()).absolutePath()));
        for (const jobs::ImportedJob& i : result_.jobs)
            if (const auto job = store_.find(i.id)) {
                try {
                    jobs::apply_schedule(*job, dir);
                } catch (const std::exception& e) {
                    // The tasks of a replaced job (one with administrator rights asks for
                    // approval to remove them) may still run it: the user must know.
                    if (i.replaced) schedule_errors << qs(i.name) + ": " + QString::fromUtf8(e.what());
                }
            }
        if (app_->isChecked()) {
            AppSettings a = AppSettings::load();
            a.apply_json(bundle_->app);
            a.save();
        }
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
    QApplication::restoreOverrideCursor();
    if (!error.isEmpty()) {
        plain_warning(this, windowTitle(), error);
        return;
    }
    if (!schedule_errors.isEmpty())
        plain_warning(this, windowTitle(),
                      tr("The jobs were imported, but the schedule of a job that was replaced could not be removed, so "
                         "it may still run on its own. Open the job and choose Pause schedule, or delete its task in "
                         "Task Scheduler:") +
                          "\n\n" + schedule_errors.join("\n"));
    QDialog::accept();
}
