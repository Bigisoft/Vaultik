#include "job_card.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>

#include "theme.h"

void JobCardDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    const Theme& t = theme();
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);

    const QRect card = option.rect.adjusted(2, 2, -2, -2);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    if (selected || hovered) {
        QPainterPath path;
        path.addRoundedRect(card, 6, 6);
        painter->fillPath(path, selected ? t.accent_soft : t.hover);
    }
    if (selected) {
        // The accent marker Windows uses for the current item in a navigation list.
        QPainterPath marker;
        marker.addRoundedRect(QRectF(card.left() + 1, card.center().y() - 9, 3, 18), 1.5, 1.5);
        painter->fillPath(marker, t.accent);
    }

    const int left = card.left() + 14;
    const int width = card.right() - 10 - left;
    QFont name_font = option.font;
    name_font.setWeight(QFont::DemiBold);
    QFont small_font = option.font;
    small_font.setPointSizeF(option.font.pointSizeF() * 0.9);
    const QFontMetrics name_fm(name_font);
    const QFontMetrics small_fm(small_font);

    int y = card.top() + 9;
    painter->setFont(name_font);
    painter->setPen(t.text);
    painter->drawText(QRect(left, y, width, name_fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      name_fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, width));
    y += name_fm.height() + 3;

    painter->setFont(small_font);
    int text_left = left;
    const int status = index.data(StatusRole).toInt();
    if (status >= 0) {
        const int size = small_fm.height();
        status_icon(static_cast<vaultik::jobs::RunStatus>(status))
            .paint(painter, QRect(left, y, size, size));
        text_left += size + 5;
    }
    painter->setPen(t.text_muted);
    painter->drawText(QRect(text_left, y, card.right() - 10 - text_left, small_fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      small_fm.elidedText(index.data(StatusTextRole).toString(), Qt::ElideRight,
                                          card.right() - 10 - text_left));
    y += small_fm.height() + 2;
    painter->setPen(t.text_faint);
    painter->drawText(QRect(left, y, width, small_fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                      small_fm.elidedText(index.data(DetailRole).toString(), Qt::ElideRight, width));
    painter->restore();
}

QSize JobCardDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const {
    QFont name_font = option.font;
    name_font.setWeight(QFont::DemiBold);
    QFont small_font = option.font;
    small_font.setPointSizeF(option.font.pointSizeF() * 0.9);
    const int h = QFontMetrics(name_font).height() + 2 * QFontMetrics(small_font).height() + 9 + 3 + 2 + 12;
    return {200, h};
}
