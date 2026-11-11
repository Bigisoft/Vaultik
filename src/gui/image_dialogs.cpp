#include "image_dialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QDateTime>
#include <QFileInfo>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <thread>

#include "common.h"
#include "runner.h"
#include "vaultik/image/recovery_media.hpp"
#include "vaultik/platform/drive_health.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"

#ifdef _WIN32
#include <windows.h>

#include "vaultik/util/win.hpp"
#endif

using namespace vaultik;

namespace {

QLabel* note_label(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setForegroundRole(QPalette::PlaceholderText);
    return l;
}

// Runs vaultik with administrator rights in a console window of its own and waits for it.
// Returns its exit code, or -1 with `error` set when it could not start (approval declined).
int run_engine_elevated(QWidget* parent, const QString& busy, const QStringList& args, QString& error) {
    std::vector<std::string> argv{u8(QDir::toNativeSeparators(Runner::engine_path()))};
    for (const QString& a : args) argv.push_back(u8(a));
    int code = -1;
    error = run_busy(parent, busy, [&](const std::function<bool()>&) { code = os::run_command_elevated(argv, true); });
    return error.isEmpty() ? code : -1;
}

QString layout_text(const image::DiskLayout& l) {
    return QObject::tr("%1, %2, %3")
        .arg(l.model.empty() ? QObject::tr("Disk") : qs(l.model), format_bytes(l.size),
             l.style == image::PartitionStyle::Gpt ? QStringLiteral("GPT") : QStringLiteral("MBR"));
}

}  // namespace

