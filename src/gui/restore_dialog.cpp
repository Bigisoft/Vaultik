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
#include <QListWidget>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSplitter>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common.h"
#include "find_page.h"
#include "preview.h"
#include "runner.h"
#include "theme.h"
#include "vaultik/model/tree.hpp"
#include "vaultik/ops/move_here.hpp"

using namespace vaultik;

namespace {

enum Role { KindRole = Qt::UserRole, SubtreeRole, PathRole, LoadedRole, ParentTreeRole };

constexpr std::uint64_t kAutoPreviewLimit = 20u << 20;  // larger files (and cloud backups) wait for a click

QString original_of(const QString& snapshot_path) {
    return original_path(snapshot_path);
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
                             CloudCredentials cloud, const QString& select_path)
    : QDialog(parent),
      job_(job),
      repo_(std::move(repo)),
      snapshots_(std::move(snapshots)),
      password_(std::move(password)),
      cloud_(std::move(cloud)) {
    setWindowTitle(tr("Restore from \"%1\"").arg(qs(job.name)));
    resize(1240, 720);

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
    permissions_ = new QCheckBox(tr("Restore permissions (who may open or change each item)"));
    permissions_->setToolTip(tr("Gives each file and folder the owner and access list it had when it was backed "
                                "up. On by itself for the original location; restored elsewhere, another "
                                "account's permissions could keep you out of your own restored files."));

    bar_ = new QProgressBar;
    bar_->setRange(0, 0);
    bar_->hide();
    status_ = new QLabel(tr("Pick a file or folder, or restore everything in this version."));
    status_->setTextFormat(Qt::PlainText);  // it shows names from the backup
    status_->setWordWrap(true);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    move_ = buttons->addButton(tr("Move to this PC..."), QDialogButtonBox::ActionRole);
    move_->setToolTip(tr("Set up this computer from an old one's backup: its Documents, Desktop, pictures, browser "
                         "profiles, email and game saves, each into the same folder of your account here"));
    connect(move_, &QPushButton::clicked, this, &RestoreDialog::move_here);
    thaw_ = buttons->addButton(tr("Request thaw..."), QDialogButtonBox::ActionRole);
    thaw_->setToolTip(tr("This backup keeps its data in archive storage: ask the cloud storage for a readable copy "
                         "of what restoring the selected item (or the whole version) needs. Asking again shows how "
                         "far the thaw is."));
    connect(thaw_, &QPushButton::clicked, this, &RestoreDialog::request_thaw);
    restore_ = buttons->addButton(tr("Restore"), QDialogButtonBox::AcceptRole);
    restore_->setDefault(true);
    connect(restore_, &QPushButton::clicked, this, &RestoreDialog::start_restore);
    connect(buttons, &QDialogButtonBox::rejected, this, &RestoreDialog::reject);

    auto* target_row = new QHBoxLayout;
    target_row->addWidget(to_folder_);
    target_row->addWidget(target_, 1);
    target_row->addWidget(browse_);

    preview_ = new PreviewWidget;
    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(tree_);
    split->addWidget(preview_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    split->setSizes({560, 640});

    auto* layout = new QVBoxLayout(this);
    auto* top = new QFormLayout;
    top->addRow(tr("Version:"), snapshot_);
    layout->addLayout(top);
    layout->addWidget(split, 1);
    layout->addLayout(target_row);
    layout->addWidget(to_original_);
    layout->addWidget(overwrite_);
    layout->addWidget(permissions_);
    layout->addWidget(bar_);
    layout->addWidget(status_);
    layout->addWidget(buttons);

    connect(snapshot_, &QComboBox::currentIndexChanged, this, &RestoreDialog::show_snapshot);
    connect(tree_, &QTreeWidget::itemExpanded, this, &RestoreDialog::expand);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &RestoreDialog::update_target_state);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &RestoreDialog::preview_selected);
    connect(to_original_, &QRadioButton::toggled, this, &RestoreDialog::update_target_state);
    connect(to_original_, &QRadioButton::toggled, permissions_, &QCheckBox::setChecked);
    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Restore into"), target_->text());
        if (!dir.isEmpty()) target_->setText(QDir::toNativeSeparators(dir));
    });

    runner_ = new Runner(this);
    connect(runner_, &Runner::progress, this, [this](const QJsonObject& ev) {
        if (thaw_running_) {
            status_->setText(tr("Asking for a thaw: part %1...").arg(ev.value("files").toInteger()));
            return;
        }
        status_->setText(tr("%1 files, %2   %3")
                             .arg(ev.value("files").toInteger())
                             .arg(format_bytes(static_cast<std::uint64_t>(ev.value("bytes").toInteger())))
                             .arg(ev.value("current").toString()));
    });
    connect(runner_, &Runner::warning, this, [this](const QString&) { ++warnings_; });
    connect(runner_, &Runner::finished, this, [this](const QString& result, const QJsonObject& ev) {
        bar_->hide();
        restore_->setEnabled(true);
        move_->setEnabled(true);
        thaw_->setEnabled(true);
        snapshot_->setEnabled(true);
        const bool thaw = thaw_running_;
        thaw_running_ = false;
        if (thaw && result == "success") {
            QString text = tr("Thaw: %1 part(s) asked for now, %2 still being thawed, %3 readable already.")
                               .arg(ev.value("requested").toInteger())
                               .arg(ev.value("in_progress").toInteger())
                               .arg(ev.value("available").toInteger());
            text += " " + (ev.value("ready").toBool()
                               ? tr("Everything this restore needs can be read now: restore it while the thawed copy lasts.")
                               : tr("A thaw takes hours (up to 48 for Deep Archive). Request the thaw again later to "
                                    "see how far it is, then restore."));
            status_->setText(text);
        } else if (thaw) {
            status_->setText(result == "cancelled" ? tr("Thaw request cancelled.")
                                                   : tr("The thaw request failed: %1").arg(ev.value("message").toString()));
        } else if (ev.value("archived").toBool()) {
            // The data is in archive storage: offer the thaw instead of a bare failure.
            thaw_->show();
            status_->setText(tr("This data is in archive storage and must be thawed before it can be restored."));
            if (plain_question(this, windowTitle(),
                               tr("The data of this version is in archive storage (Glacier or Deep Archive): it must "
                                  "be thawed before it can be restored, which takes hours and is billed by the cloud "
                                  "storage. Request a thaw of what this restore needs now?")))
                request_thaw();
        } else if (result == "success" || result == "partial") {
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

    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QTreeWidgetItem* item = tree_->itemAt(pos);
        if (item == nullptr || item->data(0, PathRole).toString().isEmpty()) return;
        tree_->setCurrentItem(item);
        QMenu menu(this);
        menu.addAction(icon(QStringLiteral("history_20_regular")), tr("Show all versions of this item..."), this,
                       &RestoreDialog::show_versions_of_selected);
        menu.exec(tree_->viewport()->mapToGlobal(pos));
    });
    set_role(restore_, "primary");
    restore_->setIcon(icon(QStringLiteral("arrow_counterclockwise_20_regular"), theme().accent_text));
    browse_->setIcon(icon(QStringLiteral("folder_open_20_regular")));

    show_archived_note();

    show_snapshot(selected >= 0 && selected < static_cast<int>(snapshots_.size()) ? selected : 0);
    if (selected > 0) {
        const QSignalBlocker block(snapshot_);
        snapshot_->setCurrentIndex(selected);
    }
    if (!select_path.isEmpty()) this->select_path(select_path);
    update_target_state();
}

