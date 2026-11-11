#include "job_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimeEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "common.h"
#include "vaultik/ops/backup.hpp"
#include "key_dialog.h"
#include "vaultik/repo/keys.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/profile_items.hpp"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/ops/virt.hpp"
#include "vaultik/image/disk.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/config.hpp"
#include "vaultik/repo/backends.hpp"
#include "vaultik/repo/repository.hpp"
#include "runner.h"

using namespace vaultik;

namespace {

// Index order of the preset combo box.
const char* kPresets[] = {"background", "low", "normal", "high"};

QSpinBox* spin(int min, int max, const QString& suffix, const QString& off_text) {
    auto* s = new QSpinBox;
    s->setRange(min, max);
    s->setSuffix(suffix);
    if (!off_text.isEmpty()) s->setSpecialValueText(off_text);
    return s;
}

QComboBox* combo(const QStringList& items) {
    auto* c = new QComboBox;
    c->addItems(items);
    return c;
}

QWidget* row(std::initializer_list<QWidget*> widgets) {
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    for (QWidget* x : widgets) l->addWidget(x);
    l->addStretch();
    return w;
}

QStringList items_of(const QListWidget* list) {
    QStringList out;
    for (int i = 0; i < list->count(); ++i) out << list->item(i)->text();
    return out;
}

}  // namespace

JobDialog::JobDialog(const jobs::JobStore& store, const jobs::Job* existing, QWidget* parent)
    : QDialog(parent), store_(store), editing_(existing != nullptr) {
    setWindowTitle(editing_ ? tr("Edit backup job") : tr("New backup job"));
    setSizeGripEnabled(true);

    auto* tabs = new QTabWidget;
    tabs->addTab(scrolled(build_general()), tr("General"));
    tabs->addTab(scrolled(build_sources()), tr("What to back up"));
    tabs->addTab(scrolled(build_schedule()), tr("Schedule"));
    tabs->addTab(scrolled(build_resources()), tr("Resources"));
    tabs->addTab(scrolled(build_retention()), tr("Old backups"));
#ifdef _WIN32
    const int machines_tab = tabs->addTab(scrolled(build_machines()), tr("Virtual machines"));
    // Asking Hyper-V takes a few seconds: only when the tab is opened.
    connect(tabs, &QTabWidget::currentChanged, this, [this, machines_tab](int index) {
        if (index == machines_tab && !hyperv_listed_) list_hyperv();
    });
#endif

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &JobDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &JobDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    // The tabs scroll, so the window starts at a comfortable size that fits the screen.
    fit_to_screen(this, QSize(720, 640), parent);

    if (existing != nullptr) {
        job_ = *existing;
        load(*existing);
    } else {
        jobs::Job defaults;
        defaults.schedule.kind = jobs::Schedule::Kind::Daily;
        defaults.resources = priority_preset("low");
        defaults.retention = jobs::default_retention();
        for (const jobs::ExclusionPreset& p : jobs::exclusion_presets())
            if (p.default_on) defaults.excludes.insert(defaults.excludes.end(), p.patterns.begin(), p.patterns.end());
        load(defaults);
    }
}

JobDialog::~JobDialog() {
    cloud_.wipe();
    copy_cloud_.wipe();
}

QWidget* JobDialog::build_general() {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    name_ = new QLineEdit;
    name_->setPlaceholderText(tr("e.g. Documents to USB drive"));
    form->addRow(tr("Name:"), name_);

    repository_ = new QLineEdit;
    repository_->setPlaceholderText(tr("Folder on a USB drive, another disk or a network share, or cloud storage"));
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString start = is_cloud_location(u8(repository_->text())) ? QString() : repository_->text();
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Backup destination"), start);
        if (!dir.isEmpty()) repository_->setText(QDir::toNativeSeparators(dir));
    });
    auto* cloud = new QPushButton(tr("Cloud storage..."));
    connect(cloud, &QPushButton::clicked, this, [this] {
        CloudTarget initial = cloud_;
        initial.location = is_cloud_location(u8(repository_->text())) ? repository_->text().trimmed() : QString();
        CloudDialog dialog(initial, this);
        initial.wipe();
        if (dialog.exec() != QDialog::Accepted) return;
        cloud_.wipe();
        cloud_ = dialog.target();
        repository_->setText(cloud_.location);
    });
    auto* repo_row = new QWidget;
    auto* rl = new QHBoxLayout(repo_row);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(repository_, 1);
    rl->addWidget(browse);
    rl->addWidget(cloud);
    form->addRow(tr("Destination:"), repo_row);

    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    password2_ = new QLineEdit;
    password2_->setEchoMode(QLineEdit::Password);
    if (editing_) {
        password_->setPlaceholderText(tr("unchanged"));
        password2_->setPlaceholderText(tr("unchanged"));
    }
    form->addRow(tr("Password:"), password_);
    form->addRow(tr("Repeat:"), password2_);

    auto* note = new QLabel(tr("The password encrypts the backups. If the destination already holds a "
                               "Vaultik repository, enter its password. Without it the backups cannot be "
                               "restored, so keep a copy somewhere safe. It is stored for scheduled runs, "
                               "readable only by your user account."));
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);

    // An optional second copy elsewhere (3-2-1).
    copy_repository_ = new QLineEdit;
    copy_repository_->setPlaceholderText(tr("Optional: another drive or cloud storage"));
    copy_repository_->setClearButtonEnabled(true);
    auto* copy_browse = new QPushButton(tr("Browse..."));
    connect(copy_browse, &QPushButton::clicked, this, [this] {
        const QString start = is_cloud_location(u8(copy_repository_->text())) ? QString() : copy_repository_->text();
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Second copy"), start);
        if (!dir.isEmpty()) copy_repository_->setText(QDir::toNativeSeparators(dir));
    });
    auto* copy_cloud = new QPushButton(tr("Cloud storage..."));
    connect(copy_cloud, &QPushButton::clicked, this, [this] {
        CloudTarget initial = copy_cloud_;
        initial.location = is_cloud_location(u8(copy_repository_->text())) ? copy_repository_->text().trimmed() : QString();
        CloudDialog dialog(initial, this);
        initial.wipe();
        if (dialog.exec() != QDialog::Accepted) return;
        copy_cloud_.wipe();
        copy_cloud_ = dialog.target();
        copy_repository_->setText(copy_cloud_.location);
    });
    auto* copy_row = new QWidget;
    auto* cl = new QHBoxLayout(copy_row);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(copy_repository_, 1);
    cl->addWidget(copy_browse);
    cl->addWidget(copy_cloud);
    form->addRow(tr("Second copy:"), copy_row);
    auto* copy_note = new QLabel(tr("After each backup, the new versions are copied there too, so one failed "
                                    "drive or account does not lose them (e.g. a USB drive and cloud storage). "
                                    "It uses the same password and keeps versions by the same rules."));
    copy_note->setWordWrap(true);
    copy_note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(copy_note);
    return page;
}

void JobDialog::estimate_size() {
    const jobs::Job j = collect();
    if (j.sources.empty()) {
        QMessageBox::information(this, windowTitle(), tr("Add the folders or files to back up first."));
        return;
    }
    SourceEstimate e;
    const QString error = run_busy(this, tr("Counting files..."), [&](const std::function<bool()>& cancelled) {
        e = estimate_sources(j.sources, j.excludes, cancelled);
    });
    if (!error.isEmpty()) {
        if (error != tr("Cancelled.")) plain_warning(this, windowTitle(), error);
        return;
    }
    // Typical speeds: an external hard disk, or uploading at 100 Mbit/s (12.5 MB/s).
    const bool cloud = is_cloud_location(u8(repository_->text().trimmed()));
    const double seconds = static_cast<double>(e.bytes) / (cloud ? 12.5e6 : 100e6);
    QString text = tr("%1 in %2.").arg(format_bytes(e.bytes), count_of(e.files, tr("file"), tr("files"))) + " ";
    text += cloud ? tr("The first backup takes roughly %1 when uploading at 100 Mbit/s.").arg(format_duration(seconds))
                  : tr("The first backup takes roughly %1 to an external hard disk.").arg(format_duration(seconds));
    text += " " + tr("It stores less after compression and deduplication, and later backups only read what changed.");
    if (e.unreadable > 0)
        text += " " + tr("%1 could not be read.").arg(count_of(e.unreadable, tr("folder"), tr("folders")));
    estimate_->setText(text);
    estimate_->show();
}

