#include "disk_tools_dialogs.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include <algorithm>

#include "common.h"
#include "elevated_task.h"
#include "runner.h"
#include "vaultik/image/disk_tools.hpp"
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

void disable_last(QComboBox* box) {
    if (auto* model = qobject_cast<QStandardItemModel*>(box->model())) model->item(box->count() - 1)->setEnabled(false);
}

bool pickable(const QComboBox* box) {
    if (box->currentIndex() < 0) return false;
    const auto* model = qobject_cast<const QStandardItemModel*>(box->model());
    return model == nullptr || model->item(box->currentIndex())->isEnabled();
}

void pick_first_enabled(QComboBox* box) {
    const auto* model = qobject_cast<const QStandardItemModel*>(box->model());
    for (int i = 0; i < box->count(); ++i)
        if (model == nullptr || model->item(i)->isEnabled()) {
            box->setCurrentIndex(i);
            return;
        }
}

QString disk_text(const image::DiskInfo& d) {
    QString text = qs(image::describe(d));
#ifndef _WIN32
    text += " (" + qs(d.path) + ")";
#endif
    QStringList vols;
    for (const image::DiskVolume& v : d.volumes) {
        QString one = v.mount.empty() ? QString() : qs(v.mount);
#ifdef _WIN32
        one = one.left(2);
#endif
        if (!v.label.empty()) one += (one.isEmpty() ? "" : " ") + qs(v.label);
        if (!one.isEmpty()) vols << one;
    }
    if (!vols.isEmpty()) text += "  -  " + vols.join(", ");
    return text;
}

// The disk argument for the engine: its number on Windows, its device on Linux.
QString disk_arg(const image::DiskInfo& d) {
#ifdef _WIN32
    return QString::number(d.number);
#else
    return qs(d.path);
#endif
}

// The disks, each with why it cannot be erased (empty when it can). A mounted partition (Linux)
// is no reason: the user is asked whether to unmount it when the job starts.
std::vector<std::pair<image::DiskInfo, std::string>> tool_disks() {
    std::vector<std::pair<image::DiskInfo, std::string>> out;
    const std::vector<int> running = image::system_disks();
    const std::vector<int> keep = image::protected_tool_disks();
    for (image::DiskInfo& d : image::list_tool_disks()) {
        std::string why = image::tool_refusal(d, running, keep, true);
        out.emplace_back(std::move(d), std::move(why));
    }
    return out;
}

// Runs the engine with administrator rights and shows its progress in a status window (see
// run_elevated_task). Linux without pkexec: shows the command to run in a terminal with sudo
// instead (code -2).
ElevatedResult run_disk_engine(QWidget* parent, const ElevatedTask& task, const QStringList& args) {
#ifndef _WIN32
    if (!elevated_tasks_possible()) {
        QStringList quoted{QStringLiteral("sudo"), Runner::engine_path()};
        for (QString a : args) quoted << "'" + a.replace("'", "'\\''") + "'";
        const QString command = quoted.join(' ');
        QMessageBox box(QMessageBox::Information, QObject::tr("Run as root"), QString(), QMessageBox::Close, parent);
        box.setTextFormat(Qt::PlainText);
        box.setText(QObject::tr("Writing to a disk needs root. Run this command in a terminal, where it shows its "
                                "progress and Ctrl+C stops it:") +
                    "\n\n" + command);
        QPushButton* copy = box.addButton(QObject::tr("Copy the command"), QMessageBox::ActionRole);
        box.exec();
        if (box.clickedButton() == copy) QApplication::clipboard()->setText(command);
        ElevatedResult r;
        r.code = -2;
        return r;
    }
#endif
    return run_elevated_task(parent, task, args);
}

// The engine's exit code when a drive is in use by another program (see --force-dismount).
constexpr int kEngineInUse = 6;

// The drives of disk `d` a program may be using, as the user knows them. Windows: its drive
// letters (or the disk itself), any of which may be the one in use (named when the engine did
// not say which ones are); Linux: the mounted ones.
QStringList drives_of(const image::DiskInfo& d) {
    QStringList out;
#ifdef _WIN32
    for (const image::DiskVolume& v : d.volumes)
        if (!v.mount.empty()) out << (v.mount.size() <= 3 ? qs(v.mount.substr(0, 2)) : qs(v.mount));
    if (out.isEmpty()) out << qs(image::describe(d));
#else
    for (const std::string& m : image::mounted_volumes(d)) out << qs(m);
#endif
    return out;
}

