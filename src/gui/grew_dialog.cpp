#include "grew_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "common.h"
#include "runner.h"
#include "vaultik/jobs/scheduler.hpp"
#include "vaultik/platform/os.hpp"

using namespace vaultik;

namespace {

constexpr std::uint64_t kDay = 864'000'000'000ull;  // FILETIME ticks

}  // namespace

GrewDialog::GrewDialog(const jobs::JobStore& store, jobs::Job job, std::shared_ptr<Repository> repo,
                       std::vector<SnapshotEntry> snapshots, QWidget* parent)
    : QDialog(parent), store_(store), job_(std::move(job)), repo_(std::move(repo)) {
    setWindowTitle(tr("What grew: %1").arg(qs(job_.name)));
    for (SnapshotEntry& s : snapshots) snapshots_.emplace_back(s.id, std::move(s.snapshot));

    auto* intro = new QLabel(tr("The folders that added the most new data to this backup: what each version stored "
                                "that the versions before it did not have yet, as stored (compressed). Where a "
                                "folder is one that is rarely worth keeping, Vaultik offers to leave it out from now "
                                "on."));
    intro->setWordWrap(true);

    period_ = new QComboBox;
    period_->addItem(tr("Last 7 days"), 7);
    period_->addItem(tr("Last 30 days"), 30);
    period_->addItem(tr("Last 90 days"), 90);
    period_->addItem(tr("Last year"), 365);
    period_->setCurrentIndex(1);
    auto* form = new QFormLayout;
    form->addRow(tr("Period:"), period_);

    summary_ = new QLabel;
    summary_->setWordWrap(true);
    summary_->setTextFormat(Qt::PlainText);

    folders_ = new QTreeWidget;
    folders_->setRootIsDecorated(false);
    folders_->setHeaderLabels({tr("Folder"), tr("New data"), tr("Files"), QString()});
    folders_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    folders_->header()->setStretchLastSection(false);

    presets_title_ = new QLabel(tr("Ready-made exclusions this job does not use, and what they would have left out:"));
    presets_title_->setWordWrap(true);
    presets_ = new QTreeWidget;
    presets_->setRootIsDecorated(false);
    presets_->setHeaderLabels({tr("Exclusion"), tr("New data"), tr("Files"), QString()});
    presets_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    presets_->header()->setStretchLastSection(false);
    presets_->setMaximumHeight(140);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(form);
    layout->addWidget(summary_);
    layout->addWidget(folders_, 1);
    layout->addWidget(presets_title_);
    layout->addWidget(presets_);
    layout->addWidget(buttons);
    connect(period_, &QComboBox::currentIndexChanged, this, &GrewDialog::compute);
    fit_to_screen(this, QSize(860, 600), parent);
    QMetaObject::invokeMethod(this, &GrewDialog::compute, Qt::QueuedConnection);
}

void GrewDialog::compute() {
    const int days = period_->currentData().toInt();
    jobs::GrowthOptions o;
    const std::uint64_t now = os::now();
    const std::uint64_t back = static_cast<std::uint64_t>(days) * kDay;
    o.since = now > back ? now - back : 0;
    o.excludes = job_.excludes;
    // The job's versions: those tagged with it, else (older versions) those of this computer.
    const std::string tag = "job:" + job_.id;
    const auto tagged = [tag](const Snapshot& s) { return std::find(s.tags.begin(), s.tags.end(), tag) != s.tags.end(); };
    if (std::any_of(snapshots_.begin(), snapshots_.end(), [&](const auto& s) { return tagged(s.second); }))
        o.select = tagged;
    else
        o.select = [host = os::hostname()](const Snapshot& s) { return s.hostname == host; };
    jobs::GrowthReport report;
    const std::shared_ptr<Repository> repo = repo_;
    const QString error = run_busy(this, tr("Reading what each version added..."), [&](const std::function<bool()>& cancelled) {
        o.should_cancel = cancelled;
        report = jobs::what_grew(*repo, snapshots_, o);
    });
    if (!error.isEmpty()) {
        if (error != tr("Cancelled.")) plain_warning(this, windowTitle(), error);
        summary_->setText(tr("Not worked out."));
        folders_->clear();
        presets_->clear();
        return;
    }
    report_ = std::move(report);
    show_report();
}