QWidget* JobDialog::build_disks() {
    auto* box = new QWidget;
    auto* layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new QLabel(tr("Disks to back up whole:")));
    disks_ = new QListWidget;
    layout->addWidget(disks_, 1);
    auto* refresh = new QPushButton(tr("Look again"));
    connect(refresh, &QPushButton::clicked, this, [this] {
        std::vector<std::string> checked;
        for (int i = 0; i < disks_->count(); ++i)
            if (disks_->item(i)->checkState() == Qt::Checked) checked.push_back(u8(disks_->item(i)->data(Qt::UserRole).toString()));
        fill_disks(checked);
    });
    layout->addWidget(row({refresh}));
    auto* note = new QLabel(tr("A disk image holds every partition of the disk (Windows, its boot and recovery "
                               "partitions, your data) and its layout, so the whole computer can be brought back "
                               "onto a new disk with the recovery drive (More > Create recovery drive). Only the "
                               "used space is read, and later images store only what changed. It needs "
                               "administrator rights: scheduled runs have them; Back up now works when Vaultik "
                               "runs as administrator. The destination must be on another disk."));
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);
    return box;
}

void JobDialog::fill_disks(const std::vector<std::string>& checked) {
    if (disks_ == nullptr) return;
    disks_->clear();
    std::vector<std::string> left = checked;
    for (const image::DiskInfo& d : image::list_disks()) {
        // The disk Windows runs from is kept by that role, so a replaced system disk is still found.
        const std::string id = d.system ? std::string("system") : d.id;
        if (id.empty()) continue;  // no partition table: nothing to image
        QString text = qs(image::describe(d));
        QStringList vols;
        for (const image::DiskVolume& v : d.volumes)
            if (!v.mount.empty()) vols << qs(v.mount.substr(0, 2) + (v.label.empty() ? "" : " " + v.label));
        if (!vols.isEmpty()) text += "\n    " + vols.join(", ");
        auto* item = new QListWidgetItem(text, disks_);
        item->setData(Qt::UserRole, qs(id));
        const bool on = std::find(checked.begin(), checked.end(), id) != checked.end() ||
                        std::find(checked.begin(), checked.end(), d.id) != checked.end();
        item->setCheckState(on ? Qt::Checked : Qt::Unchecked);
        std::erase(left, id);
        std::erase(left, d.id);
    }
    // Disks of the job that are not connected now stay in it.
    for (const std::string& id : left) {
        auto* item = new QListWidgetItem(tr("%1 (not connected)").arg(qs(id)), disks_);
        item->setData(Qt::UserRole, qs(id));
        item->setCheckState(Qt::Checked);
    }
}

void JobDialog::update_kind() {
    const bool image = kind_ != nullptr && kind_->currentIndex() == 1;
    if (files_box_ != nullptr) files_box_->setVisible(!image);
    if (disks_box_ != nullptr) disks_box_->setVisible(image);
    if (admin_ != nullptr) {
        if (image) admin_->setChecked(true);
        admin_->setEnabled(!image);
    }
}

QWidget* JobDialog::build_sources() {
    auto* page = new QWidget;
    auto* outer = new QVBoxLayout(page);
#ifdef _WIN32
    kind_ = new QComboBox;
    kind_->addItems({tr("Folders and files"), tr("Whole disks (disk image)")});
    outer->addWidget(row({new QLabel(tr("Back up:")), kind_}));
#endif
    files_box_ = new QWidget;
    auto* layout = new QVBoxLayout(files_box_);
    layout->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(files_box_, 1);
#ifdef _WIN32
    disks_box_ = build_disks();
    disks_box_->hide();
    outer->addWidget(disks_box_, 1);
    connect(kind_, &QComboBox::currentIndexChanged, this, &JobDialog::update_kind);
#endif

    layout->addWidget(new QLabel(tr("Folders and files to back up:")));
    sources_ = new QListWidget;
    layout->addWidget(sources_, 2);
    auto* add_folder = new QPushButton(tr("Add folder..."));
    auto* add_file = new QPushButton(tr("Add files..."));
    auto* remove = new QPushButton(tr("Remove"));
    connect(add_folder, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder to back up"));
        if (!dir.isEmpty()) sources_->addItem(QDir::toNativeSeparators(dir));
    });
    connect(add_file, &QPushButton::clicked, this, [this] {
        for (const QString& f : QFileDialog::getOpenFileNames(this, tr("Files to back up")))
            sources_->addItem(QDir::toNativeSeparators(f));
    });
    connect(remove, &QPushButton::clicked, this, [this] { qDeleteAll(sources_->selectedItems()); });
    // Ready-made choices: the account's own folders, wherever Windows keeps them.
    auto* common = new QPushButton(tr("Add common data"));
    auto* common_menu = new QMenu(common);
    connect(common_menu, &QMenu::aboutToShow, this, [this, common_menu] {
        common_menu->clear();
        for (const std::string& group : jobs::profile_groups()) {
            QString label;
            if (group == "documents") label = tr("Documents");
            else if (group == "desktop") label = tr("Desktop");
            else if (group == "pictures") label = tr("Pictures");
            else if (group == "music") label = tr("Music");
            else if (group == "videos") label = tr("Videos");
            else if (group == "downloads") label = tr("Downloads");
            else if (group == "browsers") label = tr("Browser bookmarks, passwords and history");
            else if (group == "email") label = tr("Email (Outlook data files, Thunderbird)");
            else if (group == "games") label = tr("Game saves");
            else label = qs(group);
            const std::vector<std::string> paths = jobs::existing_paths(group);
            QStringList shown;
            for (const std::string& p : paths) shown << qs(p);
            QAction* act = common_menu->addAction(label);
            act->setEnabled(!paths.empty());
            act->setToolTip(paths.empty() ? tr("Not on this computer") : shown.join('\n'));
            connect(act, &QAction::triggered, this, [this, shown] {
                for (const QString& p : shown) {
                    // Not twice, and not inside a folder already listed.
                    bool covered = false;
                    for (int i = 0; i < sources_->count() && !covered; ++i) {
                        const QString have = QDir::toNativeSeparators(sources_->item(i)->text());
                        covered = p.compare(have, Qt::CaseInsensitive) == 0 ||
                                  p.startsWith(have.endsWith('\\') ? have : have + '\\', Qt::CaseInsensitive);
                    }
                    if (!covered) sources_->addItem(p);
                }
            });
        }
        common_menu->setToolTipsVisible(true);
    });
    common->setMenu(common_menu);
    common->setToolTip(tr("Your own folders and the data of common programs, wherever Windows keeps them"));
    auto* estimate = new QPushButton(tr("Estimate size"));
    estimate->setToolTip(tr("Count what the first backup reads, with the exclusions below"));
    connect(estimate, &QPushButton::clicked, this, &JobDialog::estimate_size);
    layout->addWidget(row({add_folder, add_file, common, remove, estimate}));
    estimate_ = new QLabel;
    estimate_->setWordWrap(true);
    estimate_->setTextFormat(Qt::PlainText);
    estimate_->setForegroundRole(QPalette::PlaceholderText);
    estimate_->hide();
    layout->addWidget(estimate_);

    // Ready-made exclusions, two per row.
    layout->addWidget(new QLabel(tr("Skip:")));
    auto* presets = new QGridLayout;
    presets->setContentsMargins(0, 0, 0, 0);
    int n = 0;
    for (const jobs::ExclusionPreset& p : jobs::exclusion_presets()) {
        QString label, tip;
        if (p.id == "temp") label = tr("Temporary files");
        else if (p.id == "caches") label = tr("Browser and app caches");
        else if (p.id == "dev") label = tr("Developer folders (node_modules, Python caches)");
        else if (p.id == "clutter") label = tr("Windows clutter (thumbnails, page file)");
        else if (p.id == "recycle") label = tr("Recycle bin");
        else label = qs(p.id);
        for (const std::string& pattern : p.patterns) tip += (tip.isEmpty() ? "" : "\n") + qs(pattern);
        auto* box = new QCheckBox(label);
        box->setToolTip(tip);
        presets->addWidget(box, n / 2, n % 2);
        preset_boxes_.push_back(box);
        ++n;
    }
    layout->addLayout(presets);

    layout->addWidget(new QLabel(tr("Also skip names matching (wildcards * and ?; a pattern with a path "
                                    "separator matches the full path):")));
    excludes_ = new QListWidget;
    layout->addWidget(excludes_, 1);
    auto* add_ex = new QPushButton(tr("Add pattern..."));
    auto* remove_ex = new QPushButton(tr("Remove"));
    connect(add_ex, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString p = QInputDialog::getText(this, tr("Skip pattern"), tr("Pattern, e.g. *.tmp or node_modules:"),
                                                QLineEdit::Normal, QString(), &ok);
        if (ok && !p.trimmed().isEmpty()) excludes_->addItem(p.trimmed());
    });
    connect(remove_ex, &QPushButton::clicked, this, [this] { qDeleteAll(excludes_->selectedItems()); });
    layout->addWidget(row({add_ex, remove_ex}));
    layout = outer;  // what follows applies to both kinds

