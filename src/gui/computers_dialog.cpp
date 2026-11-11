#include "computers_dialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLocale>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "common.h"
#include "repo_handle.h"
#include "theme.h"
#include "vaultik/crypto/crypto.hpp"
#include "vaultik/platform/os.hpp"
#include "vaultik/repo/cloud.hpp"

using namespace vaultik;

namespace {

constexpr int kSourceRole = Qt::UserRole;
constexpr int kComputerRole = Qt::UserRole + 1;

QString health_text(jobs::Health h) {
    switch (h) {
        case jobs::Health::Ok: return QObject::tr("OK");
        case jobs::Health::Unknown: return QObject::tr("No status yet");
        case jobs::Health::Warning: return QObject::tr("Warning");
        case jobs::Health::Stale: return QObject::tr("Overdue");
        case jobs::Health::Failed: return QObject::tr("Failed");
    }
    return {};
}

QIcon health_icon(jobs::Health h) {
    const Theme& t = theme();
    switch (h) {
        case jobs::Health::Ok: return icon(QStringLiteral("checkmark_circle_20_filled"), t.success);
        case jobs::Health::Unknown: return icon(QStringLiteral("subtract_circle_20_filled"), t.neutral);
        case jobs::Health::Warning: return icon(QStringLiteral("warning_20_filled"), t.warning);
        case jobs::Health::Stale: return icon(QStringLiteral("clock_20_regular"), t.warning);
        case jobs::Health::Failed: return icon(QStringLiteral("dismiss_circle_20_filled"), t.danger);
    }
    return {};
}

QString kind_text(const std::string& kind) {
    if (kind == "backup") return QObject::tr("Backup");
    if (kind == "check") return QObject::tr("Health check");
    if (kind == "full_check") return QObject::tr("Full health check");
    if (kind == "copy") return QObject::tr("Second copy");
    if (kind == "restore_test") return QObject::tr("Test restore");
    return qs(kind);
}

QString result_text(const std::string& result) {
    if (result == "success") return QObject::tr("completed");
    if (result == "partial") return QObject::tr("completed, some files skipped");
    if (result == "failed") return QObject::tr("failed");
    if (result == "cancelled") return QObject::tr("cancelled");
    return qs(result);
}

// The newest saved version: from the status record when there is one, else from the snapshots.
std::uint64_t last_backup_of(const jobs::Computer& c) {
    std::uint64_t t = c.latest;
    if (c.status)
        for (const jobs::JobStatus& j : c.status->jobs) t = std::max(t, j.last_success);
    return t;
}

QString when(std::uint64_t t) {
    return t == 0 ? QObject::tr("never") : format_time(t);
}

}  // namespace

ComputersDialog::ComputersDialog(std::vector<ComputerSource> sources, QWidget* parent)
    : QDialog(parent), sources_(std::move(sources)) {
    setWindowTitle(tr("All computers"));
    resize(860, 600);

    auto* intro = new QLabel(tr("Every computer that backs up into the same place as your jobs (a shared folder, a "
                                "network drive or cloud storage). After each run, Vaultik leaves a small encrypted "
                                "status note there, so each computer can see how the others are doing."));
    intro->setWordWrap(true);
    intro->setProperty("role", "muted");

    tree_ = new QTreeWidget;
    tree_->setColumnCount(5);
    tree_->setHeaderLabels({tr("Computer"), tr("State"), tr("Last backup"), tr("Versions"), tr("Size")});
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->header()->setStretchLastSection(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int c = 1; c < 5; ++c) tree_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &ComputersDialog::show_details);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &ComputersDialog::update_buttons);

    details_ = new QPlainTextEdit;
    details_->setReadOnly(true);
    details_->setPlaceholderText(tr("Choose a computer to see the details of its jobs."));
    details_->setMaximumHeight(200);

    summary_ = new QLabel;
    summary_->setProperty("role", "muted");

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    refresh_ = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    refresh_->setIcon(icon(QStringLiteral("arrow_sync_20_regular")));
    connect(refresh_, &QPushButton::clicked, this, &ComputersDialog::load);
    ask_ = buttons->addButton(tr("Ask to back up now"), QDialogButtonBox::ActionRole);
    ask_->setToolTip(tr("That computer backs up when one of its jobs here runs, or when Vaultik there next looks "
                        "(every 15 minutes, every hour for cloud storage)."));
    connect(ask_, &QPushButton::clicked, this, &ComputersDialog::ask_backup);
    forget_ = buttons->addButton(tr("Remove..."), QDialogButtonBox::ActionRole);
    forget_->setToolTip(tr("Remove the status note of a computer that no longer backs up here"));
    connect(forget_, &QPushButton::clicked, this, &ComputersDialog::forget);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(tree_, 1);
    layout->addWidget(details_);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(summary_, 1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    load();
    update_buttons();
}

