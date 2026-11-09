#include "find_page.h"

#include <algorithm>
#include <cstdlib>

#include <QCheckBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common.h"
#include "preview.h"
#include "theme.h"

using namespace vaultik;

namespace {

constexpr int kDataRole = Qt::UserRole + 1;

QString kind_icon(NodeKind kind) {
    switch (kind) {
        case NodeKind::Dir: return QStringLiteral("folder_20_regular");
        case NodeKind::File: return QStringLiteral("document_20_regular");
        default: return QStringLiteral("link_20_regular");
    }
}

// How a version compares with the item on this computer now.
QString compare_with_disk(const QString& snapshot_path, const Node& node) {
    const QString original = original_path(snapshot_path);
    if (original.isEmpty()) return {};
    const QFileInfo fi(original);
    if (!fi.exists()) return QObject::tr("Not on this PC now");
    if (node.kind == NodeKind::Dir) return fi.isDir() ? QObject::tr("Folder exists") : QObject::tr("Different now");
    if (node.kind != NodeKind::File) return {};
    const QDateTime mtime = to_datetime(node.mtime);
    const bool same = fi.isFile() && static_cast<std::uint64_t>(fi.size()) == node.size && mtime.isValid() &&
                      std::llabs(fi.lastModified().msecsTo(mtime)) < 2000;
    return same ? QObject::tr("Same as now") : QObject::tr("Different from now");
}

}  // namespace

// ---- ItemHistoryView ---------------------------------------------------------------------------

ItemHistoryView::ItemHistoryView(QWidget* parent) : QWidget(parent) {
    title_ = new QLabel(this);
    title_->setProperty("role", "heading");
    location_ = new QLabel(this);
    location_->setProperty("role", "muted");
    location_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    all_ = new QCheckBox(tr("Also show versions where it did not change"), this);
    restore_ = new QPushButton(tr("Restore this version..."), this);
    restore_->setProperty("role", "primary");
    preview_ = new QPushButton(tr("Preview"), this);
    preview_->setToolTip(tr("Look inside this version without restoring it"));

    tree_ = new QTreeWidget(this);
    tree_->setHeaderLabels({tr("Backed up"), tr("Modified"), tr("Size"), tr("Compared with this PC")});
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(0);
    tree_->header()->setStretchLastSection(true);

    auto* top = new QHBoxLayout;
    top->setContentsMargins(0, 0, 0, 0);
    auto* names = new QVBoxLayout;
    names->addWidget(title_);
    names->addWidget(location_);
    top->addLayout(names, 1);
    top->addWidget(preview_, 0, Qt::AlignBottom);
    top->addWidget(restore_, 0, Qt::AlignBottom);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->addLayout(top);
    layout->addWidget(all_);
    layout->addWidget(tree_, 1);

    connect(all_, &QCheckBox::toggled, this, &ItemHistoryView::fill);
    connect(tree_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const int i = selected_version();
        restore_->setEnabled(i >= 0);
        bool file = false;
        for (const ItemVersion& v : history_)
            if (static_cast<int>(v.snapshot) == i) file = v.node.kind == NodeKind::File;
        preview_->setEnabled(i >= 0 && file);
    });
    connect(preview_, &QPushButton::clicked, this, [this] {
        const int i = selected_version();
        for (const ItemVersion& v : history_) {
            if (static_cast<int>(v.snapshot) != i) continue;
            const int slash = path_.lastIndexOf('/');
            const QString name = slash >= 0 ? path_.mid(slash + 1) : path_;
            const QString when = QLocale().toString(to_datetime(versions_[v.snapshot].snapshot.time), QLocale::ShortFormat);
            PreviewDialog dialog(repo_, v.node, name, tr("backed up %1").arg(when), this);
            dialog.exec();
            return;
        }
    });
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (const int i = selected_version(); i >= 0) emit restore_requested(i, path_);
    });
    connect(restore_, &QPushButton::clicked, this, [this] {
        if (const int i = selected_version(); i >= 0) emit restore_requested(i, path_);
    });
    refresh_icons();
    clear();
}

void ItemHistoryView::refresh_icons() {
    restore_->setIcon(icon(QStringLiteral("arrow_counterclockwise_20_regular"), theme().accent_text));
    preview_->setIcon(icon(QStringLiteral("document_20_regular")));
    if (!history_.empty()) fill();
}