#ifdef _WIN32
    vss_ = new QCheckBox(tr("Back up open and locked files too (Volume Shadow Copy; only when Vaultik "
                            "runs with administrator rights, otherwise such files are skipped)"));
    layout->addWidget(vss_);
    skip_sql_ = new QCheckBox(tr("Do not pause SQL Server during the shadow copy (its database files are then "
                                 "saved as after a power cut, which SQL Server recovers from)"));
    auto* skip_row = new QHBoxLayout;
    skip_row->addSpacing(20);
    skip_row->addWidget(skip_sql_, 1);
    layout->addLayout(skip_row);
    connect(vss_, &QCheckBox::toggled, skip_sql_, &QWidget::setEnabled);
#endif

    // Commands around the backup (scheduled and manual runs).
    auto* commands = new QFormLayout;
    before_command_ = new QLineEdit;
    before_command_->setPlaceholderText(tr("Optional, e.g. a database dump"));
    after_command_ = new QLineEdit;
    after_command_->setPlaceholderText(tr("Optional, e.g. starting a stopped program again"));
    commands->addRow(tr("Before the backup, run:"), before_command_);
    commands->addRow(tr("After it, run:"), after_command_);
    layout->addLayout(commands);
    auto* commands_note = new QLabel(tr("Command lines as in a command prompt. When the first one fails (an exit "
                                        "code other than 0), nothing is backed up. The second runs after the "
                                        "backup whatever happened; VAULTIK_RESULT tells it the result (success, "
                                        "partial, failed or cancelled). They run with the job's rights."));
    commands_note->setWordWrap(true);
    commands_note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(commands_note);
    return page;
}

void JobDialog::update_on_connect() {
    const std::string repo = u8(repository_->text().trimmed());
    QString why;
    if (repo.empty()) {
        why = tr("Choose a destination folder first.");
    } else if (admin_ != nullptr && admin_->isChecked()) {
        why = tr("Not for a job with administrator rights: it would run without them when the drive is connected.");
    } else if (is_cloud_location(repo) || repo.starts_with("\\\\") || repo.starts_with("//")) {
        why = tr("Only for a destination on a drive connected to this computer.");
    } else {
        const os::VolumeLocation loc = os::volume_of(repo);
        const bool known = !job_.repository_volume.empty() && repo == job_.repository;  // recorded before
        if (loc.volume.empty() && !known)
            why = tr("The destination drive is not connected now. Connect it to turn this on.");
        else if (loc.system)
            why = tr("The destination is on the Windows drive, which is always connected.");
    }
    on_connect_->setEnabled(why.isEmpty());
    if (!why.isEmpty()) on_connect_->setChecked(false);
    on_connect_note_->setText(
        why.isEmpty() ? tr("Vaultik recognises the drive itself, also under another drive letter, and tells you when "
                           "it can be unplugged. Vaultik needs to be running, e.g. in the notification area (Settings, "
                           "Start with Windows).")
                      : why);
    // Sign-out and continuous backups start from the user's session too.
    if (on_sign_out_ == nullptr) return;
    const bool admin = admin_ != nullptr && admin_->isChecked();
    on_sign_out_->setEnabled(!admin);
    continuous_->setEnabled(!admin);
    if (admin) {
        on_sign_out_->setChecked(false);
        continuous_->setChecked(false);
    }
    events_note_->setText(admin ? tr("Not when you sign out, nor continuously, for a job with administrator rights: "
                                     "those backups would run without them.")
                                : tr("Signing out and continuous backups need Vaultik running, e.g. in the "
                                     "notification area (Settings, Start with Windows). Continuous backups do not "
                                     "count changes in excluded folders."));
}

QWidget* JobDialog::build_schedule() {
    auto* page = new QWidget;
    auto* form = new QFormLayout(page);

    schedule_kind_ = combo({tr("Manually only"), tr("Every few hours"), tr("Daily"), tr("On selected days")});
    form->addRow(tr("Run:"), schedule_kind_);

    time_ = new QTimeEdit;
    time_->setDisplayFormat(QStringLiteral("HH:mm"));
    time_row_ = row({time_});
    form->addRow(tr("At:"), time_row_);

    every_hours_ = spin(1, 23, tr(" h"), QString());
    minute_ = spin(0, 59, QString(), QString());
    hourly_row_ = row({new QLabel(tr("every")), every_hours_, new QLabel(tr("at minute")), minute_});
    form->addRow(QString(), hourly_row_);

    const QString names[] = {tr("Mon"), tr("Tue"), tr("Wed"), tr("Thu"), tr("Fri"), tr("Sat"), tr("Sun")};
    days_row_ = new QWidget;
    auto* dl = new QHBoxLayout(days_row_);
    dl->setContentsMargins(0, 0, 0, 0);
    for (int d = 0; d < 7; ++d) {
        days_[d] = new QCheckBox(names[d]);
        dl->addWidget(days_[d]);
    }
    dl->addStretch();
    form->addRow(tr("Days:"), days_row_);

    // Also when the destination drive is plugged in (an external disk that is not always there).
    // Conditions for scheduled runs; "Back up now" always runs.
    skip_battery_ = new QCheckBox(tr("Not while the computer runs on battery"));
    wake_ = new QCheckBox(tr("Wake the computer from sleep to back up"));
    skip_metered_ = new QCheckBox(tr("Cloud storage: not over a metered connection (mobile data)"));
    skip_metered_->setToolTip(tr("A skipped run shows in the Activity tab; the next scheduled run tries again."));
    skip_unchanged_ = new QCheckBox(tr("Only save a version when something changed"));
    skip_unchanged_->setToolTip(tr("A run that finds every file as it was shows in the Activity tab as \"nothing "
                                   "changed\" and still counts as a backup; no new version is added."));
    form->addRow(tr("Versions:"), skip_unchanged_);
    check_every_file_ = new QCheckBox(tr("Check every file, not only the folder listing (slower)"));
    check_every_file_->setToolTip(
        tr("Windows' folder listing can show a file with several names (a hard link) as unchanged when it "
           "was changed through its other name. Vaultik checks files it knows have several names anyway; "
           "this also checks every other file, for folders where hard links come and go, such as package "
           "stores."));
    form->addRow(QString(), check_every_file_);
    record_apps_ = new QCheckBox(tr("Keep a list of the installed programs with each version"));
    record_apps_->setToolTip(tr("On a new computer, \"Reinstall programs\" in the restore window then installs the "
                                "missing ones with winget. The list is small and stored encrypted like the files."));
    form->addRow(QString(), record_apps_);
    repair_data_ = new QComboBox;
    repair_data_->addItem(tr("Off"), 0);
    repair_data_->addItem(tr("5% of the backup's size (recommended for drives)"), 5);
    repair_data_->addItem(tr("10%"), 10);
    repair_data_->addItem(tr("20%"), 20);
    repair_data_->setToolTip(tr("Stores extra repair data next to the backup, so data damaged by bad sectors on the "
                                "backup drive can be rebuilt by Repair. Data saved before it is turned on gets it "
                                "during the health checks."));
    form->addRow(tr("Repair data:"), repair_data_);
    form->addRow(tr("Scheduled runs:"), skip_battery_);
    form->addRow(QString(), wake_);
    form->addRow(QString(), skip_metered_);

    on_connect_ = new QCheckBox(tr("Also back up when the destination drive is connected"));
    form->addRow(QString(), on_connect_);
    on_connect_note_ = new QLabel;
    on_connect_note_->setWordWrap(true);
    on_connect_note_->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), on_connect_note_);
    connect(repository_, &QLineEdit::textChanged, this, &JobDialog::update_on_connect);

    // Backups started by something happening rather than a time.
    on_idle_ = new QCheckBox(tr("When the computer has been idle for 10 minutes"));
    on_lock_ = new QCheckBox(tr("When I lock the computer"));
    on_sign_out_ = new QCheckBox(tr("When I sign out or shut down"));
    on_sign_out_->setToolTip(tr("Windows waits for the backup and says why; you can also go on without waiting."));
    on_update_ = new QCheckBox(tr("When Windows starts installing updates"));
    on_update_->setToolTip(tr("Windows does not wait for the backup: it installs the update meanwhile. Vaultik in "
                              "the notification area holds up the restart that follows until the backup is done. "
                              "Defender definitions and Store apps do not start a backup."));
    event_gap_ = spin(1, 168, tr(" h"), QString());
