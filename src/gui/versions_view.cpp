#include "versions_view.h"

#include <QDate>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QStackedLayout>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common.h"
#include "theme.h"

using namespace vaultik;

namespace {

constexpr int kIndexRole = Qt::UserRole + 1;  // the version's position; -1 for a group header

enum Column { kTime, kChanges, kSize, kAdded, kNote, kColumns };

QString group_of(const QDate& day, const QDate& today) {
    if (day == today) return QObject::tr("Today");
    if (day == today.addDays(-1)) return QObject::tr("Yesterday");
    const QDate week_start = today.addDays(1 - today.dayOfWeek());
    if (day >= week_start) return QObject::tr("Earlier this week");
    if (day.year() == today.year() && day.month() == today.month()) return QObject::tr("Earlier this month");
    const QLocale locale;
    return day.year() == today.year() ? locale.standaloneMonthName(day.month())
                                      : locale.standaloneMonthName(day.month()) + " " + QString::number(day.year());
}

QString changes_text(const Snapshot& s) {
    const SnapshotStats& st = s.stats;
    const QLocale locale;
    if (!st.changes_known) return count_of(st.files, QObject::tr("file"), QObject::tr("files"));
    if (!s.parent) return QObject::tr("First backup, %1").arg(count_of(st.files, QObject::tr("file"), QObject::tr("files")));
    QStringList parts;
    if (st.files_new) parts << QObject::tr("%1 new").arg(locale.toString(static_cast<qulonglong>(st.files_new)));
    if (st.files_changed) parts << QObject::tr("%1 changed").arg(locale.toString(static_cast<qulonglong>(st.files_changed)));
    if (st.items_removed) parts << QObject::tr("%1 removed").arg(locale.toString(static_cast<qulonglong>(st.items_removed)));
    return parts.isEmpty() ? QObject::tr("No changes") : parts.join(QStringLiteral(", "));
}

}  // namespace

VersionsView::VersionsView(QWidget* parent) : QWidget(parent) {
    restore_ = new QPushButton(tr("Restore..."));
    explore_ = new QPushButton(tr("Open in Explorer"));
    explore_->setToolTip(tr("Show this version as a read-only folder in File Explorer; files are read from the "
                            "backup when you open them"));
    pin_ = new QPushButton(tr("Pin"));
    note_ = new QPushButton(tr("Note..."));
    delete_ = new QPushButton(tr("Delete"));
    restore_->setToolTip(tr("Browse this version and restore files from it"));
    pin_->setToolTip(tr("A pinned version is never removed by cleanup"));
    note_->setToolTip(tr("Describe this version, e.g. \"before reinstalling Windows\""));
    delete_->setToolTip(tr("Remove this version now; its space is freed at the next cleanup"));
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    actions->addWidget(restore_);
    actions->addWidget(explore_);
    actions->addWidget(pin_);
    actions->addWidget(note_);
    actions->addStretch();
    actions->addWidget(delete_);

    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(kColumns);
    tree_->setHeaderLabels({tr("Backed up"), tr("Changes"), tr("Size"), tr("Added"), tr("Note")});
    tree_->setRootIsDecorated(false);
    tree_->setIndentation(0);
    tree_->setUniformRowHeights(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setStretchLastSection(true);
    tree_->header()->setSectionsMovable(false);

    message_ = new QLabel(this);
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    message_->setProperty("role", "muted");

    auto* stack_host = new QWidget(this);
    auto* stack = new QStackedLayout(stack_host);
    stack->addWidget(tree_);
    stack->addWidget(message_);

    footer_ = new QLabel(this);
    footer_->setProperty("role", "faint");
    footer_->setWordWrap(true);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 8, 0, 0);
    layout->addLayout(actions);
    layout->addWidget(stack_host, 1);
    layout->addWidget(footer_);

    connect(tree_, &QTreeWidget::itemSelectionChanged, this, &VersionsView::update_buttons);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const int i = item->data(0, kIndexRole).toInt();
        if (i >= 0) emit restore_requested(i);
    });
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &VersionsView::context_menu);
    connect(restore_, &QPushButton::clicked, this, [this] {
        if (const int i = selected_index(); i >= 0) emit restore_requested(i);
    });
    connect(explore_, &QPushButton::clicked, this, [this] {
        if (const int i = selected_index(); i >= 0) emit explore_requested(i);
    });
    connect(pin_, &QPushButton::clicked, this, [this] {
        if (const int i = selected_index(); i >= 0) emit pin_requested(i, !versions_[static_cast<std::size_t>(i)].snapshot.pinned);
    });
    connect(note_, &QPushButton::clicked, this, [this] {
        if (const int i = selected_index(); i >= 0) emit note_requested(i);
    });
    connect(delete_, &QPushButton::clicked, this, [this] {
        if (const int i = selected_index(); i >= 0) emit delete_requested(i);
    });
    refresh_icons();
    set_message(QString());
}