const jobs::Computer* ComputersDialog::current(int* source) const {
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || !item->data(0, kComputerRole).isValid()) return nullptr;
    const int s = item->data(0, kSourceRole).toInt();
    const int c = item->data(0, kComputerRole).toInt();
    if (s < 0 || static_cast<std::size_t>(s) >= computers_.size()) return nullptr;
    const auto& list = computers_[static_cast<std::size_t>(s)];
    if (c < 0 || static_cast<std::size_t>(c) >= list.size()) return nullptr;
    if (source != nullptr) *source = s;
    return &list[static_cast<std::size_t>(c)];
}

void ComputersDialog::update_buttons() {
    const jobs::Computer* pc = current(nullptr);
    // Only another computer with a status note: this one starts its backups itself, and one without
    // a note has nothing to remove and no jobs to ask for.
    const bool other = pc != nullptr && !pc->this_computer && !pc->status_name.empty();
    ask_->setEnabled(other && pc->status && !pc->status->jobs.empty());
    forget_->setEnabled(other);
}

void ComputersDialog::ask_backup() {
    int s = 0;
    const jobs::Computer* pc = current(&s);
    if (pc == nullptr || !pc->status) return;
    const ComputerSource& src = sources_[static_cast<std::size_t>(s)];
    jobs::StatusFile file;
    file.name = pc->status_name;
    file.status = pc->status;
    ask_->setEnabled(false);
    const std::string from = os::hostname();
    RepoHandle::action_async(
        this, src.repository, src.password, src.cloud,
        [file, from](Repository& repo) { jobs::ask_backup(repo, file, {}, from, os::now()); },
        [this, host = qs(pc->host)](const QString& error) {
            if (!error.isEmpty()) {
                plain_warning(this, tr("Ask to back up now"), error);
                update_buttons();
                return;
            }
            summary_->setText(tr("Asked %1 to back up now.").arg(host));
            load();
        });
}

void ComputersDialog::forget() {
    int s = 0;
    const jobs::Computer* pc = current(&s);
    if (pc == nullptr || pc->this_computer || pc->status_name.empty()) return;
    const QString host = pc->host == "?" ? tr("this unknown computer") : qs(pc->host);
    if (QMessageBox::question(this, tr("Remove computer"),
                              tr("Remove the status note of %1 from this backup? Do this for a computer that no "
                                 "longer backs up here. Its versions stay, and if it still backs up here, its next "
                                 "run leaves a new note.").arg(host)) != QMessageBox::Yes)
        return;
    const ComputerSource& src = sources_[static_cast<std::size_t>(s)];
    const std::string name = pc->status_name;
    forget_->setEnabled(false);
    RepoHandle::action_async(
        this, src.repository, src.password, src.cloud,
        [name](Repository& repo) { jobs::forget_computer(repo, name); },
        [this](const QString& error) {
            if (!error.isEmpty()) {
                plain_warning(this, tr("Remove computer"), error);
                update_buttons();
                return;
            }
            load();
        });
}

ComputersDialog::~ComputersDialog() {
    for (ComputerSource& s : sources_) secure_wipe(s.password.data(), s.password.size());
}

void ComputersDialog::load() {
    ++generation_;
    tree_->clear();
    details_->clear();
    computers_.assign(sources_.size(), {});
    pending_ = 0;
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        const ComputerSource& s = sources_[i];
        auto* top = new QTreeWidgetItem(tree_, {s.title});
        top->setData(0, kSourceRole, static_cast<int>(i));
        top->setFirstColumnSpanned(true);
        top->setIcon(0, icon(cloud_kind(s.repository) == CloudKind::None ? QStringLiteral("hard_drive_20_regular")
                                                                           : QStringLiteral("cloud_20_regular")));
        top->setExpanded(true);
        if (!s.error.isEmpty()) {
            auto* row = new QTreeWidgetItem(top, {s.error});
            row->setFirstColumnSpanned(true);
            continue;
        }
        auto* row = new QTreeWidgetItem(top, {tr("Reading the backup...")});
        row->setFirstColumnSpanned(true);
        ++pending_;
        RepoHandle::computers_async(this, s.repository, s.password, s.cloud,
                                    [this, i, generation = generation_](std::vector<jobs::Computer> list, QString error) {
                                        if (generation != generation_) return;
                                        --pending_;
                                        fill(static_cast<int>(i), list, error);
                                    });
    }
    refresh_->setEnabled(pending_ == 0);
    summary_->setText(pending_ > 0 ? tr("Reading %1 backup(s)...").arg(pending_) : QString());
    if (sources_.empty()) summary_->setText(tr("None of your jobs has a backup destination yet."));
}

