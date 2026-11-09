#include "key_dialog.h"

#include <QApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

#include "common.h"
#include "runner.h"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/repo/keys.hpp"
#include "vaultik/repo/backends.hpp"

using namespace vaultik;

namespace {

void wipe(std::string& s) {
    secure_wipe(s.data(), s.size());
    s.clear();
}

bool same_location(const QString& a, const QString& b) {
    if (is_cloud_location(u8(a)) || is_cloud_location(u8(b))) {
        QString x = a.trimmed();
        QString y = b.trimmed();
        while (x.endsWith('/')) x.chop(1);
        while (y.endsWith('/')) y.chop(1);
        return x == y;
    }
    const QString x = QDir::cleanPath(QFileInfo(a).absoluteFilePath());
    const QString y = QDir::cleanPath(QFileInfo(b).absoluteFilePath());
#ifdef Q_OS_WIN
    return x.compare(y, Qt::CaseInsensitive) == 0;
#else
    return x == y;
#endif
}

}  // namespace

void show_recovery_key(QWidget* parent, const QString& key, const QString& location) {
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Recovery key"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(QObject::tr("This is the recovery key of the backup in %1. If you forget the password, "
                                         "it lets you set a new one (\"Password and recovery key\" in the toolbar).\n\n"
                                         "Write it down or save it, and keep it apart from this computer. It is "
                                         "shown only now. Anyone who has it can open the backup.")
                                 .arg(location));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* field = new QLineEdit(key);
    field->setReadOnly(true);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSize(mono.pointSize() + 3);
    field->setFont(mono);
    field->setMinimumWidth(QFontMetrics(mono).horizontalAdvance(key) + 40);
    layout->addWidget(field);

    auto* buttons = new QDialogButtonBox;
    auto* copy = buttons->addButton(QObject::tr("Copy"), QDialogButtonBox::ActionRole);
    auto* save = buttons->addButton(QObject::tr("Save to a file..."), QDialogButtonBox::ActionRole);
    auto* done = buttons->addButton(QObject::tr("I have kept it"), QDialogButtonBox::AcceptRole);
    QObject::connect(copy, &QPushButton::clicked, &dialog, [key] { QApplication::clipboard()->setText(key); });
    QObject::connect(save, &QPushButton::clicked, &dialog, [&dialog, key, location] {
        const QString path = QFileDialog::getSaveFileName(&dialog, QObject::tr("Save the recovery key"),
                                                          QDir::homePath() + "/Vaultik recovery key.txt",
                                                          QObject::tr("Text files (*.txt)"));
        if (path.isEmpty()) return;
        QFile f(path);
        const QByteArray text = QObject::tr("Vaultik recovery key for the backup in %1\n%2\n").arg(location, key).toUtf8();
        // Readable by the user only when it is created (on Windows a file in the user's folders is
        // private already).
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
            f.write(text) != text.size())
            QMessageBox::warning(&dialog, dialog.windowTitle(), QObject::tr("Cannot write %1.").arg(path));
    });
    QObject::connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addWidget(buttons);
    dialog.exec();
}

KeyDialog::KeyDialog(const jobs::JobStore& store, const QString& location, QWidget* parent)
    : QDialog(parent), store_(store) {
    setWindowTitle(tr("Password and recovery key"));
    resize(560, 0);

    location_ = new QLineEdit(location);
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose the backup folder"), location_->text());
        if (!dir.isEmpty()) location_->setText(QDir::toNativeSeparators(dir));
    });
    auto* cloud = new QPushButton(tr("Cloud storage..."));
    connect(cloud, &QPushButton::clicked, this, [this] {
        CloudTarget initial = cloud_;
        const QString current = location_->text().trimmed();
        if (is_cloud_location(u8(current))) initial.location = current;
        CloudDialog dialog(initial, this);
        initial.wipe();
        if (dialog.exec() != QDialog::Accepted) return;
        cloud_.wipe();
        cloud_ = dialog.target();
        location_->setText(cloud_.location);
    });
    auto* loc_row = new QHBoxLayout;
    loc_row->addWidget(location_, 1);
    loc_row->addWidget(browse);
    loc_row->addWidget(cloud);

    use_password_ = new QRadioButton(tr("Current password"));
    use_recovery_ = new QRadioButton(tr("Recovery key (password forgotten)"));
    use_password_->setChecked(true);
    secret_ = new QLineEdit;
    secret_->setEchoMode(QLineEdit::Password);
    connect(use_recovery_, &QRadioButton::toggled, this, [this](bool on) {
        secret_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password);
        secret_->setPlaceholderText(on ? QStringLiteral("XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX") : QString());
        secret_->clear();
    });

    auto* unlock = new QGroupBox(tr("Unlock with"));
    auto* ul = new QVBoxLayout(unlock);
    auto* radios = new QHBoxLayout;
    radios->addWidget(use_password_);
    radios->addWidget(use_recovery_);
    radios->addStretch();
    ul->addLayout(radios);
    ul->addWidget(secret_);

    new_password_ = new QLineEdit;
    new_password_->setEchoMode(QLineEdit::Password);
    new_password2_ = new QLineEdit;
    new_password2_->setEchoMode(QLineEdit::Password);
    auto* change = new QPushButton(tr("Set new password"));
    connect(change, &QPushButton::clicked, this, &KeyDialog::change_password);
    auto* pw_box = new QGroupBox(tr("New password"));
    auto* pf = new QFormLayout(pw_box);
    pf->addRow(tr("New password:"), new_password_);
    pf->addRow(tr("Repeat:"), new_password2_);
    pf->addRow(QString(), change);

    auto* recovery = new QPushButton(tr("Make a new recovery key"));
    connect(recovery, &QPushButton::clicked, this, &KeyDialog::make_recovery_key);
    auto* rk_box = new QGroupBox(tr("Recovery key"));
    auto* rl = new QVBoxLayout(rk_box);
    auto* rk_note = new QLabel(tr("A new recovery key replaces the old one, which then stops working. Backups "
                                  "made before Vaultik had recovery keys have none until you make one here."));
    rk_note->setWordWrap(true);
    rl->addWidget(rk_note);
    rl->addWidget(recovery, 0, Qt::AlignLeft);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QFormLayout;
    top->addRow(tr("Backup folder:"), loc_row);
    layout->addLayout(top);
    layout->addWidget(unlock);
    layout->addWidget(pw_box);
    layout->addWidget(rk_box);
    layout->addWidget(buttons);
}