ImageRestoreDialog::ImageRestoreDialog(const jobs::Job& job, std::shared_ptr<Repository> repo,
                                       std::vector<SnapshotEntry> images, int selected, QWidget* parent)
    : QDialog(parent), job_(job), repo_(std::move(repo)), images_(std::move(images)) {
    setWindowTitle(tr("Restore a disk image"));
    resize(620, 560);
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    version_ = new QComboBox;
    for (const SnapshotEntry& s : images_) {
        QString text = format_time(s.snapshot.time);
        if (!s.snapshot.note.empty()) text += "  " + qs(s.snapshot.note);
        version_->addItem(text);
    }
    form->addRow(tr("Version:"), version_);
    disk_ = new QComboBox;
    form->addRow(tr("Disk in it:"), disk_);
    layout->addLayout(form);
    parts_ = new QLabel;
    parts_->setWordWrap(true);
    parts_->setTextFormat(Qt::PlainText);
    layout->addWidget(parts_);

    browse_files_ = new QPushButton(tr("Open its drives in File Explorer..."));
    browse_files_->setToolTip(tr("Copies the image to a temporary virtual disk and opens it read-only (needs "
                                 "administrator rights), to take single files out of it"));
    connect(browse_files_, &QPushButton::clicked, this, &ImageRestoreDialog::browse_files);
    auto* browse_row = new QHBoxLayout;
    browse_row->addWidget(browse_files_);
    browse_row->addStretch();
    layout->addLayout(browse_row);

    layout->addWidget(new QLabel(tr("Restore to:")));
    to_file_ = new QRadioButton(tr("A virtual disk file (.vhdx), to open in File Explorer or use in Hyper-V"));
    to_file_->setChecked(true);
    layout->addWidget(to_file_);
    file_ = new QLineEdit;
    auto* file_browse = new QPushButton(tr("Browse..."));
    connect(file_browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("Virtual disk file"), file_->text(),
                                                       tr("Virtual disks (*.vhdx)"));
        if (!f.isEmpty()) file_->setText(QDir::toNativeSeparators(f));
    });
    auto* file_row = new QHBoxLayout;
    file_row->addSpacing(20);
    file_row->addWidget(file_, 1);
    file_row->addWidget(file_browse);
    layout->addLayout(file_row);

    to_disk_ = new QRadioButton(tr("A disk of this computer (everything on it is replaced)"));
    layout->addWidget(to_disk_);
    target_ = new QComboBox;
    auto* target_row = new QHBoxLayout;
    target_row->addSpacing(20);
    target_row->addWidget(target_, 1);
    layout->addLayout(target_row);
    warning_ = new QLabel;
    warning_->setWordWrap(true);
    warning_->setTextFormat(Qt::PlainText);
    auto* warning_row = new QHBoxLayout;
    warning_row->addSpacing(20);
    warning_row->addWidget(warning_, 1);
    layout->addLayout(warning_row);
    confirm_ = new QLineEdit;
    auto* confirm_row = new QHBoxLayout;
    confirm_row->addSpacing(20);
    confirm_row->addWidget(confirm_, 1);
    layout->addLayout(confirm_row);
    boot_ = new QCheckBox(tr("Then make the disk start Windows (bcdboot)"));
    boot_->setChecked(true);
    auto* boot_row = new QHBoxLayout;
    boot_row->addSpacing(20);
    boot_row->addWidget(boot_, 1);
    layout->addLayout(boot_row);
    layout->addWidget(note_label(tr("The disk Windows runs from cannot be replaced while it runs: start the computer "
                                    "from the recovery drive (More > Create recovery drive) to restore it.")));

    bar_ = new QProgressBar;
    bar_->setRange(0, 1000);
    bar_->hide();
    layout->addWidget(bar_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    layout->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    restore_ = buttons->addButton(tr("Restore"), QDialogButtonBox::AcceptRole);
    connect(restore_, &QPushButton::clicked, this, &ImageRestoreDialog::start);
    connect(buttons, &QDialogButtonBox::rejected, this, &ImageRestoreDialog::reject);
    layout->addWidget(buttons);

    runner_ = new Runner(this);
    connect(runner_, &Runner::progress, this, [this](const QJsonObject& ev) {
        const double total = ev.value("total").toDouble();
        if (total > 0) bar_->setValue(static_cast<int>(ev.value("done").toDouble() * 1000 / total));
        status_->setText(tr("%1 written  ·  %2")
                             .arg(format_bytes(static_cast<std::uint64_t>(ev.value("bytes").toInteger())),
                                  ev.value("current").toString()));
    });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        bar_->hide();
        update_state();
        version_->setEnabled(true);
        if (result == "success")
            status_->setText(tr("Restored to %1. Double-click it in File Explorer to open its drives.")
                                 .arg(QDir::toNativeSeparators(ev.value("file").toString())));
        else if (result == "cancelled")
            status_->setText(tr("Cancelled; the unfinished file was removed."));
        else
            status_->setText(tr("The restore failed: %1").arg(ev.value("message").toString()));
    });

    connect(version_, &QComboBox::currentIndexChanged, this, &ImageRestoreDialog::show_version);
    connect(disk_, &QComboBox::currentIndexChanged, this, [this] {
        fill_targets();
        update_state();
    });
    connect(to_file_, &QRadioButton::toggled, this, &ImageRestoreDialog::update_state);
    connect(target_, &QComboBox::currentIndexChanged, this, &ImageRestoreDialog::update_state);
    connect(confirm_, &QLineEdit::textChanged, this, &ImageRestoreDialog::update_state);
    connect(file_, &QLineEdit::textChanged, this, &ImageRestoreDialog::update_state);

    disks_ = image::list_disks();
    version_->setCurrentIndex(std::clamp(selected, 0, std::max(0, version_->count() - 1)));
    show_version(version_->currentIndex());
}

void ImageRestoreDialog::reject() {
    if (runner_->running()) {
        runner_->cancel();
        return;
    }
    QDialog::reject();
}

const image::DiskLayout* ImageRestoreDialog::layout() const {
    const int i = disk_->currentIndex();
    return i >= 0 && static_cast<std::size_t>(i) < layouts_.size() ? &layouts_[static_cast<std::size_t>(i)] : nullptr;
}