void ItemHistoryView::set_source(std::shared_ptr<Repository> repo, std::vector<SnapshotEntry> versions) {
    repo_ = std::move(repo);
    versions_ = std::move(versions);
    clear();
}

void ItemHistoryView::clear() {
    path_.clear();
    history_.clear();
    tree_->clear();
    title_->setText(tr("Pick a file or folder"));
    location_->setText(tr("Its versions appear here, with when it changed."));
    restore_->setEnabled(false);
    preview_->setEnabled(false);
    all_->setEnabled(false);
}

void ItemHistoryView::show_item(const QString& path) {
    if (!repo_) return;
    clear();
    path_ = path;
    const int slash = path.lastIndexOf('/');
    title_->setText(slash >= 0 ? path.mid(slash + 1) : path);
    const QString original = original_path(path);
    location_->setText(original.isEmpty() ? path : original);

    std::vector<ItemVersion> found;
    const std::shared_ptr<Repository> repo = repo_;
    const std::vector<SnapshotRecord> records = records_of(versions_);
    const std::string p = u8(path);
    const QString error = run_busy(this, tr("Looking through the versions..."),
                                   [&](const std::function<bool()>& cancelled) {
                                       found = item_history(*repo, records, p, cancelled);
                                   });
    if (!error.isEmpty()) {
        location_->setText(error);
        return;
    }
    history_ = std::move(found);
    all_->setEnabled(true);
    fill();
}

void ItemHistoryView::fill() {
    tree_->clear();
    const QLocale locale;
    for (std::size_t k = 0; k < history_.size(); ++k) {
        const ItemVersion& v = history_[k];
        if (!all_->isChecked() && !v.changed) continue;
        const Snapshot& s = versions_[v.snapshot].snapshot;
        auto* item = new QTreeWidgetItem(tree_);
        item->setData(0, kDataRole, static_cast<int>(v.snapshot));
        item->setText(0, locale.toString(to_datetime(s.time), QLocale::ShortFormat));
        item->setIcon(0, icon(kind_icon(v.node.kind)));
        item->setText(1, v.node.kind == NodeKind::Dir ? QString() : locale.toString(to_datetime(v.node.mtime), QLocale::ShortFormat));
        item->setText(2, v.node.kind == NodeKind::File ? format_bytes(v.node.size) : QString());
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        QString compare = compare_with_disk(path_, v.node);
        if (!v.changed) compare = tr("Unchanged") + (compare.isEmpty() ? QString() : ", " + compare.toLower());
        item->setText(3, compare);
        if (!v.changed) {
            for (int c = 0; c < 4; ++c) item->setForeground(c, theme().text_faint);
        }
    }
    for (int c = 0; c < 3; ++c) tree_->resizeColumnToContents(c);
    const std::size_t changed = static_cast<std::size_t>(
        std::count_if(history_.begin(), history_.end(), [](const ItemVersion& v) { return v.changed; }));
    QString text = original_path(path_);
    if (text.isEmpty()) text = path_;
    text += "  ·  " + tr("%1, in %2 of %3 backups")
                          .arg(count_of(changed, tr("version"), tr("versions")))
                          .arg(history_.size())
                          .arg(versions_.size());
    location_->setText(text);
    if (tree_->topLevelItemCount() > 0) tree_->setCurrentItem(tree_->topLevelItem(0));
}

int ItemHistoryView::selected_version() const {
    const QList<QTreeWidgetItem*> sel = tree_->selectedItems();
    return sel.isEmpty() ? -1 : sel.front()->data(0, kDataRole).toInt();
}

// ---- FindPage ----------------------------------------------------------------------------------