void VersionsView::refresh_icons() {
    restore_->setIcon(icon(QStringLiteral("arrow_counterclockwise_20_regular")));
    explore_->setIcon(icon(QStringLiteral("folder_open_20_regular")));
    pin_->setIcon(icon(QStringLiteral("pin_20_regular")));
    note_->setIcon(icon(QStringLiteral("note_20_regular")));
    delete_->setIcon(icon(QStringLiteral("delete_20_regular")));
    if (!versions_.empty()) set_versions(std::vector<SnapshotEntry>(versions_), std::set<Id>(leaving_), footer_->text());
}

void VersionsView::set_message(const QString& text) {
    versions_.clear();
    tree_->clear();
    message_->setText(text);
    static_cast<QStackedLayout*>(message_->parentWidget()->layout())->setCurrentWidget(message_);
    footer_->clear();
    update_buttons();
}

void VersionsView::set_versions(const std::vector<SnapshotEntry>& newest_first, const std::set<Id>& leaving,
                                const QString& rules) {
    // Keep the selection across a refresh (after pinning, for example).
    Id selected{};
    bool had_selection = false;
    if (const int i = selected_index(); i >= 0) {
        selected = versions_[static_cast<std::size_t>(i)].id;
        had_selection = true;
    }
    versions_ = newest_first;
    leaving_ = leaving;
    tree_->clear();
    if (versions_.empty()) {
        set_message(tr("No versions yet. The first backup creates one."));
        return;
    }
    static_cast<QStackedLayout*>(message_->parentWidget()->layout())->setCurrentWidget(tree_);

    const Theme& t = theme();
    const QDate today = QDate::currentDate();
    const QLocale locale;
    QString current_group;
    QFont group_font = tree_->font();
    group_font.setWeight(QFont::DemiBold);
    QTreeWidgetItem* select_item = nullptr;
    for (std::size_t i = 0; i < versions_.size(); ++i) {
        const Snapshot& s = versions_[i].snapshot;
        const QDateTime when = to_datetime(s.time);
        const QString group = group_of(when.date(), today);
        if (group != current_group) {
            current_group = group;
            auto* header = new QTreeWidgetItem(tree_);
            header->setText(0, group);
            header->setData(0, kIndexRole, -1);
            header->setFirstColumnSpanned(true);
            header->setFlags(Qt::ItemIsEnabled);
            header->setFont(0, group_font);
            header->setForeground(0, t.text_muted);
        }
        auto* item = new QTreeWidgetItem(tree_);
        item->setData(0, kIndexRole, static_cast<int>(i));
        const bool recent = when.date() >= today.addDays(-1);
        item->setText(kTime, recent ? locale.toString(when.time(), QLocale::ShortFormat)
                                    : locale.toString(when, QStringLiteral("ddd d MMM, ")) +
                                          locale.toString(when.time(), QLocale::ShortFormat));
        item->setToolTip(kTime, locale.toString(when, QLocale::LongFormat) + "\n" + qs(id_hex(versions_[i].id).substr(0, 8)));
        item->setText(kChanges, changes_text(s));
        item->setText(kSize, format_bytes(s.stats.bytes));
        item->setText(kAdded, s.stats.stored_bytes > 0 ? "+" + format_bytes(s.stats.stored_bytes) : QString());
        item->setToolTip(kAdded, tr("Space this backup added after compression and encryption"));
        item->setTextAlignment(kSize, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(kAdded, Qt::AlignRight | Qt::AlignVCenter);

        if (s.pinned) {
            item->setIcon(kTime, icon(QStringLiteral("pin_20_filled"), t.accent));
        } else if (s.stats.errors > 0) {
            item->setIcon(kTime, status_icon(jobs::RunStatus::Partial));
            item->setToolTip(kChanges, tr("%1 could not be read and are missing from this version")
                                           .arg(count_of(s.stats.errors, tr("file"), tr("files"))));
        } else {
            item->setIcon(kTime, icon(QStringLiteral("checkmark_circle_20_filled"), t.success));
        }
        QString note = qs(s.note);
        if (!s.pinned && leaving_.contains(versions_[i].id)) {
            item->setForeground(kNote, t.text_faint);
            note = note.isEmpty() ? tr("Removed at the next cleanup") : note + "  (" + tr("removed at the next cleanup") + ")";
        }
        item->setText(kNote, note);
        if (had_selection && versions_[i].id == selected) select_item = item;
    }
    for (int c = 0; c < kNote; ++c) tree_->resizeColumnToContents(c);
    tree_->setColumnWidth(kTime, tree_->columnWidth(kTime) + 12);
    if (select_item != nullptr) tree_->setCurrentItem(select_item);
    footer_->setText(rules);
    update_buttons();
}

int VersionsView::selected_index() const {
    const QList<QTreeWidgetItem*> sel = tree_->selectedItems();
    if (sel.isEmpty()) return -1;
    const int i = sel.front()->data(0, kIndexRole).toInt();
    return i >= 0 && i < static_cast<int>(versions_.size()) ? i : -1;
}

void VersionsView::update_buttons() {
    const int i = selected_index();
    for (QPushButton* b : {restore_, explore_, pin_, note_, delete_}) b->setEnabled(i >= 0);
    const bool pinned = i >= 0 && versions_[static_cast<std::size_t>(i)].snapshot.pinned;
    pin_->setText(pinned ? tr("Unpin") : tr("Pin"));
    delete_->setEnabled(i >= 0 && !pinned);
    delete_->setToolTip(pinned ? tr("Unpin this version to delete it")
                               : tr("Remove this version now; its space is freed at the next cleanup"));
}

void VersionsView::context_menu(const QPoint& pos) {
    QTreeWidgetItem* item = tree_->itemAt(pos);
    if (item == nullptr || item->data(0, kIndexRole).toInt() < 0) return;
    tree_->setCurrentItem(item);
    const int i = item->data(0, kIndexRole).toInt();
    const bool pinned = versions_[static_cast<std::size_t>(i)].snapshot.pinned;
    QMenu menu(this);
    menu.addAction(icon(QStringLiteral("arrow_counterclockwise_20_regular")), tr("Restore..."), this,
                   [this, i] { emit restore_requested(i); });
    menu.addAction(icon(QStringLiteral("folder_open_20_regular")), tr("Open in Explorer"), this,
                   [this, i] { emit explore_requested(i); });
    menu.addAction(icon(QStringLiteral("pin_20_regular")), pinned ? tr("Unpin") : tr("Pin"), this,
                   [this, i, pinned] { emit pin_requested(i, !pinned); });
    menu.addAction(icon(QStringLiteral("note_20_regular")), tr("Note..."), this, [this, i] { emit note_requested(i); });
    menu.addSeparator();
    QAction* del = menu.addAction(icon(QStringLiteral("delete_20_regular")), tr("Delete"), this,
                                  [this, i] { emit delete_requested(i); });
    del->setEnabled(!pinned);
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}
