#include "job_dialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTimeEdit>
#include <QVBoxLayout>

#include "common.h"
#include "key_dialog.h"
#include "vaultik/repo/keys.hpp"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/jobs/scheduler.hpp"
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
    resize(640, 520);

    auto* tabs = new QTabWidget;
    tabs->addTab(build_general(), tr("General"));
    tabs->addTab(build_sources(), tr("What to back up"));
    tabs->addTab(build_schedule(), tr("Schedule"));
    tabs->addTab(build_resources(), tr("Resources"));
    tabs->addTab(build_retention(), tr("Old backups"));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &JobDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &JobDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    if (existing != nullptr) {
        job_ = *existing;
        load(*existing);
    } else {
        jobs::Job defaults;
        defaults.schedule.kind = jobs::Schedule::Kind::Daily;
        defaults.resources = priority_preset("low");
        defaults.retention = jobs::default_retention();
        load(defaults);
    }
}

JobDialog::~JobDialog() {
    cloud_.wipe();
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
    return page;
}

QWidget* JobDialog::build_sources() {
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);

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
    layout->addWidget(row({add_folder, add_file, remove}));

    layout->addWidget(new QLabel(tr("Skip names matching (wildcards * and ?; a pattern with a path "
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
    return page;
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

    // Health checks: after a scheduled or manual backup, when one is due.
    check_weekly_ = new QCheckBox(tr("Once a week, read back"));
    check_percent_ = spin(1, 100, QStringLiteral("%"), QString());
    form->addRow(tr("Check:"), row({check_weekly_, check_percent_, new QLabel(tr("of the stored data"))}));
    check_monthly_ = new QCheckBox(tr("Once a month, read back all of it"));
    form->addRow(QString(), check_monthly_);
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
    auto* admin_note = new QLabel(tr("Lets scheduled runs back up open and locked files through a shadow copy. "
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
    sources_->clear();
    for (const auto& s : job.sources) sources_->addItem(qs(s));
    excludes_->clear();
    for (const auto& e : job.excludes) excludes_->addItem(qs(e));
    if (vss_ != nullptr) vss_->setChecked(job.vss);
    if (skip_sql_ != nullptr) {
        skip_sql_->setChecked(job.vss_skip_sql);
        skip_sql_->setEnabled(job.vss);
    }
    if (admin_ != nullptr) admin_->setChecked(job.run_as_admin);
    check_weekly_->setChecked(job.health.weekly);
    check_percent_->setValue(job.health.sample_percent);
    check_percent_->setEnabled(job.health.weekly);
    check_monthly_->setChecked(job.health.monthly);

    schedule_kind_->setCurrentIndex(static_cast<int>(job.schedule.kind));
    time_->setTime(QTime(job.schedule.hour, job.schedule.minute));
    every_hours_->setValue(job.schedule.every_hours);
    minute_->setValue(job.schedule.minute);
    for (int d = 0; d < 7; ++d) days_[d]->setChecked((job.schedule.weekdays >> d) & 1u);
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
    } else {
        j.cloud_key_id.clear();
        j.cloud_region.clear();
    }
    j.sources.clear();
    for (const QString& s : items_of(sources_)) j.sources.push_back(u8(s));
    j.excludes.clear();
    for (const QString& e : items_of(excludes_)) j.excludes.push_back(u8(e));
    if (vss_ != nullptr) j.vss = vss_->isChecked();
    if (skip_sql_ != nullptr) j.vss_skip_sql = skip_sql_->isChecked();
    if (admin_ != nullptr) j.run_as_admin = admin_->isChecked();

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

    ResourcePolicy& r = j.resources;
    r.cpu = static_cast<CpuPriority>(cpu_->currentIndex());
    r.io = static_cast<IoPriority>(io_->currentIndex());
    r.memory = static_cast<MemoryPriority>(ram_->currentIndex());
    r.cpu_limit_percent = static_cast<unsigned>(cpu_limit_->value());
    r.max_memory = static_cast<std::uint64_t>(max_memory_->value()) << 20;
    r.read_rate = static_cast<std::uint64_t>(read_limit_->value()) << 20;
    r.write_rate = static_cast<std::uint64_t>(write_limit_->value()) << 20;

    j.retention = collect_retention();
    j.health.weekly = check_weekly_->isChecked();
    j.health.sample_percent = check_percent_->value();
    j.health.monthly = check_monthly_->isChecked();
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
        QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
        return;
    }
    QApplication::restoreOverrideCursor();
    job_ = j;
    QDialog::accept();
}