#ifdef _WIN32
    form->addRow(tr("Also back up:"), on_idle_);
    form->addRow(QString(), on_lock_);
    form->addRow(QString(), on_sign_out_);
    form->addRow(QString(), on_update_);
    form->addRow(QString(), row({new QLabel(tr("but not again within")), event_gap_, new QLabel(tr("of the last backup"))}));
    const QString continuous_label;
#else
    // Idle, lock and sign-out are Task Scheduler and Windows session events; Linux has no
    // counterpart that works for every desktop, so these stay off (and hidden) there.
    for (QWidget* w : std::initializer_list<QWidget*>{on_idle_, on_lock_, on_sign_out_, event_gap_}) {
        w->setParent(this);
        w->hide();
    }
    const QString continuous_label = tr("Also back up:");
#endif
    continuous_ = new QCheckBox(tr("Continuously,"));
    continuous_minutes_ = spin(1, 1440, tr(" min"), QString());
    form->addRow(continuous_label,
                 row({continuous_, continuous_minutes_, new QLabel(tr("after files in the job's folders change"))}));
    events_note_ = new QLabel;
    events_note_->setWordWrap(true);
    events_note_->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), events_note_);
    const auto update_events = [this] {
        event_gap_->setEnabled(on_idle_->isChecked() || on_lock_->isChecked() || on_sign_out_->isChecked() ||
                               on_update_->isChecked());
        continuous_minutes_->setEnabled(continuous_->isChecked());
    };
    for (QCheckBox* box : {on_idle_, on_lock_, on_sign_out_, on_update_, continuous_})
        connect(box, &QCheckBox::toggled, this, update_events);
    update_events();

    // Health checks: after a scheduled or manual backup, when one is due.
    check_weekly_ = new QCheckBox(tr("Once a week, read back"));
    check_percent_ = spin(1, 100, QStringLiteral("%"), QString());
    form->addRow(tr("Check:"), row({check_weekly_, check_percent_, new QLabel(tr("of the stored data"))}));
    check_monthly_ = new QCheckBox(tr("Once a month, read back all of it"));
    form->addRow(QString(), check_monthly_);
    check_restore_ = new QCheckBox(tr("Once a month, test a restore of a few files"));
    check_restore_->setToolTip(tr("Restores up to 20 files (at most 500 MB) of the newest version into a temporary "
                                  "folder, checks them, compares them with the unchanged files on this PC and "
                                  "removes them again. Any time: Test a restore on the job's Overview."));
    form->addRow(QString(), check_restore_);
    auto* check_note = new QLabel(tr("A check runs after a backup when it is due. It reads stored data back and "
                                     "compares it with what was saved, so a failing disk or damaged files are "
                                     "found while the originals still exist. Each week reads a different part. "
                                     "From cloud storage, checks are downloads; the monthly one downloads "
                                     "everything."));
    check_note->setWordWrap(true);
    check_note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), check_note);
    connect(check_weekly_, &QCheckBox::toggled, check_percent_, &QWidget::setEnabled);
    connect(check_monthly_, &QCheckBox::clicked, this, [this] { check_monthly_touched_ = true; });
    connect(repository_, &QLineEdit::textChanged, this, [this](const QString& text) {
        if (!editing_ && !check_monthly_touched_)
            check_monthly_->setChecked(jobs::default_health(u8(text.trimmed())).monthly);
    });

#ifdef _WIN32
    admin_ = new QCheckBox(tr("Run with administrator rights"));
    form->addRow(QString(), admin_);
    connect(admin_, &QCheckBox::toggled, this, &JobDialog::update_on_connect);
    auto* admin_note = new QLabel(tr("Lets scheduled runs back up open and locked files through a shadow copy. "
                                     "Scheduled runs use the system account, so they also run when nobody is "
                                     "signed in, but cannot reach network shares. "
                                     "Windows asks for administrator approval each time the job is saved. "
                                     "Vaultik must be installed in a folder only administrators can change, "
                                     "such as C:\\Program Files\\Vaultik."));
    admin_note->setWordWrap(true);
    admin_note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(QString(), admin_note);
#endif

    auto* note = new QLabel(tr("Scheduled runs use the system scheduler, so Vaultik does not need to stay "
                               "open. A run missed while the computer was off starts at the next chance."));
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    form->addRow(note);

    connect(schedule_kind_, &QComboBox::currentIndexChanged, this, &JobDialog::update_schedule_fields);
    return page;
}

QWidget* JobDialog::build_resources() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* top = new QFormLayout;
    preset_ = combo({tr("Background (only use idle resources)"), tr("Low (stay out of the way)"),
                     tr("Normal"), tr("High (finish fast)"), tr("Custom")});
    top->addRow(tr("Priority:"), preset_);
    layout->addLayout(top);

    auto* prio = new QGroupBox(tr("Priorities (matter only when the computer is busy)"));
    auto* pf = new QFormLayout(prio);
    cpu_ = combo({tr("Idle"), tr("Low"), tr("Normal"), tr("High")});
    io_ = combo({tr("Background"), tr("Low"), tr("Normal")});
    ram_ = combo({tr("Low"), tr("Normal")});
    pf->addRow(tr("CPU:"), cpu_);
    pf->addRow(tr("Disk and network:"), io_);
    pf->addRow(tr("Memory:"), ram_);
    layout->addWidget(prio);

    auto* limits = new QGroupBox(tr("Limits (always apply)"));
    auto* lf = new QFormLayout(limits);
    cpu_limit_ = spin(0, 100, tr(" %"), tr("No limit"));
    max_memory_ = spin(0, 65536, tr(" MB"), tr("No limit"));
    max_memory_->setSingleStep(64);
    read_limit_ = spin(0, 10000, tr(" MB/s"), tr("No limit"));
    write_limit_ = spin(0, 10000, tr(" MB/s"), tr("No limit"));
    lf->addRow(tr("CPU use at most:"), cpu_limit_);
    lf->addRow(tr("Memory at most:"), max_memory_);
    lf->addRow(tr("Read speed at most:"), read_limit_);
    lf->addRow(tr("Write speed at most:"), write_limit_);
    layout->addWidget(limits);

    // Slower while people work: weekdays between two times.
    work_box_ = new QGroupBox(tr("Slower during working hours"));
    work_box_->setCheckable(true);
    auto* wf = new QFormLayout(work_box_);
    work_start_ = new QTimeEdit;
    work_end_ = new QTimeEdit;
    for (QTimeEdit* t : {work_start_, work_end_}) t->setDisplayFormat(QStringLiteral("HH:mm"));
    wf->addRow(tr("On weekdays from:"), row({work_start_, new QLabel(tr("to")), work_end_}));
    work_read_ = spin(0, 10000, tr(" MB/s"), tr("No change"));
    work_write_ = spin(0, 10000, tr(" MB/s"), tr("No change"));
    wf->addRow(tr("Read speed at most:"), work_read_);
    wf->addRow(tr("Write and upload speed at most:"), work_write_);
    auto* work_note = new QLabel(tr("A backup that runs into these hours slows down, and speeds up again "
                                    "when they end."));
    work_note->setWordWrap(true);
    work_note->setForegroundRole(QPalette::PlaceholderText);
    wf->addRow(work_note);
    layout->addWidget(work_box_);
    layout->addStretch();

    connect(preset_, &QComboBox::activated, this, &JobDialog::apply_preset);
    for (QComboBox* c : {cpu_, io_, ram_})
        connect(c, &QComboBox::activated, this, [this] { preset_->setCurrentIndex(4); });
    return page;
}