KeyDialog::~KeyDialog() {
    for (QLineEdit* e : {secret_, new_password_, new_password2_}) e->clear();
    cloud_.wipe();
}

std::unique_ptr<Backend> KeyDialog::open_location(const QString& location) const {
    const std::string loc = u8(location);
    if (!is_cloud_location(loc)) return open_backend(loc);
    // Keys from the cloud dialog, else from a saved job with this destination, else the environment.
    if (!cloud_.key_id.isEmpty() && same_location(cloud_.location, location)) {
        const CloudCredentials keys = cloud_.credentials();
        return open_backend(loc, &keys);
    }
    for (const jobs::Job& job : store_.list()) {
        if (!same_location(qs(job.repository), location)) continue;
        const CloudCredentials keys = store_.cloud_credentials(job);
        return open_backend(loc, &keys);
    }
    return open_backend(loc);
}

bool KeyDialog::check_location() {
    const QString dir = location_->text().trimmed();
    if (is_cloud_location(u8(dir))) {
        if (secret_->text().isEmpty()) {
            QMessageBox::warning(this, windowTitle(),
                                 use_recovery_->isChecked() ? tr("Enter the recovery key.")
                                                            : tr("Enter the current password."));
            return false;
        }
        return true;
    }
    if (dir.isEmpty() || !QFileInfo::exists(QDir(dir).filePath(QStringLiteral("config")))) {
        QMessageBox::warning(this, windowTitle(), tr("Choose a folder that holds a Vaultik backup."));
        return false;
    }
    if (secret_->text().isEmpty()) {
        QMessageBox::warning(this, windowTitle(),
                             use_recovery_->isChecked() ? tr("Enter the recovery key.") : tr("Enter the current password."));
        return false;
    }
    return true;
}

void KeyDialog::change_password() {
    if (!check_location()) return;
    if (new_password_->text() != new_password2_->text()) {
        QMessageBox::warning(this, windowTitle(), tr("The two new passwords do not match."));
        return;
    }
    const QString dir = location_->text().trimmed();
    Credential credential{u8(secret_->text()), use_recovery_->isChecked()};
    std::string pw = u8(new_password_->text());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        const std::unique_ptr<Backend> backend = open_location(dir);
        vaultik::change_password(*backend, credential, pw);
    } catch (const Error& e) {
        QApplication::restoreOverrideCursor();
        wipe(credential.secret);
        wipe(pw);
        QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
        return;
    }
    QStringList updated;
    QStringList failed;
    QStringList admin_failed;
    for (const jobs::Job& job : store_.list()) {
        if (!same_location(qs(job.repository), dir)) continue;
        try {
            store_.set_password(job.id, pw);
            updated << qs(job.name);
        } catch (const Error&) {
            failed << qs(job.name);
            continue;
        }
        // Scheduled runs of an administrator job read their own copy, which needs approval to update.
        if (job.run_as_admin && job.schedule.kind != jobs::Schedule::Kind::Manual) {
            try {
                jobs::apply_schedule(job, u8(QDir::toNativeSeparators(QFileInfo(Runner::engine_path()).absolutePath())));
            } catch (const Error&) {
                admin_failed << qs(job.name);
            }
        }
    }
    QApplication::restoreOverrideCursor();
    wipe(credential.secret);
    wipe(pw);
    secret_->clear();
    new_password_->clear();
    new_password2_->clear();

    QString text = tr("The password is changed. Use the new one from now on.");
    if (!updated.isEmpty()) text += "\n\n" + tr("Saved jobs updated: %1").arg(updated.join(", "));
    if (!failed.isEmpty())
        text += "\n\n" + tr("Could not update the saved jobs %1; edit them and enter the new password.")
                             .arg(failed.join(", "));
    if (!admin_failed.isEmpty())
        text += "\n\n" + tr("Scheduled runs of %1 still use the old password: open the job and save it to "
                            "update them.")
                             .arg(admin_failed.join(", "));
    QMessageBox::information(this, windowTitle(), text);
}

void KeyDialog::make_recovery_key() {
    if (!check_location()) return;
    const QString dir = location_->text().trimmed();
    Credential credential{u8(secret_->text()), use_recovery_->isChecked()};
    std::string key;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        const std::unique_ptr<Backend> owned = open_location(dir);
        Backend& backend = *owned;
        if (has_recovery_key(backend)) {
            QApplication::restoreOverrideCursor();
            const auto answer = QMessageBox::question(
                this, windowTitle(), tr("This backup has a recovery key already. Replace it? The old one stops working."));
            if (answer != QMessageBox::Yes) {
                wipe(credential.secret);
                return;
            }
            QApplication::setOverrideCursor(Qt::WaitCursor);
        }
        key = create_recovery_key(backend, credential);
    } catch (const Error& e) {
        QApplication::restoreOverrideCursor();
        wipe(credential.secret);
        QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
        return;
    }
    QApplication::restoreOverrideCursor();
    wipe(credential.secret);
    secret_->clear();
    show_recovery_key(this, qs(key), QDir::toNativeSeparators(dir));
    wipe(key);
}