// Asks whether to force `drives` closed; true when the user agreed.
bool ask_force_close(QWidget* parent, const QStringList& drives, const QString& yes) {
    const QString text =
        drives.size() == 1
            ? QObject::tr("%1 is being used by another program. Vaultik can force it closed: any program with files "
                          "open on it loses access to them, and unsaved changes to files on that drive are lost. Force "
                          "it closed and continue?")
                  .arg(drives.front())
            : QObject::tr("%1 are being used by other programs. Vaultik can force them closed: any program with files "
                          "open on them loses access to them, and unsaved changes to files on those drives are lost. "
                          "Force them closed and continue?")
                  .arg(drives.join(", "));
    QMessageBox box(QMessageBox::Warning, parent->windowTitle(), text, QMessageBox::Cancel, parent);
    box.setTextFormat(Qt::PlainText);
    QPushButton* force = box.addButton(yes, QMessageBox::DestructiveRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    return box.clickedButton() == force;
}

// Runs the engine (run_disk_engine) for a job that writes to a disk or drive. When a drive is in
// use by another program, asks whether to force it closed and, if the user agrees, runs the same
// job (the same identity checks and confirmation, nothing typed again) with --force-dismount.
// The engine finds out when it locks the drives and names the ones in use (an "in_use" event;
// in a window of its own, in a file: --in-use-file); the question names those, or `drives` (the
// ones that may be in use) when it named none. Linux: `drives` are mounted, so the question
// comes before it starts. Returns code kEngineInUse when the user did not agree.
ElevatedResult run_disk_engine_forcing(QWidget* parent, const ElevatedTask& task, QStringList args,
                                       const QStringList& drives, const QString& yes) {
#ifndef _WIN32
    if (!drives.isEmpty()) {
        if (!ask_force_close(parent, drives, yes)) {
            ElevatedResult r;
            r.code = kEngineInUse;
            return r;
        }
        args << "--force-dismount";
        return run_disk_engine(parent, task, args);
    }
#endif
    ElevatedResult r;
    {
        QTemporaryDir dir;
        QStringList first = args;
        const QString file = dir.isValid() ? QDir::toNativeSeparators(dir.filePath(QStringLiteral("in-use.txt"))) : QString();
        if (!file.isEmpty()) first << "--in-use-file" << file;
        r = run_disk_engine(parent, task, first);
        if (r.code == kEngineInUse && r.in_use.isEmpty() && !file.isEmpty()) {
            QFile f(file);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                for (const QString& line : QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts))
                    if (!line.trimmed().isEmpty()) r.in_use << line.trimmed();
        }
    }
    if (r.code != kEngineInUse || !r.error.isEmpty()) return r;
    if (!ask_force_close(parent, r.in_use.isEmpty() ? drives : r.in_use, yes)) return r;
    args << "--force-dismount";
    return run_disk_engine(parent, task, args);
}

// The line under a disk tool after it ran: `ok` when it finished, `in_use` when a drive was in
// use (and not forced closed), else `not_done` and why.
QString outcome_text(const ElevatedResult& r, const QString& ok, const QString& in_use, const QString& not_done) {
    if (r.left_running) return QObject::tr("It goes on without Vaultik.");
    if (!r.error.isEmpty()) return QObject::tr("It did not start: %1").arg(r.error);
    if (r.code == kEngineInUse) return in_use;
    if (r.code == 0) return ok;
    if (!r.message.isEmpty()) return QObject::tr("%1: %2").arg(not_done, r.message);
    if (r.in_window) return QObject::tr("%1; its window showed why.").arg(not_done);
    return QObject::tr("%1 (the engine ended with code %2).").arg(not_done).arg(r.code);
}

}  // namespace

// ---- set up a new disk ---------------------------------------------------------------------------