void JobDialog::apply_preset(int index) {
    if (index < 0 || index > 3) return;
    const ResourcePolicy p = priority_preset(kPresets[index]);
    cpu_->setCurrentIndex(static_cast<int>(p.cpu));
    io_->setCurrentIndex(static_cast<int>(p.io));
    ram_->setCurrentIndex(static_cast<int>(p.memory));
}

void JobDialog::update_schedule_fields() {
    const auto kind = static_cast<jobs::Schedule::Kind>(schedule_kind_->currentIndex());
    time_row_->setEnabled(kind == jobs::Schedule::Kind::Daily || kind == jobs::Schedule::Kind::Weekly);
    hourly_row_->setEnabled(kind == jobs::Schedule::Kind::Hourly);
    days_row_->setEnabled(kind == jobs::Schedule::Kind::Weekly);
}

namespace {

// Index order of the "keep" combo box.
enum KeepMode { kSmart, kNewest, kDays, kCustom, kEverything };

bool is_smart(const RetentionPolicy& p) {
    const RetentionPolicy d = jobs::default_retention();
    return p.last == 0 && p.hourly == 0 && p.yearly == 0 && p.within_days == 0 && p.daily == d.daily &&
           p.weekly == d.weekly && p.monthly == d.monthly;
}

}  // namespace

QWidget* JobDialog::build_retention() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    keep_mode_ = combo({tr("Smart (recommended)"), tr("Keep the newest versions"), tr("Keep versions for a number of days"),
                        tr("Custom rules"), tr("Keep everything")});
    auto* form = new QFormLayout;
    form->addRow(tr("Old versions:"), keep_mode_);
    layout->addLayout(form);

    keep_pages_ = new QStackedWidget;
    // Smart: nothing to set.
    keep_pages_->addWidget(new QWidget);
    // Newest N.
    keep_count_ = spin(1, 10000, QString(), QString());
    keep_pages_->addWidget(row({new QLabel(tr("Keep the newest")), keep_count_, new QLabel(tr("versions"))}));
    // N days.
    keep_days_ = spin(1, 10000, QString(), QString());
    keep_pages_->addWidget(row({new QLabel(tr("Keep every version from the last")), keep_days_, new QLabel(tr("days"))}));
    // Custom.
    auto* custom = new QWidget;
    auto* cf = new QFormLayout(custom);
    cf->setContentsMargins(0, 0, 0, 0);
    const QString off = tr("off");
    keep_within_ = spin(0, 10000, QString(), off);
    keep_last_ = spin(0, 1000, QString(), off);
    keep_hourly_ = spin(0, 1000, QString(), off);
    keep_daily_ = spin(0, 1000, QString(), off);
    keep_weekly_ = spin(0, 1000, QString(), off);
    keep_monthly_ = spin(0, 1000, QString(), off);
    keep_yearly_ = spin(0, 1000, QString(), off);
    cf->addRow(tr("Keep every version from the last:"), row({keep_within_, new QLabel(tr("days"))}));
    cf->addRow(tr("Keep the newest:"), row({keep_last_, new QLabel(tr("versions"))}));
    cf->addRow(tr("Keep one per hour for:"), row({keep_hourly_, new QLabel(tr("hours"))}));
    cf->addRow(tr("Keep one per day for:"), row({keep_daily_, new QLabel(tr("days"))}));
    cf->addRow(tr("Keep one per week for:"), row({keep_weekly_, new QLabel(tr("weeks"))}));
    cf->addRow(tr("Keep one per month for:"), row({keep_monthly_, new QLabel(tr("months"))}));
    cf->addRow(tr("Keep one per year for:"), row({keep_yearly_, new QLabel(tr("years"))}));
    keep_pages_->addWidget(custom);
    // Everything.
    keep_pages_->addWidget(new QWidget);
    layout->addWidget(keep_pages_);

    size_limit_ = new QCheckBox(tr("Keep the backup under"));
    size_limit_gb_ = spin(1, 1000000, tr(" GB"), QString());
    size_limit_gb_->setValue(500);
    layout->addWidget(row({size_limit_, size_limit_gb_}));
    auto* size_note = new QLabel(tr("When the backup grows past this, the oldest versions are removed first. The "
                                    "newest version and pinned versions always stay."));
    size_note->setWordWrap(true);
    size_note->setProperty("role", "faint");
    layout->addWidget(size_note);

    keep_summary_ = new QLabel;
    keep_summary_->setWordWrap(true);
    keep_summary_->setProperty("role", "muted");
    layout->addSpacing(8);
    layout->addWidget(keep_summary_);

    auto* note = new QLabel(tr("Every version is a complete backup, so removing one never breaks the others. "
                               "Old versions are removed after each run, and the space only they used is freed. "
                               "Pin a version in the Versions tab to keep it whatever these rules say."));
    note->setWordWrap(true);
    note->setProperty("role", "faint");
    layout->addWidget(note);
    layout->addStretch();

    connect(keep_mode_, &QComboBox::currentIndexChanged, this, &JobDialog::update_retention_fields);
    connect(size_limit_, &QCheckBox::toggled, this, &JobDialog::update_retention_fields);
    for (QSpinBox* s : {keep_count_, keep_days_, keep_within_, keep_last_, keep_hourly_, keep_daily_, keep_weekly_,
                        keep_monthly_, keep_yearly_, size_limit_gb_})
        connect(s, &QSpinBox::valueChanged, this, &JobDialog::update_retention_fields);
    return page;
}

RetentionPolicy JobDialog::collect_retention() const {
    RetentionPolicy p;
    switch (keep_mode_->currentIndex()) {
        case kSmart: p = jobs::default_retention(); break;
        case kNewest: p.last = keep_count_->value(); break;
        case kDays: p.within_days = keep_days_->value(); break;
        case kCustom:
            p.within_days = keep_within_->value();
            p.last = keep_last_->value();
            p.hourly = keep_hourly_->value();
            p.daily = keep_daily_->value();
            p.weekly = keep_weekly_->value();
            p.monthly = keep_monthly_->value();
            p.yearly = keep_yearly_->value();
            break;
        default: break;
    }
    if (size_limit_->isChecked()) p.max_bytes = static_cast<std::uint64_t>(size_limit_gb_->value()) << 30;
    return p;
}