int ImageRestoreDialog::target_disk() const {
    return target_->currentIndex() < 0 ? -1 : target_->currentData().toInt();
}

void ImageRestoreDialog::show_version(int index) {
    layouts_.clear();
    disk_->clear();
    parts_->clear();
    if (index < 0 || static_cast<std::size_t>(index) >= images_.size()) return;
    try {
        layouts_ = image::image_disks(*repo_, images_[static_cast<std::size_t>(index)].snapshot);
    } catch (const std::exception& e) {
        parts_->setText(tr("This version cannot be read: %1").arg(QString::fromUtf8(e.what())));
    }
    for (const image::DiskLayout& l : layouts_) disk_->addItem(layout_text(l));
    disk_->setEnabled(layouts_.size() > 1);
    if (const image::DiskLayout* l = layout()) {
        QStringList lines;
        for (const image::Partition& p : l->partitions)
            if (!(l->style == image::PartitionStyle::Mbr && image::is_container(p.mbr_type)))
                lines << "  " + qs(image::describe(p));
        parts_->setText(tr("It holds:") + "\n" + lines.join('\n'));
        const QString name = QStringLiteral("%1-%2.vhdx")
                                 .arg(qs(job_.id.empty() ? std::string("disk") : job_.id),
                                      to_datetime(images_[static_cast<std::size_t>(index)].snapshot.time).toString("yyyy-MM-dd"));
        if (file_->text().isEmpty() || file_->text().endsWith(".vhdx"))
            file_->setText(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/" + name));
    }
    fill_targets();
    update_state();
}

void ImageRestoreDialog::fill_targets() {
    target_->clear();
    const image::DiskLayout* l = layout();
    std::vector<int> keep;
    if (!job_.repository.empty() && !is_cloud_location(job_.repository)) keep = os::disk_numbers(os::absolute_path(job_.repository));
    for (const int n : os::disk_numbers(u8(Runner::engine_path()))) keep.push_back(n);
    const std::vector<int> running = image::system_disks();
    for (const image::DiskInfo& d : disks_) {
        QString why = qs(image::target_refusal(d, running, keep));
        if (why.isEmpty() && l != nullptr) why = qs(image::fits(*l, d.size, d.sector_size));
        QString text = qs(image::describe(d));
        if (!why.isEmpty()) text += "  -  " + tr("not possible: %1").arg(why);
        target_->addItem(text, d.number);
        if (!why.isEmpty()) {
            // Shown, but cannot be picked.
            if (auto* model = qobject_cast<QStandardItemModel*>(target_->model()))
                model->item(target_->count() - 1)->setEnabled(false);
        }
    }
    // The first disk that can be picked.
    for (int i = 0; i < target_->count(); ++i) {
        auto* model = qobject_cast<QStandardItemModel*>(target_->model());
        if (model == nullptr || model->item(i)->isEnabled()) {
            target_->setCurrentIndex(i);
            break;
        }
    }
}

void ImageRestoreDialog::update_state() {
    const bool busy = runner_->running();
    const bool disk = to_disk_->isChecked();
    file_->setEnabled(!disk && !busy);
    target_->setEnabled(disk && !busy);
    confirm_->setEnabled(disk && !busy);
    boot_->setEnabled(disk && !busy);
    const int n = target_disk();
    bool ok = layout() != nullptr && !busy;
    warning_->clear();
    if (disk) {
        auto* model = qobject_cast<QStandardItemModel*>(target_->model());
        const bool pickable = n >= 0 && (model == nullptr || model->item(target_->currentIndex())->isEnabled());
        if (pickable) {
            auto it = std::find_if(disks_.begin(), disks_.end(), [n](const image::DiskInfo& d) { return d.number == n; });
            QString vols;
            if (it != disks_.end())
                for (const image::DiskVolume& v : it->volumes)
                    if (!v.mount.empty() || !v.label.empty())
                        vols += (vols.isEmpty() ? "" : ", ") + qs(v.mount.empty() ? v.label : v.mount.substr(0, 2) + " " + v.label);
            warning_->setText(tr("Everything on disk %1 is erased%2. To go on, type %3 below.")
                                  .arg(n)
                                  .arg(vols.isEmpty() ? QString() : tr(" (it holds %1 now)").arg(vols))
                                  .arg(qs(image::confirmation_text(n))));
            confirm_->setPlaceholderText(qs(image::confirmation_text(n)));
        }
        ok = ok && pickable && image::confirmed(u8(confirm_->text()), n);
    } else {
        ok = ok && !file_->text().trimmed().isEmpty();
    }
    restore_->setEnabled(ok);
    browse_files_->setEnabled(layout() != nullptr && !busy);
}

void ImageRestoreDialog::start() {
    const image::DiskLayout* l = layout();
    if (l == nullptr || runner_->running()) return;
    const SnapshotEntry& snap = images_[static_cast<std::size_t>(version_->currentIndex())];
    const QString index = QString::number(disk_->currentIndex() + 1);
    if (to_file_->isChecked()) {
        const QString file = file_->text().trimmed();
        if (QFileInfo::exists(file)) {
            QMessageBox::warning(this, windowTitle(), tr("%1 exists already. Choose a new file name.").arg(file));
            return;
        }
        bar_->setValue(0);
        bar_->show();
        version_->setEnabled(false);
        restore_->setEnabled(false);
        status_->setText(tr("Starting..."));
        runner_->start({"image", "restore", qs(id_hex(snap.id)), "--job", qs(job_.id), "--disk-index", index, "--vhdx", file});
        update_state();
        return;
    }
    const int n = target_disk();
    if (!image::confirmed(u8(confirm_->text()), n)) return;
    // The disk shown when the user typed the confirmation, checked against the disks now (a disk
    // unplugged meanwhile can pass its number to another), and again by the engine when it opens
    // the disk.
    auto shown = std::find_if(disks_.begin(), disks_.end(), [n](const image::DiskInfo& d) { return d.number == n; });
    if (shown == disks_.end()) return;
    const image::DiskIdentity chosen = image::identity_of(*shown);
    const std::vector<image::DiskInfo> now = image::list_disks();
    auto current = std::find_if(now.begin(), now.end(), [n](const image::DiskInfo& d) { return d.number == n; });
    const std::string other = current == now.end() ? "disk " + std::to_string(n) + " is not connected any more"
                                                   : image::identity_mismatch(*current, chosen);
    if (!other.empty()) {
        confirm_->clear();
        disks_ = now;
        fill_targets();
        update_state();
        QMessageBox::warning(this, windowTitle(),
                             tr("The disks changed since this window listed them (%1). Nothing was written. Check the "
                                "list and choose the disk again.")
                                 .arg(qs(other)));
        return;
    }
    QStringList args{"image", "restore", qs(id_hex(snap.id)), "--job", qs(job_.id), "--disk-index", index,
                     "--to-disk", QString::number(n), "--confirm", qs(image::confirmation_text(n)),
                     "--expect-size", QString::number(chosen.size), "--pause"};
    if (!chosen.serial.empty()) args << "--expect-serial" << qs(chosen.serial);
    if (!chosen.id.empty()) args << "--expect-id" << qs(chosen.id);
    if (boot_->isChecked()) args << "--boot";
    QString error;
    const int code = run_engine_elevated(this, tr("Restoring onto disk %1 in its own window...").arg(n), args, error);
    confirm_->clear();
    if (!error.isEmpty()) status_->setText(tr("The restore did not start: %1").arg(error));
    else if (code == 0) status_->setText(tr("Disk %1 was restored.").arg(n));
    else status_->setText(tr("The restore did not finish; its window showed why."));
    disks_ = image::list_disks();
    fill_targets();
    update_state();
}

void ImageRestoreDialog::browse_files() {
    if (layout() == nullptr) return;
    const SnapshotEntry& snap = images_[static_cast<std::size_t>(version_->currentIndex())];
    std::vector<std::string> argv{u8(QDir::toNativeSeparators(Runner::engine_path())), "image", "mount", id_hex(snap.id),
                                  "--job", job_.id, "--disk-index", std::to_string(disk_->currentIndex() + 1)};
    // Its window stays open (and the drives with it) until Enter is pressed there.
    std::thread([argv] {
        try {
            os::run_command_elevated(argv, true);
        } catch (const std::exception&) {
            // approval declined: nothing to clean up
        }
    }).detach();
    status_->setText(tr("The drives open in a window of their own; press Enter there to close them."));
}

// ---- recovery drive ------------------------------------------------------------------------

RecoveryDriveDialog::RecoveryDriveDialog(const std::vector<jobs::Job>& jobs, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Create recovery drive"));
    resize(560, 420);
    for (const jobs::Job& j : jobs) {
        backups_.push_back(j.repository);
        if (!j.second_copy.repository.empty()) backups_.push_back(j.second_copy.repository);
    }
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note_label(tr("A recovery drive starts a computer that no longer starts Windows (or has a new, "
                                    "empty disk) and restores a disk image onto it: plug in the backup drive (or "
                                    "connect to the network share), choose the version and the disk, and Vaultik "
                                    "puts it back. Keep it somewhere safe; one drive works for every computer.")));
    adk_ = new QLabel;
    adk_->setWordWrap(true);
    adk_->setTextFormat(Qt::PlainText);
    adk_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(adk_);

    to_usb_ = new QRadioButton(tr("A USB stick (everything on it is erased; up to 32 GB)"));
    layout->addWidget(to_usb_);
    usb_ = new QComboBox;
    auto* refresh = new QPushButton(tr("Look again"));
    connect(refresh, &QPushButton::clicked, this, &RecoveryDriveDialog::fill_drives);
    auto* usb_row = new QHBoxLayout;
    usb_row->addSpacing(20);
    usb_row->addWidget(usb_, 1);
    usb_row->addWidget(refresh);
    layout->addLayout(usb_row);
    confirm_ = new QLineEdit;
    auto* confirm_row = new QHBoxLayout;
    confirm_row->addSpacing(20);
    confirm_row->addWidget(confirm_, 1);
    layout->addLayout(confirm_row);

    to_iso_ = new QRadioButton(tr("An ISO file (to write to a USB stick or DVD later, or to start a virtual machine)"));
    to_iso_->setChecked(true);
    layout->addWidget(to_iso_);
    iso_ = new QLineEdit(QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                                                  "/Vaultik recovery.iso"));
    auto* iso_browse = new QPushButton(tr("Browse..."));
    connect(iso_browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getSaveFileName(this, tr("ISO file"), iso_->text(), tr("ISO files (*.iso)"));
        if (!f.isEmpty()) iso_->setText(QDir::toNativeSeparators(f));
    });
    auto* iso_row = new QHBoxLayout;
    iso_row->addSpacing(20);
    iso_row->addWidget(iso_, 1);
    iso_row->addWidget(iso_browse);
    layout->addLayout(iso_row);
    layout->addWidget(note_label(tr("Making it takes a few minutes and needs administrator rights; it runs in a window "
                                    "of its own.")));
    layout->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    create_ = buttons->addButton(tr("Create"), QDialogButtonBox::AcceptRole);
    connect(create_, &QPushButton::clicked, this, &RecoveryDriveDialog::start);
    connect(buttons, &QDialogButtonBox::rejected, this, &RecoveryDriveDialog::reject);
    layout->addWidget(buttons);

    const image::AdkInfo adk = image::find_adk();
    adk_found_ = adk.found;
    adk_->setText(adk.found ? tr("Windows PE from the Windows ADK is installed.")
                            : qs(adk.missing) + ".\n" + qs(image::adk_help()));
    connect(to_usb_, &QRadioButton::toggled, this, &RecoveryDriveDialog::update_state);
    connect(usb_, &QComboBox::currentIndexChanged, this, &RecoveryDriveDialog::update_state);
    connect(confirm_, &QLineEdit::textChanged, this, &RecoveryDriveDialog::update_state);
    connect(iso_, &QLineEdit::textChanged, this, &RecoveryDriveDialog::update_state);
    fill_drives();
}

