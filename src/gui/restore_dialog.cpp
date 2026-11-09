#include "restore_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common.h"
#include "runner.h"
#include "vaultik/model/tree.hpp"

using namespace vaultik;

namespace {

enum Role { KindRole = Qt::UserRole, SubtreeRole, PathRole, LoadedRole };

// Where a snapshot path came from on this kind of system; empty when it has no sensible
// original location (a whole drive or share).
QString original_of(const QString& snapshot_path) {
    const QStringList parts = snapshot_path.split('/', Qt::SkipEmptyParts);
#ifdef Q_OS_WIN
    if (parts.isEmpty()) return {};
    if (parts[0] == "UNC") return parts.size() < 4 ? QString() : "\\\\" + parts.mid(1).join('\\');
    if (parts[0].size() != 1 || parts.size() < 2) return {};
    return parts[0] + ":\\" + parts.mid(1).join('\\');
#else
    if (parts.isEmpty()) return {};
    return "/" + parts.join('/');
#endif
}

QString snapshot_label(const SnapshotEntry& e) {
    QString label = format_time(e.snapshot.time) + "   " + format_bytes(e.snapshot.stats.bytes) + "   " +
                    qs(id_hex(e.id).substr(0, 8));
    if (e.snapshot.stats.errors > 0) label += "   " + QObject::tr("(%1 skipped)").arg(e.snapshot.stats.errors);
    return label;
}

}  // namespace

RestoreDialog::RestoreDialog(const jobs::Job& job, std::shared_ptr<Repository> repo,
                             std::vector<SnapshotEntry> snapshots, int selected, QWidget* parent, QString password,
                             CloudCredentials cloud)
    : QDialog(parent),
      job_(job),
      repo_(std::move(repo)),
      snapshots_(std::move(snapshots)),
      password_(std::move(password)),
      cloud_(std::move(cloud)) {
    setWindowTitle(tr("Restore from \"%1\"").arg(qs(job.name)));
    resize(760, 600);

    snapshot_ = new QComboBox;
    for (const auto& e : snapshots_) snapshot_->addItem(snapshot_label(e));

    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setStretchLastSection(false);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);

    to_folder_ = new QRadioButton(tr("To another folder:"));
    to_original_ = new QRadioButton(tr("To the original location"));
    to_folder_->setChecked(true);
    target_ = new QLineEdit;
    target_->setText(QDir::toNativeSeparators(QDir::homePath() + "/Restored"));
    browse_ = new QPushButton(tr("Browse..."));
    overwrite_ = new QCheckBox(tr("Replace files that already exist"));

    bar_ = new QProgressBar;
    bar_->setRange(0, 0);
    bar_->hide();
    status_ = new QLabel(tr("Pick a file or folder, or restore everything in the snapshot."));
    status_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    restore_ = buttons->addButton(tr("Restore"), QDialogButtonBox::AcceptRole);
    restore_->setDefault(true);
    connect(restore_, &QPushButton::clicked, this, &RestoreDialog::start_restore);
    connect(buttons, &QDialogButtonBox::rejected, this, &RestoreDialog::reject);

    auto* target_row = new QHBoxLayout;
    target_row->addWidget(to_folder_);
    target_row->addWidget(target_, 1);
    target_row->addWidget(browse_);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QFormLayout;
    top->addRow(tr("Snapshot:"), snapshot_);
    layout->addLayout(top);
    layout->addWidget(tree_, 1);
    layout->addLayout(target_row);
    layout->addWidget(to_original_);
    layout->addWidget(overwrite_);
    layout->addWidget(bar_);
    layout->addWidget(status_);
    layout->addWidget(buttons);

    connect(snapshot_, &QComboBox::currentIndexChanged, this, &RestoreDialog::show_snapshot);
    connect(tree_, &QTreeWidget::itemExpanded, this, &RestoreDialog::expand);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &RestoreDialog::update_target_state);
    connect(to_original_, &QRadioButton::toggled, this, &RestoreDialog::update_target_state);
    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Restore into"), target_->text());
        if (!dir.isEmpty()) target_->setText(QDir::toNativeSeparators(dir));
    });

    runner_ = new Runner(this);
    connect(runner_, &Runner::progress, this, [this](const QJsonObject& ev) {
        status_->setText(tr("%1 files, %2   %3")
                             .arg(ev.value("files").toInteger())
                             .arg(format_bytes(static_cast<std::uint64_t>(ev.value("bytes").toInteger())))
                             .arg(ev.value("current").toString()));
    });
    connect(runner_, &Runner::warning, this, [this](const QString&) { ++warnings_; });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        bar_->hide();
        restore_->setEnabled(true);
        snapshot_->setEnabled(true);
        if (result == "success" || result == "partial") {
            QString text = tr("Restored %1 files (%2) in %3.")
                               .arg(ev.value("files").toInteger())
                               .arg(format_bytes(static_cast<std::uint64_t>(ev.value("bytes").toInteger())))
                               .arg(format_duration(ev.value("seconds").toDouble()));
            const auto skipped = ev.value("skipped").toInteger();
            if (skipped > 0) text += " " + tr("%1 skipped (already there, or links).").arg(skipped);
            if (ev.value("errors").toInteger() > 0)
                text += " " + tr("%1 item(s) failed.").arg(ev.value("errors").toInteger());
            status_->setText(text);
        } else if (result == "cancelled") {
            status_->setText(tr("Restore cancelled. Files already restored were kept."));
        } else {
            status_->setText(tr("Restore failed: %1").arg(ev.value("message").toString()));
        }
    });

    show_snapshot(selected >= 0 && selected < static_cast<int>(snapshots_.size()) ? selected : 0);
    if (selected > 0) snapshot_->setCurrentIndex(selected);
    update_target_state();
}