void RestoreDialog::preview_selected() {
    const QList<QTreeWidgetItem*> sel = tree_->selectedItems();
    if (sel.isEmpty()) {
        preview_->clear();
        return;
    }
    QTreeWidgetItem* item = sel.front();
    const auto kind = static_cast<NodeKind>(item->data(0, KindRole).toInt());
    if (kind != NodeKind::File) {
        preview_->clear();
        preview_->show_message(item->text(0), kind == NodeKind::Dir ? tr("A folder. Pick a file inside it to preview it.")
                                                                     : tr("A link: it is stored without content."));
        return;
    }
    // The file's entry, from the listing of the folder that holds it.
    try {
        const Id parent = id_from_hex(u8(item->data(0, ParentTreeRole).toString()));
        const Tree tree = parse_tree(repo_->load_blob(BlobType::Tree, parent));
        const Node* node = tree.find(u8(item->text(0)));
        if (node == nullptr) throw Error("not found in its folder");
        const bool now = !is_cloud_location(job_.repository) && node->size <= kAutoPreviewLimit;
        preview_->show_file(repo_, *node, item->text(0), now);
    } catch (const std::exception& e) {
        preview_->show_message(item->text(0), tr("Cannot read this file: %1").arg(QString::fromUtf8(e.what())));
    }
}