FindPage::FindPage(QWidget* parent) : QWidget(parent) {
    query_ = new QLineEdit(this);
    query_->setPlaceholderText(tr("File or folder name, e.g. budget or *.xlsx"));
    query_->setClearButtonEnabled(true);
    search_ = new QPushButton(tr("Search"), this);
    search_->setProperty("role", "primary");
    status_ = new QLabel(this);
    status_->setProperty("role", "muted");

    results_ = new QTreeWidget(this);
    results_->setHeaderLabels({tr("Name"), tr("Location"), tr("Size"), tr("In backups"), tr("Last backed up")});
    results_->setRootIsDecorated(false);
    results_->setIndentation(0);
    results_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    results_->header()->setStretchLastSection(false);

    history_ = new ItemHistoryView(this);

    auto* split = new QSplitter(Qt::Vertical, this);
    split->addWidget(results_);
    split->addWidget(history_);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);

    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(query_, 1);
    row->addWidget(search_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->addLayout(row);
    layout->addWidget(status_);
    layout->addWidget(split, 1);

    connect(search_, &QPushButton::clicked, this, &FindPage::search);
    connect(query_, &QLineEdit::returnPressed, this, &FindPage::search);
    connect(results_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QList<QTreeWidgetItem*> sel = results_->selectedItems();
        if (!sel.isEmpty()) history_->show_item(sel.front()->data(0, kDataRole).toString());
    });
    connect(history_, &ItemHistoryView::restore_requested, this, &FindPage::restore_requested);
    refresh_icons();
    set_source(nullptr, {});
}

void FindPage::refresh_icons() {
    search_->setIcon(icon(QStringLiteral("search_20_regular"), theme().accent_text));
    query_->setClearButtonEnabled(true);
    history_->refresh_icons();
    for (int i = 0; i < results_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = results_->topLevelItem(i);
        item->setIcon(0, icon(kind_icon(static_cast<NodeKind>(item->data(1, kDataRole).toInt()))));
    }
}

void FindPage::set_source(std::shared_ptr<Repository> repo, std::vector<SnapshotEntry> versions) {
    repo_ = std::move(repo);
    versions_ = std::move(versions);
    history_->set_source(repo_, versions_);
    results_->clear();
    const bool ready = repo_ != nullptr;
    query_->setEnabled(ready);
    search_->setEnabled(ready);
    status_->setText(ready ? tr("Searches the names in all %1.").arg(count_of(versions_.size(), tr("version"), tr("versions")))
                           : tr("Open the Versions tab first to load this backup."));
    if (ready && !pending_path_.isEmpty()) {
        const QString path = std::exchange(pending_path_, QString());
        show_path(path);
    }
}

void FindPage::show_path(const QString& snapshot_path) {
    if (!repo_) {
        pending_path_ = snapshot_path;
        return;
    }
    query_->setText(snapshot_path.section(QLatin1Char('/'), -1));
    history_->show_item(snapshot_path);
}

void FindPage::search() {
    const QString pattern = query_->text().trimmed();
    if (!repo_ || pattern.isEmpty()) return;
    std::vector<FoundItem> found;
    bool truncated = false;
    const std::shared_ptr<Repository> repo = repo_;
    const std::vector<SnapshotRecord> records = records_of(versions_);
    const std::string p = u8(pattern);
    const QString error = run_busy(this, tr("Searching all versions..."), [&](const std::function<bool()>& cancelled) {
        found = find_items(*repo, records, p, 1000, &truncated, cancelled);
    });
    results_->clear();
    history_->clear();
    if (!error.isEmpty()) {
        status_->setText(error);
        return;
    }
    const QLocale locale;
    for (const FoundItem& f : found) {
        auto* item = new QTreeWidgetItem(results_);
        const QString path = qs(f.path);
        const int slash = path.lastIndexOf('/');
        const QString original = original_path(slash >= 0 ? path.left(slash) : QString());
        item->setText(0, slash >= 0 ? path.mid(slash + 1) : path);
        item->setIcon(0, icon(kind_icon(f.kind)));
        item->setData(0, kDataRole, path);
        item->setData(1, kDataRole, static_cast<int>(f.kind));
        item->setText(1, original.isEmpty() ? (slash >= 0 ? path.left(slash) : QString()) : original);
        item->setText(2, f.kind == NodeKind::File ? format_bytes(f.size) : QString());
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(3, QString::number(f.snapshots.size()));
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        item->setText(4, locale.toString(to_datetime(versions_[f.snapshots.front()].snapshot.time), QLocale::ShortFormat));
    }
    results_->resizeColumnToContents(0);
    results_->resizeColumnToContents(2);
    results_->resizeColumnToContents(3);
    results_->resizeColumnToContents(4);
    QString text = found.empty() ? tr("Nothing matches \"%1\".").arg(pattern)
                                 : count_of(found.size(), tr("match"), tr("matches")) + ". " + tr("Pick one to see its versions.");
    if (truncated) text += " " + tr("Only the first ones are shown; a more precise name finds the rest.");
    status_->setText(text);
}