void RecoveryDriveDialog::fill_drives() {
    usb_->clear();
#ifdef _WIN32
    for (char c = 'D'; c <= 'Z'; ++c) {
        const std::string drive = std::string(1, c) + ":";
        const std::string root = drive + "\\";
        if (!os::is_directory(root)) continue;
        const std::string why = image::usb_refusal(drive, backups_);
        if (why.find("not a USB") != std::string::npos || why.find("not connected") != std::string::npos) continue;
        wchar_t label[MAX_PATH + 1] = {};
        GetVolumeInformationW(widen(root).c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        ULARGE_INTEGER total{};
        GetDiskFreeSpaceExW(widen(root).c_str(), nullptr, &total, nullptr);
        QString text = qs(drive) + " " + QString::fromWCharArray(label) + " (" + format_bytes(total.QuadPart) + ")";
        if (!why.empty()) text += "  -  " + tr("not possible: %1").arg(qs(why));
        usb_->addItem(text, qs(drive));
        if (!why.empty())
            if (auto* model = qobject_cast<QStandardItemModel*>(usb_->model())) model->item(usb_->count() - 1)->setEnabled(false);
    }
#endif
    if (usb_->count() == 0) usb_->addItem(tr("No USB stick found"), QString());
    update_state();
}

void RecoveryDriveDialog::update_state() {
    const bool usb = to_usb_->isChecked();
    usb_->setEnabled(usb);
    confirm_->setEnabled(usb);
    iso_->setEnabled(!usb);
    const QString drive = usb_->currentData().toString();
    auto* model = qobject_cast<QStandardItemModel*>(usb_->model());
    const bool pickable = !drive.isEmpty() && (model == nullptr || model->item(usb_->currentIndex())->isEnabled());
    confirm_->setPlaceholderText(pickable ? tr("Type ERASE %1 to erase it").arg(drive) : QString());
    bool ok = adk_found_;
    if (usb) ok = ok && pickable && confirm_->text().simplified().toUpper().remove(' ') == "ERASE" + drive;
    else ok = ok && iso_->text().trimmed().endsWith(".iso", Qt::CaseInsensitive);
    create_->setEnabled(ok);
}

void RecoveryDriveDialog::start() {
    QStringList args{"recovery-media", "--pause"};
    const bool usb = to_usb_->isChecked();
    const QString drive = usb_->currentData().toString();
    if (usb) args << "--usb" << drive << "--confirm" << "ERASE " + drive;
    else args << "--iso" << iso_->text().trimmed();
    QString error;
    const int code = run_engine_elevated(this, tr("Making the recovery drive in its own window..."), args, error);
    if (!error.isEmpty()) {
        plain_warning(this, windowTitle(), tr("It did not start: %1").arg(error));
        return;
    }
    if (code != 0) {
        plain_warning(this, windowTitle(), tr("The recovery drive was not made; its window showed why."));
        return;
    }
    QMessageBox::information(this, windowTitle(),
                             usb ? tr("The recovery drive is ready on %1. Start a computer from it (usually a key such "
                                      "as F12 at power-on opens the boot menu) to restore a disk image.")
                                       .arg(drive)
                                 : tr("The recovery ISO file is ready: %1").arg(iso_->text().trimmed()));
    accept();
}