void GrewDialog::show_report() {
    folders_->clear();
    presets_->clear();
    const jobs::GrowthReport& r = report_;
    QString summary = tr("Since %1: %2 new in %3, from %4.")
                          .arg(format_time(r.since), format_bytes(r.stored), count_of(r.files, tr("file"), tr("files")),
                               count_of(r.versions, tr("version"), tr("versions")));
    if (r.folders.empty()) summary += "\n" + tr("Nothing new was stored in that time.");
    summary_->setText(summary);

    const auto add_button = [this](QTreeWidget* tree, QTreeWidgetItem* item, const QString& label, const QString& what,
                                   std::vector<std::string> patterns) {
        auto* button = new QPushButton(label);
        QStringList tip;
        for (const std::string& p : patterns) tip << qs(p);
        button->setToolTip(tr("Adds to the job's exclusions:") + "\n" + tip.join('\n'));
        connect(button, &QPushButton::clicked, this, [this, tree, item, what, patterns] {
            if (!add_exclusion(what, patterns)) return;
            tree->setItemWidget(item, 3, nullptr);
            item->setText(3, tr("Left out from now on"));
        });
        tree->setItemWidget(item, 3, button);
    };

    for (const jobs::FolderGrowth& f : r.folders) {
        auto* item = new QTreeWidgetItem(folders_);
        const QString path = QDir::toNativeSeparators(qs(f.path));
        item->setText(0, f.rest ? tr("%1 (other files)").arg(path) : path);
        item->setToolTip(0, path);
        item->setText(1, format_bytes(f.stored));
        item->setText(2, QLocale().toString(static_cast<qulonglong>(f.files)));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        if (!f.suggestion) continue;
        if (f.suggestion->preset.empty())
            add_button(folders_, item, tr("Skip this folder"), path, f.suggestion->patterns);
        else
            add_button(folders_, item, tr("Skip: %1").arg(exclusion_preset_label(f.suggestion->preset)),
                       exclusion_preset_label(f.suggestion->preset), f.suggestion->patterns);
    }
    for (const jobs::PresetGrowth& p : r.presets) {
        auto* item = new QTreeWidgetItem(presets_);
        item->setText(0, exclusion_preset_label(p.id));
        item->setText(1, format_bytes(p.stored));
        item->setText(2, QLocale().toString(static_cast<qulonglong>(p.files)));
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        add_button(presets_, item, tr("Skip from now on"), exclusion_preset_label(p.id), p.patterns);
    }
    presets_title_->setVisible(!r.presets.empty());
    presets_->setVisible(!r.presets.empty());
    for (QTreeWidget* tree : {folders_, presets_})
        for (int c = 1; c < 4; ++c) tree->resizeColumnToContents(c);
}

bool GrewDialog::add_exclusion(const QString& what, const std::vector<std::string>& patterns) {
    QStringList list;
    for (const std::string& p : patterns) list << qs(p);
    if (!plain_question(this, windowTitle(),
                        tr("Leave %1 out of the backups of %2 from now on? Versions saved already keep it until "
                           "they are cleaned up.\n\nAdds to the job's exclusions:\n%3")
                            .arg(what, qs(job_.name), list.join('\n'))))
        return false;
    jobs::Job j = job_;
    for (const std::string& p : patterns)
        if (std::find(j.excludes.begin(), j.excludes.end(), p) == j.excludes.end()) j.excludes.push_back(p);
    try {
        store_.save(j);
    } catch (const std::exception& e) {
        plain_warning(this, windowTitle(), QString::fromUtf8(e.what()));
        return false;
    }
    job_ = j;
    changed_ = true;
    // A job with administrator rights keeps its own approved copy, which needs approval to update.
    try {
        jobs::apply_schedule(j, u8(QDir::toNativeSeparators(QFileInfo(Runner::engine_path()).absolutePath())));
    } catch (const std::exception& e) {
        plain_warning(this, windowTitle(),
                      tr("The job was saved, but its schedule could not be set up:\n%1").arg(QString::fromUtf8(e.what())));
    }
    return true;
}