NewDiskDialog::NewDiskDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Set up a new disk"));
    resize(600, 460);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note(tr("Prepares a new, blank or unwanted disk for use: a new partition table, one partition "
                              "over the whole disk, formatted and with a drive letter. Everything on the disk is "
                              "erased. The disk the system or Vaultik runs from is never offered.")));
    auto* form = new QFormLayout;
    disk_ = new QComboBox;
    auto* refresh = new QPushButton(tr("Look again"));
    connect(refresh, &QPushButton::clicked, this, &NewDiskDialog::fill_disks);
    auto* disk_row = new QHBoxLayout;
    disk_row->addWidget(disk_, 1);
    disk_row->addWidget(refresh);
    form->addRow(tr("Disk:"), disk_row);
    style_ = new QComboBox;
    style_->addItem(tr("GPT (recommended)"), QStringLiteral("--gpt"));
    style_->addItem(tr("MBR (for older computers; up to 2 TB)"), QStringLiteral("--mbr"));
    form->addRow(tr("Partition table:"), style_);
    fs_ = new QComboBox;
#ifdef _WIN32
    fs_->addItem(tr("NTFS (for Windows)"), QStringLiteral("ntfs"));
    fs_->addItem(tr("exFAT (also for Mac and Linux, cameras, TVs)"), QStringLiteral("exfat"));
#else
    fs_->addItem(tr("ext4 (for Linux)"), QStringLiteral("ext4"));
    fs_->addItem(tr("NTFS (for Windows)"), QStringLiteral("ntfs"));
    fs_->addItem(tr("exFAT (for every system, cameras, TVs)"), QStringLiteral("exfat"));
#endif
    form->addRow(tr("File system:"), fs_);
    label_ = new QLineEdit(tr("Backup"));
    form->addRow(tr("Name:"), label_);
    letter_ = new QComboBox;
#ifdef _WIN32
    letter_->addItem(tr("Any free letter"), QString());
    for (char c = 'D'; c <= 'Z'; ++c)
        if (!os::exists(std::string(1, c) + ":\\")) letter_->addItem(QString(QChar(c)) + ":", QString(QChar(c)));
    form->addRow(tr("Drive letter:"), letter_);
#else
    letter_->hide();
#endif
    layout->addLayout(form);
    warning_ = new QLabel;
    warning_->setWordWrap(true);
    warning_->setTextFormat(Qt::PlainText);
    layout->addWidget(warning_);
    confirm_ = new QLineEdit;
    layout->addWidget(confirm_);
    status_ = new QLabel;
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    layout->addStretch();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    start_ = buttons->addButton(tr("Set up the disk"), QDialogButtonBox::AcceptRole);
    connect(start_, &QPushButton::clicked, this, &NewDiskDialog::start);
    connect(buttons, &QDialogButtonBox::rejected, this, &NewDiskDialog::reject);
    layout->addWidget(buttons);

    connect(disk_, &QComboBox::currentIndexChanged, this, &NewDiskDialog::update_state);
    connect(confirm_, &QLineEdit::textChanged, this, &NewDiskDialog::update_state);
    connect(label_, &QLineEdit::textChanged, this, &NewDiskDialog::update_state);
    connect(fs_, &QComboBox::currentIndexChanged, this, &NewDiskDialog::update_state);
    fill_disks();
}

const image::DiskInfo* NewDiskDialog::chosen() const {
    if (!pickable(disk_)) return nullptr;
    const int n = disk_->currentData().toInt();
    auto it = std::find_if(disks_.begin(), disks_.end(), [n](const image::DiskInfo& d) { return d.number == n; });
    return it == disks_.end() ? nullptr : &*it;
}

void NewDiskDialog::fill_disks() {
    disk_->clear();
    disks_.clear();
    for (auto& [d, why] : tool_disks()) {
        QString text = disk_text(d);
        if (!why.empty()) text += "  -  " + tr("not possible: %1").arg(qs(why));
        disk_->addItem(text, d.number);
        if (!why.empty()) disable_last(disk_);
        disks_.push_back(d);
    }
    if (disk_->count() == 0) {
        disk_->addItem(tr("No disk found"), -1);
        disable_last(disk_);
    }
    pick_first_enabled(disk_);
    update_state();
}

