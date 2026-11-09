#pragma once

#include <QStyledItemDelegate>

// Draws a job in the sidebar as a card: its name, the last run's result with a coloured status
// icon, and its schedule. Items carry the data in these roles (the name is Qt::DisplayRole).
class JobCardDelegate : public QStyledItemDelegate {
public:
    enum Role {
        StatusRole = Qt::UserRole + 1,  // int RunStatus, or -1 when the job never ran
        StatusTextRole,                 // "Completed, today 14:02"
        DetailRole,                     // "Daily at 02:00"
    };

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};