void RestoreDialog::show_snapshot(int index) {
    tree_->clear();
    if (index < 0 || index >= static_cast<int>(snapshots_.size())) return;
    // The root is a virtual folder holding the drives (Windows) or top-level folders (Linux).
    auto* root = new QTreeWidgetItem;
    root->setData(0, KindRole, static_cast<int>(NodeKind::Dir));
    root->setData(0, SubtreeRole, qs(id_hex(snapshots_[static_cast<std::size_t>(index)].snapshot.tree)));
    root->setData(0, PathRole, QString());
    expand(root);  // fills it
    const QList<QTreeWidgetItem*> top = root->takeChildren();
    delete root;
    tree_->addTopLevelItems(top);
    // Open the single-child chain down to the backed-up folders.
    QTreeWidgetItem* cur = top.size() == 1 ? top.front() : nullptr;
    for (int depth = 0; cur != nullptr && depth < 8; ++depth) {
        cur->setExpanded(true);
        cur = cur->childCount() == 1 ? cur->child(0) : nullptr;
    }
}

void RestoreDialog::expand(QTreeWidgetItem* item) {
    if (item->data(0, LoadedRole).toBool()) return;
    item->setData(0, LoadedRole, true);
    qDeleteAll(item->takeChildren());  // the placeholder
    Id id{};
    try {
        id = id_from_hex(u8(item->data(0, SubtreeRole).toString()));
        const Tree tree = parse_tree(repo_->load_blob(BlobType::Tree, id));
        const QString base = item->data(0, PathRole).toString();
        QStyle* st = style();
        for (const Node& n : tree.nodes) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, qs(n.name));
            child->setData(0, KindRole, static_cast<int>(n.kind));
            child->setData(0, PathRole, base.isEmpty() ? qs(n.name) : base + "/" + qs(n.name));
            child->setText(2, format_time(n.mtime));
            switch (n.kind) {
                case NodeKind::Dir:
                    child->setIcon(0, st->standardIcon(QStyle::SP_DirIcon));
                    child->setData(0, SubtreeRole, qs(id_hex(n.subtree)));
                    new QTreeWidgetItem(child);  // placeholder so it can be expanded
                    break;
                case NodeKind::File:
                    child->setIcon(0, st->standardIcon(QStyle::SP_FileIcon));
                    child->setText(1, format_bytes(n.size));
                    break;
                case NodeKind::Symlink:
                    child->setIcon(0, st->standardIcon(QStyle::SP_FileLinkIcon));
                    child->setText(1, "-> " + qs(n.link_target));
                    break;
                case NodeKind::Reparse:
                    child->setIcon(0, st->standardIcon(QStyle::SP_FileLinkIcon));
                    child->setText(1, tr("link (no content)"));
                    break;
            }
        }
    } catch (const std::exception& e) {
        auto* err = new QTreeWidgetItem(item);
        err->setText(0, tr("cannot read this folder: %1").arg(QString::fromUtf8(e.what())));
    }
}

QString RestoreDialog::selected_path() const {
    const QList<QTreeWidgetItem*> sel = tree_->selectedItems();
    return sel.isEmpty() ? QString() : sel.front()->data(0, PathRole).toString();
}

void RestoreDialog::update_target_state() {
    const QString path = selected_path();
    const QString original = original_of(path);
    to_original_->setEnabled(!original.isEmpty());
    if (original.isEmpty() && to_original_->isChecked()) to_folder_->setChecked(true);
    to_original_->setText(original.isEmpty() ? tr("To the original location (pick a file or folder first)")
                                             : tr("To the original location: %1").arg(original));
    target_->setEnabled(to_folder_->isChecked());
    browse_->setEnabled(to_folder_->isChecked());
    restore_->setText(path.isEmpty() ? tr("Restore everything") : tr("Restore selected"));
}

void RestoreDialog::start_restore() {
    if (runner_->running() || snapshots_.empty()) return;
    const SnapshotEntry& snap = snapshots_[static_cast<std::size_t>(snapshot_->currentIndex())];
    const QString path = selected_path();

    QString target;
    if (to_original_->isChecked()) {
        const QString original = original_of(path);
        target = QDir::toNativeSeparators(QFileInfo(original).path());
        const auto answer = QMessageBox::question(
            this, windowTitle(),
            overwrite_->isChecked()
                ? tr("Restore %1 to its original location and REPLACE the files that are there now?").arg(original)
                : tr("Restore %1 to its original location? Files that exist already are left alone.").arg(original));
        if (answer != QMessageBox::Yes) return;
    } else {
        target = target_->text().trimmed();
        if (target.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("Choose a folder to restore into."));
            return;
        }
    }

    QStringList args{"restore"};
    if (job_.id.empty())
        args << "--repo" << qs(job_.repository);
    else
        args << "--job" << qs(job_.id);
    args << qs(id_hex(snap.id)) << "--target" << target;
    if (!path.isEmpty()) args << "--path" << path;
    if (overwrite_->isChecked()) args << "--overwrite";
    warnings_ = 0;
    bar_->show();
    restore_->setEnabled(false);
    snapshot_->setEnabled(false);
    status_->setText(tr("Starting..."));
    runner_->start(args, password_, cloud_);
}

RestoreDialog::~RestoreDialog() {
    password_.fill(QChar(0));
}

void RestoreDialog::reject() {
    if (runner_->running()) {
        const auto answer =
            QMessageBox::question(this, windowTitle(), tr("A restore is running. Stop it and close?"));
        if (answer != QMessageBox::Yes) return;
        runner_->cancel();
    }
    QDialog::reject();
}