void NewDiskDialog::update_state() {
    const image::DiskInfo* d = chosen();
    QString problem;
    image::NewDiskOptions o;
    o.filesystem = u8(fs_->currentData().toString());
    o.label = u8(label_->text().trimmed());
    try {
        image::check_init_options(o);
    } catch (const std::exception& e) {
        problem = QString::fromUtf8(e.what());
    }
    if (d == nullptr) {
        warning_->setText(problem);
        confirm_->setPlaceholderText(QString());
        start_->setEnabled(false);
        return;
    }
    const QString want = qs(image::erase_text(*d));
    warning_->setText(problem.isEmpty() ? tr("Everything on this disk is erased. To go on, type %1 below.").arg(want)
                                        : problem);
    confirm_->setPlaceholderText(want);
    start_->setEnabled(problem.isEmpty() && image::typed_matches(u8(confirm_->text()), u8(want)));
}

void NewDiskDialog::start() {
    const image::DiskInfo* d = chosen();
    if (d == nullptr) return;
    const image::DiskInfo shown = *d;
    if (!image::typed_matches(u8(confirm_->text()), image::erase_text(shown))) return;
    // The disk listed when the confirmation was typed, checked against the disks now (a disk
    // unplugged meanwhile passes its number on), and again by the engine when it opens it.
    const std::vector<image::DiskInfo> now = image::list_tool_disks();
    auto current = std::find_if(now.begin(), now.end(), [&](const image::DiskInfo& x) { return x.number == shown.number; });
    const std::string other = current == now.end() ? "the disk is not connected any more"
                                                   : image::identity_mismatch(*current, image::identity_of(shown));
    if (!other.empty()) {
        confirm_->clear();
        fill_disks();
        QMessageBox::warning(this, windowTitle(),
                             tr("The disks changed since this window listed them (%1). Nothing was written. Check the "
                                "list and choose the disk again.")
                                 .arg(qs(other)));
        return;
    }
    const image::DiskIdentity id = image::identity_of(shown);
    QStringList args{"disk", "init", disk_arg(shown), style_->currentData().toString(), "--fs", fs_->currentData().toString(),
                     "--confirm", qs(image::erase_text(shown)), "--expect-size", QString::number(id.size)};
    if (!label_->text().trimmed().isEmpty()) args << "--label" << label_->text().trimmed();
    if (!letter_->currentData().toString().isEmpty()) args << "--letter" << letter_->currentData().toString();
    if (!id.serial.empty()) args << "--expect-serial" << qs(id.serial);
    if (!id.id.empty()) args << "--expect-id" << qs(id.id);
#ifdef _WIN32
    const QStringList drives = drives_of(shown);
#else
    const QStringList drives = drives_of(*current);
#endif
    // Setting up takes moments and cannot be stopped halfway (a partition table half written).
    const ElevatedTask task{windowTitle(), tr("Setting up %1...").arg(qs(image::describe(shown))), QString(),
                            tr("The disk is ready to use.")};
    const ElevatedResult r = run_disk_engine_forcing(this, task, args, drives, tr("Force close and set up"));
    confirm_->clear();
    if (r.code == -2) return;  // Linux: the command is run in a terminal
    status_->setText(outcome_text(r, tr("The disk is ready to use."),
                                  tr("The disk was not set up: it is in use. Nothing was written."),
                                  tr("The disk was not set up")));
    fill_disks();
}

// ---- drive cleanser ------------------------------------------------------------------------------