void RestoreDialog::select_path(const QString& path) {
    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    QTreeWidgetItem* current = nullptr;
    for (int depth = 0; depth < parts.size(); ++depth) {
        QTreeWidgetItem* next = nullptr;
        const int count = current ? current->childCount() : tree_->topLevelItemCount();
        for (int i = 0; i < count && next == nullptr; ++i) {
            QTreeWidgetItem* child = current ? current->child(i) : tree_->topLevelItem(i);
            if (child->text(0) == parts[depth]) next = child;
        }
        if (next == nullptr) return;
        if (depth + 1 < parts.size()) {
            expand(next);  // loads its children
            next->setExpanded(true);
        }
        current = next;
    }
    if (current != nullptr) {
        tree_->setCurrentItem(current);
        tree_->scrollToItem(current, QAbstractItemView::PositionAtCenter);
    }
}

void RestoreDialog::show_versions_of_selected() {
    const QString path = selected_path();
    if (path.isEmpty()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Versions"));
    dialog.resize(720, 460);
    auto* view = new ItemHistoryView(&dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(view, 1);
    layout->addWidget(buttons);
    view->set_source(repo_, snapshots_);
    int chosen = -1;
    connect(view, &ItemHistoryView::restore_requested, &dialog, [&](int index, const QString&) {
        chosen = index;
        dialog.accept();
    });
    view->show_item(path);
    if (dialog.exec() == QDialog::Accepted && chosen >= 0) {
        snapshot_->setCurrentIndex(chosen);  // shows that version
        select_path(path);
    }
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
        for (const Node& n : tree.nodes) {
            auto* child = new QTreeWidgetItem(item);
            child->setText(0, qs(n.name));
            child->setData(0, KindRole, static_cast<int>(n.kind));
            child->setData(0, PathRole, base.isEmpty() ? qs(n.name) : base + "/" + qs(n.name));
            child->setData(0, ParentTreeRole, qs(id_hex(id)));
            child->setText(2, format_time(n.mtime));
            switch (n.kind) {
                case NodeKind::Dir:
                    child->setIcon(0, icon(QStringLiteral("folder_20_regular")));
                    child->setData(0, SubtreeRole, qs(id_hex(n.subtree)));
                    new QTreeWidgetItem(child);  // placeholder so it can be expanded
                    break;
                case NodeKind::File:
                    child->setIcon(0, icon(QStringLiteral("document_20_regular")));
                    child->setText(1, format_bytes(n.size));
                    break;
                case NodeKind::Symlink:
                    child->setIcon(0, icon(QStringLiteral("link_20_regular")));
                    child->setText(1, "-> " + qs(n.link_target));
                    break;
                case NodeKind::Reparse:
                    child->setIcon(0, icon(QStringLiteral("link_20_regular")));
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
        const bool yes = plain_question(
            this, windowTitle(),
            overwrite_->isChecked()
                ? tr("Restore %1 to its original location and REPLACE the files that are there now?").arg(original)
                : tr("Restore %1 to its original location? Files that exist already are left alone.").arg(original));
        if (!yes) return;
    } else {
        target = target_->text().trimmed();
        if (target.isEmpty()) {
            QMessageBox::warning(this, windowTitle(), tr("Choose a folder to restore into."));
            return;
        }
    }

    QStringList args = QStringList{"restore"} << repo_args();
    args << qs(id_hex(snap.id)) << "--target" << target;
    if (!path.isEmpty()) args << "--path" << path;
    if (overwrite_->isChecked()) args << "--overwrite";
    if (permissions_->isChecked()) args << "--permissions";
    warnings_ = 0;
    bar_->show();
    restore_->setEnabled(false);
    move_->setEnabled(false);
    thaw_->setEnabled(false);
    snapshot_->setEnabled(false);
    status_->setText(tr("Starting..."));
    runner_->start(args, password_, cloud_);
}

QStringList RestoreDialog::repo_args() const {
    if (job_.id.empty()) return {QStringLiteral("--repo"), qs(job_.repository)};
    return {QStringLiteral("--job"), qs(job_.id)};
}

void RestoreDialog::show_archived_note() {
    const std::string cls = job_.id.empty() ? cloud_.storage_class : job_.cloud_storage_class;
    const bool archived = storage_class_needs_thaw(cls);
    thaw_->setVisible(archived);
    if (archived)
        status_->setText(tr("This backup keeps its data in archive storage (%1). Pick what to restore, choose Request "
                            "thaw and wait until it is done (hours), then restore it.")
                             .arg(qs(cls)));
}

void RestoreDialog::request_thaw() {
    if (runner_->running() || snapshots_.empty()) return;
    const SnapshotEntry& snap = snapshots_[static_cast<std::size_t>(snapshot_->currentIndex())];
    const QString path = selected_path();
    QStringList args = QStringList{"cloud-thaw"} << repo_args() << qs(id_hex(snap.id));
    if (!path.isEmpty()) args << "--path" << path;
    thaw_running_ = true;
    bar_->show();
    restore_->setEnabled(false);
    move_->setEnabled(false);
    thaw_->setEnabled(false);
    snapshot_->setEnabled(false);
    status_->setText(path.isEmpty() ? tr("Asking for a thaw of this version...")
                                    : tr("Asking for a thaw of %1...").arg(path));
    runner_->start(args, password_, cloud_);
}

void RestoreDialog::move_here() {
    if (runner_->running() || snapshots_.empty()) return;
    const SnapshotEntry& snap = snapshots_[static_cast<std::size_t>(snapshot_->currentIndex())];
    std::vector<vaultik::MoveItem> plan;
    try {
        plan = vaultik::plan_move(*repo_, snap.snapshot);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, windowTitle(), QString::fromUtf8(e.what()));
        return;
    }
    if (plan.empty()) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This version holds none of an account's own folders (Documents, Desktop, ...)."));
        return;
    }

    QDialog choose(this);
    choose.setWindowTitle(tr("Move to this PC"));
    choose.resize(720, 420);
    auto* layout = new QVBoxLayout(&choose);
    auto* intro = new QLabel(tr("These go into the same folders of your account on this computer. Files that are "
                                "there already are left alone, unless you tick the box below."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* account = new QComboBox;
    for (const std::string& a : vaultik::plan_accounts(plan)) account->addItem(qs(a));
    auto* account_row = new QHBoxLayout;
    account_row->addWidget(new QLabel(tr("From the account:")));
    account_row->addWidget(account, 1);
    layout->addLayout(account_row);
    auto* list = new QListWidget;
    layout->addWidget(list, 1);
    const auto fill = [&] {
        list->clear();
        for (const vaultik::MoveItem& m : plan) {
            if (qs(m.account) != account->currentText()) continue;
            auto* item = new QListWidgetItem(tr("%1  →  %2").arg(qs(m.from).replace('/', '\\'), qs(m.to)));
            item->setData(Qt::UserRole, qs(m.id));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
            list->addItem(item);
        }
    };
    fill();
    connect(account, &QComboBox::currentIndexChanged, &choose, fill);
    auto* replace = new QCheckBox(tr("Replace files that are there already"));
    layout->addWidget(replace);
    auto* note = new QLabel(tr("Close the browsers and email programs on this computer first. Saved browser passwords "
                               "were sealed for the old account: the browser asks you to sign in again."));
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton* go = buttons->addButton(tr("Move"), QDialogButtonBox::AcceptRole);
    set_role(go, "primary");
    connect(go, &QPushButton::clicked, &choose, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &choose, &QDialog::reject);
    layout->addWidget(buttons);
    if (choose.exec() != QDialog::Accepted) return;

    QStringList ids;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked) ids << list->item(i)->data(Qt::UserRole).toString();
    if (ids.isEmpty()) return;
    QStringList args = QStringList{"move-here"} << repo_args();
    args << qs(id_hex(snap.id)) << "--account" << account->currentText() << "--only" << ids.join(',');
    if (replace->isChecked()) args << "--overwrite";
    warnings_ = 0;
    bar_->show();
    restore_->setEnabled(false);
    move_->setEnabled(false);
    thaw_->setEnabled(false);
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