void ComputersDialog::fill(int index, const std::vector<jobs::Computer>& computers, const QString& error) {
    QTreeWidgetItem* top = nullptr;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (tree_->topLevelItem(i)->data(0, kSourceRole).toInt() == index) top = tree_->topLevelItem(i);
    if (top == nullptr) return;
    qDeleteAll(top->takeChildren());
    computers_[static_cast<std::size_t>(index)] = computers;
    if (!error.isEmpty()) {
        auto* row = new QTreeWidgetItem(top, {error});
        row->setFirstColumnSpanned(true);
        row->setIcon(0, health_icon(jobs::Health::Failed));
    } else if (computers.empty()) {
        auto* row = new QTreeWidgetItem(top, {tr("No computer has backed up here yet.")});
        row->setFirstColumnSpanned(true);
    }
    const QLocale locale;
    for (std::size_t c = 0; c < computers.size(); ++c) {
        const jobs::Computer& pc = computers[c];
        QString name = pc.host == "?" ? tr("Unknown computer") : qs(pc.host);
        if (pc.this_computer) name = tr("%1 (this computer)").arg(name);
        auto* row = new QTreeWidgetItem(top);
        row->setText(0, name);
        row->setIcon(0, icon(QStringLiteral("window_20_regular")));
        row->setText(1, pc.request ? tr("%1, asked to back up").arg(health_text(pc.health)) : health_text(pc.health));
        row->setIcon(1, health_icon(pc.health));
        row->setText(2, when(last_backup_of(pc)));
        row->setText(3, locale.toString(static_cast<qulonglong>(pc.versions)));
        row->setText(4, pc.versions > 0 ? format_bytes(pc.latest_bytes) : QString());
        row->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        row->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        row->setData(0, kSourceRole, index);
        row->setData(0, kComputerRole, static_cast<int>(c));
    }
    top->setExpanded(true);
    refresh_->setEnabled(pending_ == 0);
    if (pending_ == 0) {
        int total = 0, attention = 0;
        for (const auto& list : computers_) {
            for (const jobs::Computer& pc : list) {
                ++total;
                if (pc.health == jobs::Health::Warning || pc.health == jobs::Health::Stale ||
                    pc.health == jobs::Health::Failed)
                    ++attention;
            }
        }
        summary_->setText(attention == 0 ? tr("%1 computer(s), none needs attention.").arg(total)
                                         : tr("%1 of %2 computer(s) need attention.").arg(attention).arg(total));
    }
}

void ComputersDialog::show_details() {
    details_->clear();
    const QTreeWidgetItem* item = tree_->currentItem();
    if (item == nullptr || !item->data(0, kComputerRole).isValid()) return;
    const int s = item->data(0, kSourceRole).toInt();
    const int c = item->data(0, kComputerRole).toInt();
    if (s < 0 || static_cast<std::size_t>(s) >= computers_.size()) return;
    const auto& list = computers_[static_cast<std::size_t>(s)];
    if (c < 0 || static_cast<std::size_t>(c) >= list.size()) return;
    const jobs::Computer& pc = list[static_cast<std::size_t>(c)];
    const std::uint64_t now = os::now();

    QStringList lines;
    lines << tr("%1: %2").arg(item->text(0), health_text(pc.health));
    if (pc.request) {
        lines << (pc.request->state == "running"
                      ? tr("Asked to back up by %1 on %2; it started on %3.")
                            .arg(qs(pc.request->from), format_time(pc.request->created), format_time(pc.request->started))
                      : tr("Asked to back up by %1 on %2; waiting for it to look (it looks when one of its jobs here "
                           "runs, and every 15 minutes while Vaultik runs there, every hour for cloud storage).")
                            .arg(qs(pc.request->from), format_time(pc.request->created)));
    }
    if (pc.versions > 0) {
        lines << tr("Versions: %1, the newest from %2 (%3); together they added %4 to the backup.")
                     .arg(pc.versions)
                     .arg(format_time(pc.latest), format_bytes(pc.latest_bytes), format_bytes(pc.added_bytes));
    } else {
        lines << tr("No versions from this computer.");
    }
    if (!pc.status) {
        lines << (pc.status_name.empty()
                      ? tr("This computer left no status note yet: it runs an older Vaultik, or it backed up here "
                           "without a saved job.")
                      : tr("Its status note cannot be read (damaged, or not written with this backup's key)."));
        details_->setPlainText(lines.join(QLatin1Char('\n')));
        return;
    }
    lines << tr("Vaultik %1, last note %2").arg(qs(pc.status->version), format_time(pc.status->updated));
    if (pc.record_behind)
        lines << tr("Its status note is older than its newest version: it could not be updated, or the storage "
                    "put an older note back. What it says may be out of date.");
    for (const jobs::JobStatus& j : pc.status->jobs) {
        lines << QString();
        lines << tr("%1: %2").arg(qs(j.name), health_text(jobs::job_health(j, now)));
        if (j.last_run != 0)
            lines << tr("  Last run: %1 %2, %3").arg(kind_text(j.last_kind), result_text(j.last_result), format_time(j.last_run));
        if (!j.last_message.empty()) lines << QStringLiteral("  ") + qs(j.last_message);
        lines << tr("  Last saved version: %1").arg(when(j.last_success));
        lines << (j.next_run != 0 ? tr("  Next run: %1").arg(format_time(j.next_run))
                                  : tr("  Runs when started by hand"));
        for (const std::string& w : j.warnings) lines << QStringLiteral("  ! ") + qs(w);
    }
    details_->setPlainText(lines.join(QLatin1Char('\n')));
}