DriveCleanserDialog::DriveCleanserDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Drive cleanser"));
    resize(640, 560);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note(tr("Erases a disk or a drive so what was on it cannot be brought back, for example before "
                              "you give a computer or a disk away. Or overwrites only the free space of a drive, so "
                              "files deleted earlier cannot be brought back; the files on it stay.")));

    whole_disk_ = new QRadioButton(tr("A whole disk (every partition on it; it ends up empty, without partitions)"));
    whole_disk_->setChecked(true);
    layout->addWidget(whole_disk_);
    disk_ = new QComboBox;
    auto* disk_row = new QHBoxLayout;
    disk_row->addSpacing(20);
    disk_row->addWidget(disk_, 1);
    layout->addLayout(disk_row);

    volume_ = new QRadioButton(tr("One drive (a partition; the rest of the disk stays)"));
    layout->addWidget(volume_);
    volumes_ = new QComboBox;
    auto* volume_row = new QHBoxLayout;
    volume_row->addSpacing(20);
    volume_row->addWidget(volumes_, 1);
    layout->addLayout(volume_row);

    free_space_ = new QRadioButton(tr("Only the free space of a drive (the files on it stay)"));
    layout->addWidget(free_space_);
    folder_ = new QLineEdit;
    folder_->setPlaceholderText(tr("A folder on the drive"));
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString f = QFileDialog::getExistingDirectory(this, tr("A folder on the drive"), folder_->text());
        if (!f.isEmpty()) folder_->setText(QDir::toNativeSeparators(f));
    });
    auto* folder_row = new QHBoxLayout;
    folder_row->addSpacing(20);
    folder_row->addWidget(folder_, 1);
    folder_row->addWidget(browse);
    layout->addLayout(folder_row);

    auto* refresh = new QPushButton(tr("Look again"));
    connect(refresh, &QPushButton::clicked, this, &DriveCleanserDialog::fill_targets);
    auto* form = new QFormLayout;
    method_ = new QComboBox;
    method_->addItem(tr("One pass of zeros (fast; enough for today's drives)"), QStringLiteral("zeros"));
    method_->addItem(tr("One pass of random data"), QStringLiteral("random"));
    method_->addItem(tr("Three passes: random, random, zeros (like DoD 5220.22-M; three times as long)"),
                     QStringLiteral("dod3"));
    form->addRow(tr("Method:"), method_);
    verify_ = new QCheckBox(tr("Read the last pass back to check it (takes as long again)"));
    form->addRow(QString(), verify_);
    form->addRow(QString(), refresh);
    layout->addLayout(form);

    ssd_note_ = note(tr("This is a solid-state drive (SSD, NVMe or memory card). Overwriting reaches every block "
                        "the computer can see, but not the spare blocks the drive keeps for itself, which may still "
                        "hold old data. For those, also use the drive maker's secure erase tool or the secure erase "
                        "in the computer's firmware setup."));
    layout->addWidget(ssd_note_);
    warning_ = new QLabel;
    warning_->setWordWrap(true);
    warning_->setTextFormat(Qt::PlainText);
    layout->addWidget(warning_);
    confirm_ = new QLineEdit;
    layout->addWidget(confirm_);
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
    start_ = buttons->addButton(tr("Erase"), QDialogButtonBox::AcceptRole);
    connect(start_, &QPushButton::clicked, this, &DriveCleanserDialog::start);
    connect(buttons, &QDialogButtonBox::rejected, this, &DriveCleanserDialog::reject);
    layout->addWidget(buttons);

    runner_ = new Runner(this);
    connect(runner_, &Runner::progress, this, [this](const QJsonObject& ev) {
        const double total = ev.value("total").toDouble();
        if (total > 0) bar_->setValue(static_cast<int>(std::min(1000.0, ev.value("done").toDouble() * 1000 / total)));
        status_->setText(ev.value("current").toString());
    });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        bar_->hide();
        if (result == "success") status_->setText(tr("Done: %1").arg(ev.value("message").toString()));
        else if (result == "cancelled") status_->setText(tr("Stopped; the temporary file was removed."));
        else status_->setText(tr("It did not finish: %1").arg(ev.value("message").toString()));
        update_state();
    });

    for (QRadioButton* r : {whole_disk_, volume_, free_space_}) connect(r, &QRadioButton::toggled, this, &DriveCleanserDialog::update_state);
    connect(disk_, &QComboBox::currentIndexChanged, this, &DriveCleanserDialog::update_state);
    connect(volumes_, &QComboBox::currentIndexChanged, this, &DriveCleanserDialog::update_state);
    connect(folder_, &QLineEdit::textChanged, this, &DriveCleanserDialog::update_state);
    connect(confirm_, &QLineEdit::textChanged, this, &DriveCleanserDialog::update_state);
    connect(method_, &QComboBox::currentIndexChanged, this, &DriveCleanserDialog::update_state);
    fill_targets();
}