void JobDialog::update_retention_fields() {
    keep_pages_->setCurrentIndex(keep_mode_->currentIndex());
    // Only the page in use takes room.
    for (int i = 0; i < keep_pages_->count(); ++i)
        keep_pages_->widget(i)->setSizePolicy(QSizePolicy::Preferred,
                                              i == keep_pages_->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    keep_pages_->adjustSize();
    size_limit_gb_->setEnabled(size_limit_->isChecked());
    keep_summary_->setText(retention_summary(collect_retention()));
}

void JobDialog::load(const jobs::Job& job) {
    name_->setText(qs(job.name));
    repository_->setText(qs(job.repository));
    cloud_.wipe();
    cloud_ = CloudTarget{};
    if (is_cloud_location(job.repository)) {
        cloud_.location = qs(job.repository);
        cloud_.key_id = qs(job.cloud_key_id);
        cloud_.region = qs(job.cloud_region);
        cloud_.lock_days = job.cloud_lock_days;
        cloud_.lock_compliance = job.cloud_lock_compliance;
        cloud_.storage_class = qs(job.cloud_storage_class);
        if (!job.id.empty()) {
            try {
                std::string secret = store_.cloud_secret(job.id);
                cloud_.secret = qs(secret);
                secure_wipe(secret.data(), secret.size());
            } catch (const Error&) {
                // asked for again when the job is saved
            }
        }
    }
    const jobs::SecondCopy& sc = job.second_copy;
    copy_repository_->setText(qs(sc.repository));
    copy_cloud_.wipe();
    copy_cloud_ = CloudTarget{};
    if (is_cloud_location(sc.repository)) {
        copy_cloud_.location = qs(sc.repository);
        copy_cloud_.key_id = qs(sc.cloud_key_id);
        copy_cloud_.region = qs(sc.cloud_region);
        copy_cloud_.lock_days = sc.cloud_lock_days;
        copy_cloud_.lock_compliance = sc.cloud_lock_compliance;
        copy_cloud_.storage_class = qs(sc.cloud_storage_class);
        if (!job.id.empty()) {
            try {
                std::string secret = store_.copy_secret(job.id);
                copy_cloud_.secret = qs(secret);
                secure_wipe(secret.data(), secret.size());
            } catch (const Error&) {
            }
        }
    }
    sources_->clear();
    for (const auto& s : job.sources) sources_->addItem(qs(s));
    // A preset is ticked when the job has all its patterns; the list shows the others.
    excludes_->clear();
    std::set<std::string> covered;
    const auto& presets = jobs::exclusion_presets();
    for (std::size_t i = 0; i < presets.size(); ++i) {
        const bool all = std::all_of(presets[i].patterns.begin(), presets[i].patterns.end(), [&](const std::string& p) {
            return std::find(job.excludes.begin(), job.excludes.end(), p) != job.excludes.end();
        });
        preset_boxes_[i]->setChecked(all);
        if (all) covered.insert(presets[i].patterns.begin(), presets[i].patterns.end());
    }
    for (const auto& e : job.excludes)
        if (!covered.contains(e)) excludes_->addItem(qs(e));
    if (vss_ != nullptr) vss_->setChecked(job.vss);
    if (skip_sql_ != nullptr) {
        skip_sql_->setChecked(job.vss_skip_sql);
        skip_sql_->setEnabled(job.vss);
    }
    if (admin_ != nullptr) admin_->setChecked(job.run_as_admin);
    fill_disks(job.image_disks);
    if (kind_ != nullptr) kind_->setCurrentIndex(job.image_disks.empty() ? 0 : 1);
    update_kind();
    update_on_connect();
    if (wsl_list_ != nullptr) {
        wsl_list_->clear();
        const std::vector<WslDistro> installed = wsl_distros();
        for (const WslDistro& d : installed) {
            const bool chosen = std::any_of(job.wsl_distros.begin(), job.wsl_distros.end(), [&](const std::string& n) {
                return os::fold_case(n) == os::fold_case(d.name);
            });
            add_machine_item(wsl_list_, qs(d.name), d.disk.empty() ? qs(d.folder) : qs(d.disk), chosen);
        }
        for (const std::string& n : job.wsl_distros) {
            const bool there = std::any_of(installed.begin(), installed.end(), [&](const WslDistro& d) {
                return os::fold_case(n) == os::fold_case(d.name);
            });
            if (!there) add_machine_item(wsl_list_, qs(n), tr("not installed"), true);
        }
        if (wsl_list_->count() == 0) {
            auto* none = new QListWidgetItem(tr("No WSL distributions are installed for your account."), wsl_list_);
            none->setFlags(Qt::NoItemFlags);
        }
        const int at = wsl_running_->findData(qs(job.wsl_when_running));
        wsl_running_->setCurrentIndex(at < 0 ? 0 : at);
        hyperv_list_->clear();
        for (const std::string& n : job.hyperv_vms) add_machine_item(hyperv_list_, qs(n), QString(), true);
        hyperv_staging_->setText(qs(job.hyperv_staging));
    }
    if (on_connect_->isEnabled()) on_connect_->setChecked(job.backup_on_connect);
    check_weekly_->setChecked(job.health.weekly);
    check_percent_->setValue(job.health.sample_percent);
    check_percent_->setEnabled(job.health.weekly);
    check_monthly_->setChecked(job.health.monthly);
    check_restore_->setChecked(job.health.restore_test);
    before_command_->setText(qs(job.before_command));
    after_command_->setText(qs(job.after_command));

    schedule_kind_->setCurrentIndex(static_cast<int>(job.schedule.kind));
    time_->setTime(QTime(job.schedule.hour, job.schedule.minute));
    every_hours_->setValue(job.schedule.every_hours);
    minute_->setValue(job.schedule.minute);
    for (int d = 0; d < 7; ++d) days_[d]->setChecked((job.schedule.weekdays >> d) & 1u);
    skip_battery_->setChecked(job.schedule.skip_on_battery);
    skip_unchanged_->setChecked(job.skip_unchanged);
    check_every_file_->setChecked(job.check_every_file);
    record_apps_->setChecked(job.record_apps);
    {
        int at = repair_data_->findData(static_cast<int>(job.repair_data));
        if (at < 0) {
            repair_data_->addItem(tr("%1%").arg(job.repair_data), static_cast<int>(job.repair_data));
            at = repair_data_->count() - 1;
        }
        repair_data_->setCurrentIndex(at);
    }
    wake_->setChecked(job.schedule.wake_computer);
    skip_metered_->setChecked(job.schedule.skip_metered);
    on_idle_->setChecked(job.schedule.on_idle);
    on_lock_->setChecked(job.schedule.on_lock);
    on_sign_out_->setChecked(job.schedule.on_sign_out);
    on_update_->setChecked(job.schedule.on_update);
    event_gap_->setValue(job.schedule.event_gap_hours);
    continuous_->setChecked(job.continuous);
    continuous_minutes_->setValue(job.continuous_minutes);
    update_schedule_fields();

    const ResourcePolicy& r = job.resources;
    cpu_->setCurrentIndex(static_cast<int>(r.cpu));
    io_->setCurrentIndex(static_cast<int>(r.io));
    ram_->setCurrentIndex(static_cast<int>(r.memory));
    preset_->setCurrentIndex(4);
    for (int i = 0; i < 4; ++i) {
        const ResourcePolicy p = priority_preset(kPresets[i]);
        if (p.cpu == r.cpu && p.io == r.io && p.memory == r.memory) preset_->setCurrentIndex(i);
    }
    cpu_limit_->setValue(static_cast<int>(r.cpu_limit_percent));
    max_memory_->setValue(static_cast<int>(r.max_memory >> 20));
    read_limit_->setValue(static_cast<int>(r.read_rate >> 20));
    write_limit_->setValue(static_cast<int>(r.write_rate >> 20));
    const jobs::WorkHours& w = job.work_hours;
    work_box_->setChecked(w.enabled);
    work_start_->setTime(QTime(w.start_minute / 60, w.start_minute % 60));
    work_end_->setTime(QTime(w.end_minute / 60, w.end_minute % 60));
    work_read_->setValue(static_cast<int>(w.read_rate >> 20));
    work_write_->setValue(static_cast<int>(w.write_rate >> 20));

    // The preset that matches the rules; the custom fields start from the rules (or the default).
    const RetentionPolicy& k = job.retention;
    if (k.no_rules()) keep_mode_->setCurrentIndex(kEverything);
    else if (is_smart(k)) keep_mode_->setCurrentIndex(kSmart);
    else if (k.last > 0 && k.hourly == 0 && k.daily == 0 && k.weekly == 0 && k.monthly == 0 && k.yearly == 0 && k.within_days == 0)
        keep_mode_->setCurrentIndex(kNewest);
    else if (k.within_days > 0 && k.last == 0 && k.hourly == 0 && k.daily == 0 && k.weekly == 0 && k.monthly == 0 && k.yearly == 0)
        keep_mode_->setCurrentIndex(kDays);
    else keep_mode_->setCurrentIndex(kCustom);
    keep_count_->setValue(k.last > 0 ? k.last : 30);
    keep_days_->setValue(k.within_days > 0 ? k.within_days : 90);
    size_limit_->setChecked(k.max_bytes > 0);
    if (k.max_bytes > 0) size_limit_gb_->setValue(static_cast<int>(std::max<std::uint64_t>(1, k.max_bytes >> 30)));
    const RetentionPolicy c = k.no_rules() ? jobs::default_retention() : k;
    keep_within_->setValue(c.within_days);
    keep_last_->setValue(c.last);
    keep_hourly_->setValue(c.hourly);
    keep_daily_->setValue(c.daily);
    keep_weekly_->setValue(c.weekly);
    keep_monthly_->setValue(c.monthly);
    keep_yearly_->setValue(c.yearly);
    update_retention_fields();
}

jobs::Job JobDialog::collect() const {
    jobs::Job j = job_;  // keeps id and tags
    j.name = u8(name_->text().trimmed());
    j.repository = u8(repository_->text().trimmed());
    if (is_cloud_location(j.repository)) {
        j.cloud_key_id = u8(cloud_.key_id);
        j.cloud_region = u8(cloud_.region);
        j.cloud_lock_days = cloud_kind(j.repository) == CloudKind::S3 ? cloud_.lock_days : 0;
        j.cloud_lock_compliance = j.cloud_lock_days > 0 && cloud_.lock_compliance;
        j.cloud_storage_class = cloud_kind(j.repository) == CloudKind::S3 ? u8(cloud_.storage_class) : std::string();
    } else {
        j.cloud_key_id.clear();
        j.cloud_region.clear();
        j.cloud_lock_days = 0;
        j.cloud_storage_class.clear();
    }
    jobs::SecondCopy& sc = j.second_copy;
    sc = jobs::SecondCopy{};
    sc.repository = u8(copy_repository_->text().trimmed());
    if (is_cloud_location(sc.repository)) {
        sc.cloud_key_id = u8(copy_cloud_.key_id);
        sc.cloud_region = u8(copy_cloud_.region);
        sc.cloud_lock_days = cloud_kind(sc.repository) == CloudKind::S3 ? copy_cloud_.lock_days : 0;
        sc.cloud_lock_compliance = sc.cloud_lock_days > 0 && copy_cloud_.lock_compliance;
        sc.cloud_storage_class = cloud_kind(sc.repository) == CloudKind::S3 ? u8(copy_cloud_.storage_class) : std::string();
    } else if (!sc.repository.empty()) {
        sc.repository = os::absolute_path(sc.repository);
    }
    j.sources.clear();
    for (const QString& s : items_of(sources_)) j.sources.push_back(u8(s));
    j.excludes.clear();
    const auto& presets = jobs::exclusion_presets();
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (preset_boxes_[i]->isChecked())
            for (const std::string& p : presets[i].patterns)
                if (std::find(j.excludes.begin(), j.excludes.end(), p) == j.excludes.end()) j.excludes.push_back(p);
    for (const QString& e : items_of(excludes_))
        if (std::find(j.excludes.begin(), j.excludes.end(), u8(e)) == j.excludes.end()) j.excludes.push_back(u8(e));
    if (vss_ != nullptr) j.vss = vss_->isChecked();
    if (skip_sql_ != nullptr) j.vss_skip_sql = skip_sql_->isChecked();
    j.before_command = u8(before_command_->text().trimmed());
    j.after_command = u8(after_command_->text().trimmed());
    if (admin_ != nullptr) j.run_as_admin = admin_->isChecked();
    if (wsl_list_ != nullptr) {
        j.wsl_distros.clear();
        for (int i = 0; i < wsl_list_->count(); ++i) {
            const QListWidgetItem* item = wsl_list_->item(i);
            if ((item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked)
                j.wsl_distros.push_back(u8(item->data(Qt::UserRole).toString()));
        }
        j.wsl_when_running = u8(wsl_running_->currentData().toString());
        j.hyperv_vms.clear();
        for (int i = 0; i < hyperv_list_->count(); ++i) {
            const QListWidgetItem* item = hyperv_list_->item(i);
            if ((item->flags() & Qt::ItemIsUserCheckable) && item->checkState() == Qt::Checked)
                j.hyperv_vms.push_back(u8(item->data(Qt::UserRole).toString()));
        }
        j.hyperv_staging = u8(QDir::toNativeSeparators(hyperv_staging_->text().trimmed()));
    }
    j.image_disks.clear();
    if (kind_ != nullptr && kind_->currentIndex() == 1) {
        j.sources.clear();
        j.run_as_admin = true;
        for (int i = 0; i < disks_->count(); ++i)
            if (disks_->item(i)->checkState() == Qt::Checked) j.image_disks.push_back(u8(disks_->item(i)->data(Qt::UserRole).toString()));
        if (j.image_disks.empty()) j.image_disks.push_back("");  // validate says to pick one
    }

    j.schedule.kind = static_cast<jobs::Schedule::Kind>(schedule_kind_->currentIndex());
    if (j.schedule.kind == jobs::Schedule::Kind::Hourly) {
        j.schedule.every_hours = every_hours_->value();
        j.schedule.minute = minute_->value();
    } else {
        j.schedule.hour = time_->time().hour();
        j.schedule.minute = time_->time().minute();
    }
    j.schedule.weekdays = 0;
    for (int d = 0; d < 7; ++d)
        if (days_[d]->isChecked()) j.schedule.weekdays |= static_cast<std::uint8_t>(1u << d);
    j.schedule.skip_on_battery = skip_battery_->isChecked();
    j.skip_unchanged = skip_unchanged_->isChecked();
    j.check_every_file = check_every_file_->isChecked();
    j.record_apps = record_apps_->isChecked();
    j.repair_data = static_cast<unsigned>(repair_data_->currentData().toInt());
    j.schedule.wake_computer = wake_->isChecked();
    j.schedule.skip_metered = skip_metered_->isChecked();
    j.schedule.on_idle = on_idle_->isChecked();
    j.schedule.on_lock = on_lock_->isChecked();
    j.schedule.on_sign_out = on_sign_out_->isChecked();
    j.schedule.on_update = on_update_->isChecked();
    j.schedule.event_gap_hours = event_gap_->value();
    j.continuous = continuous_->isChecked();
    j.continuous_minutes = continuous_minutes_->value();

    ResourcePolicy& r = j.resources;
    r.cpu = static_cast<CpuPriority>(cpu_->currentIndex());
    r.io = static_cast<IoPriority>(io_->currentIndex());
    r.memory = static_cast<MemoryPriority>(ram_->currentIndex());
    r.cpu_limit_percent = static_cast<unsigned>(cpu_limit_->value());
    r.max_memory = static_cast<std::uint64_t>(max_memory_->value()) << 20;
    r.read_rate = static_cast<std::uint64_t>(read_limit_->value()) << 20;
    r.write_rate = static_cast<std::uint64_t>(write_limit_->value()) << 20;
    jobs::WorkHours& w = j.work_hours;
    w.enabled = work_box_->isChecked();
    w.start_minute = work_start_->time().hour() * 60 + work_start_->time().minute();
    w.end_minute = work_end_->time().hour() * 60 + work_end_->time().minute();
    w.read_rate = static_cast<std::uint64_t>(work_read_->value()) << 20;
    w.write_rate = static_cast<std::uint64_t>(work_write_->value()) << 20;

    j.retention = collect_retention();
    j.backup_on_connect = on_connect_->isChecked();
    j.health.weekly = check_weekly_->isChecked();
    j.health.sample_percent = check_percent_->value();
    j.health.monthly = check_monthly_->isChecked();
    j.health.restore_test = check_restore_->isChecked();
    return j;
}

bool JobDialog::prepare_repository(const jobs::Job& job, const std::string& password) {
    const bool cloud = is_cloud_location(job.repository);
    const std::string root = cloud ? job.repository : os::absolute_path(job.repository);
    CloudCredentials keys = cloud_.credentials();
    // OneDrive hands out a new sign-in token on use: the newest is what gets stored.
    keys.on_secret_changed = [this](const std::string& s) { cloud_.secret = qs(s); };
    std::unique_ptr<Backend> backend;
    bool has_repo = false;
    if (cloud) {
        backend = open_backend(root, &keys);
        has_repo = backend->exists("config");
    } else {
        has_repo = os::exists(os::join(root, "config"));
        if (has_repo) backend = open_backend(root);
    }
    if (has_repo) {
        // Check the password against the stored key without loading the whole index.
        try {
            const RepoConfig config = parse_config(backend->get("config"));
            unwrap_master_key(config, password);
        } catch (const AuthError&) {
            QMessageBox::warning(this, windowTitle(), tr("The password does not match the repository in %1.")
                                                          .arg(qs(root)));
            return false;
        }
        return true;
    }
    if (!cloud && os::exists(root) && os::is_directory(root)) {
        os::DirReader reader(root);
        std::string name;
        os::FileInfo info;
        if (reader.next(name, info)) {
            const auto answer = QMessageBox::question(
                this, windowTitle(),
                tr("%1 is not empty and holds no Vaultik repository. Create the repository in it anyway?")
                    .arg(qs(root)));
            if (answer != QMessageBox::Yes) return false;
        }
    }
    if (password.size() < 8) {
        QMessageBox::warning(this, windowTitle(), tr("Use a password of at least 8 characters."));
        return false;
    }
    if (!cloud) backend = open_backend(root, nullptr, /*create=*/true);
    Repository::init(*backend, password);
    const std::string key = create_recovery_key(*backend, {password});
    QApplication::restoreOverrideCursor();
    show_recovery_key(this, qs(key), qs(root));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    return true;
}

void JobDialog::accept() {
    jobs::Job j = collect();
    const std::string pw = u8(password_->text());
    if (pw != u8(password2_->text())) {
        QMessageBox::warning(this, windowTitle(), tr("The two passwords do not match."));
        return;
    }
    if (!editing_ && pw.empty()) {
        QMessageBox::warning(this, windowTitle(), tr("Enter the repository password."));
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        jobs::validate(j);
        std::string effective = pw;
        if (effective.empty()) effective = store_.password(j.id);  // editing, password unchanged
        const bool repo_changed = !editing_ || j.repository != job_.repository || !pw.empty();
        if (repo_changed && !prepare_repository(j, effective)) {
            QApplication::restoreOverrideCursor();
            return;
        }
        store_.save(j);
        if (!pw.empty()) store_.set_password(j.id, pw);
        {
            std::string secret = is_cloud_location(j.repository) ? u8(cloud_.secret) : std::string();
            store_.set_cloud_secret(j.id, secret);
            secure_wipe(secret.data(), secret.size());
            std::string copy = is_cloud_location(j.second_copy.repository) ? u8(copy_cloud_.secret) : std::string();
            store_.set_copy_secret(j.id, copy);
            secure_wipe(copy.data(), copy.size());
        }
        const QString dir = QFileInfo(Runner::engine_path()).absolutePath();
        try {
            jobs::apply_schedule(j, u8(QDir::toNativeSeparators(dir)));
        } catch (const Error& e) {
            QApplication::restoreOverrideCursor();
            QMessageBox::warning(this, windowTitle(),
                                 tr("The job was saved, but its schedule could not be set up:\n%1").arg(e.what()));
            job_ = j;
            QDialog::accept();
            return;
        }
    } catch (const Error& e) {
        QApplication::restoreOverrideCursor();
        plain_warning(this, windowTitle(), QString::fromUtf8(e.what()));
        return;
    }
    QApplication::restoreOverrideCursor();
    job_ = j;
    QDialog::accept();
}

void JobDialog::add_machine_item(QListWidget* list, const QString& name, const QString& detail, bool checked) {
    // One item per name: a listed machine replaces the placeholder of a chosen one.
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem* item = list->item(i);
        if (item->data(Qt::UserRole).toString().compare(name, Qt::CaseInsensitive) == 0) {
            if (!detail.isEmpty()) item->setText(detail.isEmpty() ? name : name + "   (" + detail + ")");
            if (checked) item->setCheckState(Qt::Checked);
            return;
        }
    }
    auto* item = new QListWidgetItem(detail.isEmpty() ? name : name + "   (" + detail + ")", list);
    item->setData(Qt::UserRole, name);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
}

QWidget* JobDialog::build_machines() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

    auto* wsl_box = new QGroupBox(tr("WSL distributions"));
    auto* wl = new QVBoxLayout(wsl_box);
    wsl_list_ = new QListWidget;
    wl->addWidget(wsl_list_, 1);
    wsl_running_ = new QComboBox;
    wsl_running_->addItem(tr("Leave it out this time (with a warning)"), QStringLiteral("skip"));
    wsl_running_->addItem(tr("Export it while it runs (files being written may be caught half-way)"),
                          QStringLiteral("export"));
    wsl_running_->addItem(tr("Stop it first (wsl --terminate; programs running in it are closed)"),
                          QStringLiteral("stop"));
    auto* running_row = new QFormLayout;
    running_row->addRow(tr("When a chosen one is running:"), wsl_running_);
    wl->addLayout(running_row);
    auto* wsl_note = new QLabel(tr("Each ticked distribution is exported with wsl --export straight into the backup, "
                                   "as WSL\\<name>.tar, and its disk file (ext4.vhdx) is left out of the file backup. "
                                   "Restore it with \"Import as new...\" in the restore window. WSL belongs to your "
                                   "own account, so this is for jobs without administrator rights."));
    wsl_note->setWordWrap(true);
    wsl_note->setForegroundRole(QPalette::PlaceholderText);
    wl->addWidget(wsl_note);
    layout->addWidget(wsl_box, 1);

    auto* hv_box = new QGroupBox(tr("Hyper-V virtual machines"));
    auto* hl = new QVBoxLayout(hv_box);
    hyperv_list_ = new QListWidget;
    hl->addWidget(hyperv_list_, 1);
    hyperv_note_ = new QLabel;
    hyperv_note_->setWordWrap(true);
    hyperv_note_->setTextFormat(Qt::PlainText);
    hl->addWidget(hyperv_note_);
    auto* refresh = new QPushButton(tr("List machines"));
    connect(refresh, &QPushButton::clicked, this, &JobDialog::list_hyperv);
    auto* by_name = new QPushButton(tr("Add by name..."));
    connect(by_name, &QPushButton::clicked, this, [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Virtual machine"), tr("The machine's name in Hyper-V:"),
                                                   QLineEdit::Normal, QString(), &ok)
                                 .trimmed();
        if (!ok || name.isEmpty()) return;
        if (!valid_vm_name(u8(name))) {
            QMessageBox::warning(this, windowTitle(), tr("A machine name cannot contain * ? [ ] or `."));
            return;
        }
        add_machine_item(hyperv_list_, name, QString(), true);
    });
    hl->addWidget(row({refresh, by_name}));
    hyperv_staging_ = new QLineEdit;
    hyperv_staging_->setPlaceholderText(qs(default_hyperv_staging()));
    auto* staging_browse = new QPushButton(tr("Browse..."));
    connect(staging_browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Export folder"), hyperv_staging_->text());
        if (!dir.isEmpty()) hyperv_staging_->setText(QDir::toNativeSeparators(dir));
    });
    auto* staging_row = new QWidget;
    auto* sl = new QHBoxLayout(staging_row);
    sl->setContentsMargins(0, 0, 0, 0);
    sl->addWidget(new QLabel(tr("Export folder:")));
    sl->addWidget(hyperv_staging_, 1);
    sl->addWidget(staging_browse);
    hl->addWidget(staging_row);
    auto* hv_note = new QLabel(tr("Each ticked machine is exported with Export-VM (a consistent copy, also while it "
                                  "runs) into the export folder, backed up from there as Hyper-V\\<name> and removed "
                                  "again; the folder needs room for the largest machine. Restore it with \"Import as "
                                  "new...\" in the restore window. Needs \"Run with administrator rights\" (Schedule "
                                  "tab)."));
    hv_note->setWordWrap(true);
    hv_note->setForegroundRole(QPalette::PlaceholderText);
    hl->addWidget(hv_note);
    layout->addWidget(hv_box, 1);
    return page;
}

void JobDialog::list_hyperv() {
    hyperv_listed_ = true;
    HyperVList list;
    const QString error = run_busy(this, tr("Asking Hyper-V for its virtual machines..."),
                                   [&](const std::function<bool()>&) { list = hyperv_machines(); });
    if (!error.isEmpty()) {
        hyperv_note_->setText(error);
        return;
    }
    if (!list.available) {
        QString problem = qs(list.problem);
        if (!problem.isEmpty()) problem[0] = problem[0].toUpper();  // the engine's text starts a sentence here
        hyperv_note_->setText(problem +
                              (list.problem.find("administrator") != std::string::npos
                                   ? tr(". Start Vaultik as administrator to list them, or add one by name.")
                                   : QString(".")));
        return;
    }
    for (const HyperVm& m : list.machines) add_machine_item(hyperv_list_, qs(m.name), qs(m.state), false);
    hyperv_note_->setText(list.machines.empty() ? tr("There are no Hyper-V virtual machines on this computer.")
                                                : tr("%1 on this computer.")
                                                      .arg(count_of(list.machines.size(), tr("virtual machine"),
                                                                    tr("virtual machines"))));
}