void DriveCleanserDialog::reject() {
    if (runner_->running()) {
        runner_->cancel();
        return;
    }
    QDialog::reject();
}

const image::DiskInfo* DriveCleanserDialog::chosen_disk() const {
    if (!pickable(disk_)) return nullptr;
    const int n = disk_->currentData().toInt();
    auto it = std::find_if(disks_.begin(), disks_.end(), [n](const image::DiskInfo& d) { return d.number == n; });
    return it == disks_.end() ? nullptr : &*it;
}

void DriveCleanserDialog::fill_targets() {
    disk_->clear();
    volumes_->clear();
    disks_.clear();
    volume_ids_.clear();
    for (auto& [d, why] : tool_disks()) {
        QString text = disk_text(d);
        if (!why.empty()) text += "  -  " + tr("not possible: %1").arg(qs(why));
        disk_->addItem(text, d.number);
        if (!why.empty()) disable_last(disk_);
        for (const image::DiskVolume& v : d.volumes) {
#ifdef _WIN32
            if (v.mount.size() < 2 || v.mount[1] != ':') continue;  // only drives with a letter
            const std::string name = v.mount.substr(0, 2);
#else
            if (v.offset == 0) continue;  // the disk itself
            const std::string name = v.name;
#endif
            QString vtext = qs(name) + (v.label.empty() ? QString() : " " + qs(v.label)) + "  (" + format_bytes(v.length) +
                            (v.filesystem.empty() ? QString() : ", " + qs(v.filesystem)) + ")";
            std::string refusal;
            try {
                volume_ids_[name] = image::volume_identity(image::find_wipe_volume(name, true));
            } catch (const std::exception& e) {
                refusal = e.what();
            }
            if (!refusal.empty()) vtext += "  -  " + tr("not possible: %1").arg(qs(refusal));
            volumes_->addItem(vtext, qs(name));
            if (!refusal.empty()) disable_last(volumes_);
        }
        disks_.push_back(d);
    }
    if (disk_->count() == 0) {
        disk_->addItem(tr("No disk found"), -1);
        disable_last(disk_);
    }
    if (volumes_->count() == 0) {
        volumes_->addItem(tr("No drive found"), QString());
        disable_last(volumes_);
    }
    pick_first_enabled(disk_);
    pick_first_enabled(volumes_);
    update_state();
}

QString DriveCleanserDialog::confirmation() const {
    if (whole_disk_->isChecked()) {
        const image::DiskInfo* d = chosen_disk();
        return d == nullptr ? QString() : qs(image::erase_text(*d));
    }
    if (volume_->isChecked() && pickable(volumes_)) return qs(image::erase_volume_text(u8(volumes_->currentData().toString())));
    return {};
}

void DriveCleanserDialog::update_state() {
    const bool busy = runner_->running();
    const bool disk = whole_disk_->isChecked();
    const bool volume = volume_->isChecked();
    const bool free = free_space_->isChecked();
    disk_->setEnabled(disk && !busy);
    volumes_->setEnabled(volume && !busy);
    folder_->setEnabled(free && !busy);
    confirm_->setEnabled(!free && !busy);
    confirm_->setVisible(!free);
    verify_->setEnabled(!free && !busy);
    method_->setEnabled(!busy);
    const image::DiskInfo* d = chosen_disk();
    ssd_note_->setVisible(disk && d != nullptr && image::solid_state(*d));
    bool ok = !busy;
    if (free) {
        warning_->setText(tr("A temporary file fills the drive's free space and is deleted again. The drive is full "
                             "for a while, so do it when nothing else needs the space."));
        ok = ok && !folder_->text().trimmed().isEmpty();
    } else {
        const QString want = confirmation();
        const QString what = disk ? tr("Everything on this disk is overwritten and cannot be brought back.")
                                  : tr("Everything on this drive is overwritten and cannot be brought back.");
        warning_->setText(want.isEmpty() ? QString() : what + " " + tr("To go on, type %1 below.").arg(want));
        confirm_->setPlaceholderText(want);
        ok = ok && !want.isEmpty() && image::typed_matches(u8(confirm_->text()), u8(want));
    }
    start_->setEnabled(ok);
}

void DriveCleanserDialog::start() {
    if (runner_->running()) return;
    const QString method = method_->currentData().toString();
    if (free_space_->isChecked()) {
        bar_->setValue(0);
        bar_->show();
        status_->setText(tr("Starting..."));
        runner_->start({"disk", "wipe-free", folder_->text().trimmed(), "--method", method});
        update_state();
        return;
    }
    const QString want = confirmation();
    if (want.isEmpty() || !image::typed_matches(u8(confirm_->text()), u8(want))) return;
    QStringList args{"disk", "wipe"};
    QStringList drives;  // the ones that may be in use (see run_disk_engine_forcing)
    if (whole_disk_->isChecked()) {
        const image::DiskInfo shown = *chosen_disk();
        const std::vector<image::DiskInfo> now = image::list_tool_disks();
        auto current = std::find_if(now.begin(), now.end(), [&](const image::DiskInfo& x) { return x.number == shown.number; });
        const std::string other = current == now.end() ? "the disk is not connected any more"
                                                       : image::identity_mismatch(*current, image::identity_of(shown));
        if (!other.empty()) {
            confirm_->clear();
            fill_targets();
            QMessageBox::warning(this, windowTitle(),
                                 tr("The disks changed since this window listed them (%1). Nothing was written. Check "
                                    "the list and choose the disk again.")
                                     .arg(qs(other)));
            return;
        }
        const image::DiskIdentity id = image::identity_of(shown);
#ifdef _WIN32
        drives = drives_of(shown);
#else
        drives = drives_of(*current);
#endif
        args << disk_arg(shown) << "--expect-size" << QString::number(id.size);
        if (!id.serial.empty()) args << "--expect-serial" << qs(id.serial);
        if (!id.id.empty()) args << "--expect-id" << qs(id.id);
    } else {
        // The drive with that name now must be the one listed (a USB stick may have been swapped
        // for another one meanwhile); the engine checks it again on the opened drive.
        const std::string name = u8(volumes_->currentData().toString());
        const auto shown = volume_ids_.find(name);
        std::string other = shown == volume_ids_.end() ? std::string("the drive was not listed") : std::string();
        if (other.empty()) {
            try {
                const image::VolumeTarget now = image::find_wipe_volume(name, true);
                other = image::volume_mismatch(now, shown->second);
#ifdef _WIN32
                drives << qs(now.name);
#else
                if (!now.mount.empty()) drives << qs(now.mount);
#endif
            } catch (const std::exception& e) {
                other = e.what();
            }
        }
        if (!other.empty()) {
            confirm_->clear();
            fill_targets();
            QMessageBox::warning(this, windowTitle(),
                                 tr("The drives changed since this window listed them (%1). Nothing was written. Check "
                                    "the list and choose the drive again.")
                                     .arg(qs(other)));
            return;
        }
        const image::DiskIdentity& id = shown->second;
        args << "--volume" << qs(name) << "--expect-size" << QString::number(id.size);
        if (!id.serial.empty()) args << "--expect-serial" << qs(id.serial);
        if (!id.id.empty()) args << "--expect-id" << qs(id.id);
    }
    args << "--method" << method << "--confirm" << want;
    if (verify_->isChecked()) args << "--verify";
    ElevatedTask task;
    task.title = windowTitle();
    if (whole_disk_->isChecked()) {
        task.working = tr("Erasing %1...").arg(qs(image::describe(*chosen_disk())));
        task.cancel_question = tr("Stop erasing? What was erased so far stays erased and the disk will not be usable "
                                  "until it is erased or set up again.");
    } else {
        task.working = tr("Erasing %1...").arg(volumes_->currentData().toString());
        task.cancel_question = tr("Stop erasing? What was erased so far stays erased and the drive will not be usable "
                                  "until it is erased or formatted again.");
    }
    task.done = tr("Erased.");
    const ElevatedResult r = run_disk_engine_forcing(this, task, args, drives, tr("Force close and erase"));
    confirm_->clear();
    if (r.code == -2) return;
    status_->setText(r.code == 4 ? tr("Stopped before the end: what was erased so far stays erased.")
                                 : outcome_text(r, tr("Erased."), tr("Nothing was erased: the drive is in use."),
                                                tr("It did not finish")));
    fill_targets();
}
