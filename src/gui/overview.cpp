#include "overview.h"

#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QToolTip>
#include <QVBoxLayout>

#include "theme.h"

namespace {

constexpr int kCell = 13;
constexpr int kGap = 3;
constexpr int kWeeks = 26;     // about half a year
constexpr int kLabelWidth = 30;  // weekday names on the left
constexpr int kHeaderHeight = 16;  // month names on top

QColor day_color(OverviewData::Day d) {
    const Theme& t = theme();
    switch (d) {
        case OverviewData::Day::Saved: return t.success;
        case OverviewData::Day::Partial: return t.warning;
        case OverviewData::Day::Failed: return t.danger;
        case OverviewData::Day::None: break;
    }
    return t.border;
}

QString day_text(OverviewData::Day d) {
    switch (d) {
        case OverviewData::Day::Saved: return QObject::tr("a version was saved");
        case OverviewData::Day::Partial: return QObject::tr("saved, some files skipped");
        case OverviewData::Day::Failed: return QObject::tr("the backup failed");
        case OverviewData::Day::None: break;
    }
    return QObject::tr("no backup");
}

}  // namespace

BackupCalendar::BackupCalendar(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void BackupCalendar::set_days(std::map<QDate, OverviewData::Day> days) {
    days_ = std::move(days);
    update();
}

int BackupCalendar::weeks() const {
    return kWeeks;
}

QDate BackupCalendar::first_day() const {
    const QDate today = QDate::currentDate();
    const QDate this_monday = today.addDays(1 - today.dayOfWeek());
    return this_monday.addDays(-7 * (weeks() - 1));
}

QSize BackupCalendar::sizeHint() const {
    return {kLabelWidth + weeks() * (kCell + kGap), kHeaderHeight + 7 * (kCell + kGap)};
}

QDate BackupCalendar::day_at(const QPoint& pos) const {
    const int x = pos.x() - kLabelWidth;
    const int y = pos.y() - kHeaderHeight;
    if (x < 0 || y < 0) return {};
    const int col = x / (kCell + kGap);
    const int row = y / (kCell + kGap);
    if (col >= weeks() || row >= 7 || x % (kCell + kGap) >= kCell || y % (kCell + kGap) >= kCell) return {};
    const QDate d = first_day().addDays(col * 7 + row);
    return d > QDate::currentDate() ? QDate() : d;
}

void BackupCalendar::paintEvent(QPaintEvent*) {
    const Theme& t = theme();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QDate first = first_day();
    const QDate today = QDate::currentDate();
    const QLocale locale;

    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    p.setFont(small);
    p.setPen(t.text_muted);
    for (int row : {0, 2, 4}) {  // Mon, Wed, Fri
        const QRect r(0, kHeaderHeight + row * (kCell + kGap), kLabelWidth - 6, kCell);
        p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, locale.dayName(row + 1, QLocale::ShortFormat));
    }
    int last_month = -1;
    int last_label = -10;
    for (int col = 0; col < weeks(); ++col) {
        // A month's name over its first week; skipped when it would crowd the one before (the
        // first column usually shows only the end of a month).
        const QDate monday = first.addDays(col * 7);
        const QDate sunday = monday.addDays(6);
        const int month = col == 0 ? monday.month() : sunday.month();
        if (month != last_month) {
            last_month = month;
            p.setPen(t.text_muted);  // the cells below switch the pen off
            const bool stub = col == 0 && first.addDays(20).month() != month;  // under 3 weeks shown
            if (!stub && col - last_label >= 3 && col < weeks() - 1) {
                last_label = col;
                p.drawText(QRect(kLabelWidth + col * (kCell + kGap), 0, 60, kHeaderHeight - 2),
                           Qt::AlignLeft | Qt::AlignBottom, locale.monthName(month, QLocale::ShortFormat));
            }
        }
        for (int row = 0; row < 7; ++row) {
            const QDate d = monday.addDays(row);
            if (d > today) continue;
            const auto it = days_.find(d);
            const OverviewData::Day kind = it == days_.end() ? OverviewData::Day::None : it->second;
            const QRectF cell(kLabelWidth + col * (kCell + kGap), kHeaderHeight + row * (kCell + kGap), kCell, kCell);
            p.setPen(Qt::NoPen);
            p.setBrush(day_color(kind));
            p.drawRoundedRect(cell, 3, 3);
            if (d == today) {
                p.setPen(QPen(t.text, 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(cell.adjusted(-1.5, -1.5, 1.5, 1.5), 4, 4);
            }
        }
    }
}

bool BackupCalendar::event(QEvent* event) {
    if (event->type() == QEvent::ToolTip) {
        auto* help = static_cast<QHelpEvent*>(event);
        const QDate d = day_at(help->pos());
        if (d.isValid()) {
            const auto it = days_.find(d);
            QToolTip::showText(help->globalPos(),
                               QLocale().toString(d, QLocale::LongFormat) + ": " +
                                   day_text(it == days_.end() ? OverviewData::Day::None : it->second),
                               this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(event);
}

OverviewView::Tile OverviewView::make_tile(const QString& title, QWidget*& frame) {
    auto* f = new QFrame;
    f->setProperty("role", "card");
    auto* l = new QVBoxLayout(f);
    l->setContentsMargins(16, 14, 16, 14);
    l->setSpacing(4);
    Tile t;
    t.title = new QLabel(title);
    t.title->setProperty("role", "section");
    t.value = new QLabel;
    t.value->setProperty("role", "heading");
    t.value->setTextFormat(Qt::PlainText);
    t.detail = new QLabel;
    t.detail->setProperty("role", "muted");
    t.detail->setTextFormat(Qt::PlainText);
    t.detail->setWordWrap(true);
    l->addWidget(t.title);
    l->addWidget(t.value);
    l->addWidget(t.detail);
    l->addStretch();
    frame = f;
    return t;
}

OverviewView::OverviewView(QWidget* parent) : QWidget(parent) {
    // The page scrolls when the window is too low for it, instead of squeezing the tiles.
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea, QScrollArea > QWidget > QWidget { background: transparent; }"));
    auto* content = new QWidget;
    scroll->setWidget(content);
    outer->addWidget(scroll);

    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 12, 6, 0);
    layout->setSpacing(12);

    // A banner when something needs attention.
    auto* banner = new QFrame;
    banner->setProperty("role", "card");
    auto* bl = new QHBoxLayout(banner);
    bl->setContentsMargins(14, 10, 14, 10);
    auto* banner_icon = new QLabel;
    banner_icon->setPixmap(status_icon(vaultik::jobs::RunStatus::Failed).pixmap(20, 20));
    warning_text_ = new QLabel;
    warning_text_->setWordWrap(true);
    warning_text_->setTextFormat(Qt::PlainText);
    bl->addWidget(banner_icon, 0, Qt::AlignTop);
    bl->addWidget(warning_text_, 1);
    warning_ = banner;
    layout->addWidget(warning_);

    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    QWidget* f = nullptr;
    last_ = make_tile(tr("Last backup"), f);
    grid->addWidget(f, 0, 0);
    next_ = make_tile(tr("Next backup"), f);
    grid->addWidget(f, 0, 1);
    versions_ = make_tile(tr("Versions kept"), f);
    auto* show_versions = new QPushButton(tr("Show versions"));
    show_versions->setProperty("role", "flat");
    connect(show_versions, &QPushButton::clicked, this, &OverviewView::versions_requested);
    static_cast<QVBoxLayout*>(f->layout())->addWidget(show_versions, 0, Qt::AlignLeft);
    grid->addWidget(f, 1, 0);
    health_ = make_tile(tr("Health check"), f);
    check_ = new QPushButton(tr("Check now"));
    check_->setProperty("role", "flat");
    connect(check_, &QPushButton::clicked, this, &OverviewView::check_requested);
    static_cast<QVBoxLayout*>(f->layout())->addWidget(check_, 0, Qt::AlignLeft);
    grid->addWidget(f, 1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    layout->addLayout(grid);

    // The drives involved and their health; filled by set_data.
    auto* drives = new QFrame;
    drives->setProperty("role", "card");
    auto* dl = new QVBoxLayout(drives);
    dl->setContentsMargins(16, 14, 16, 14);
    dl->setSpacing(6);
    auto* drives_title = new QLabel(tr("Drives"));
    drives_title->setProperty("role", "section");
    dl->addWidget(drives_title);
    drives_list_ = new QWidget;
    new QGridLayout(drives_list_);
    drives_list_->layout()->setContentsMargins(0, 0, 0, 0);
    dl->addWidget(drives_list_);
    drives_box_ = drives;
    layout->addWidget(drives_box_);

    auto* cal = new QFrame;
    cal->setProperty("role", "card");
    auto* cl = new QVBoxLayout(cal);
    cl->setContentsMargins(16, 14, 16, 14);
    auto* cal_title = new QLabel(tr("Backup days"));
    cal_title->setProperty("role", "section");
    cl->addWidget(cal_title);
    calendar_ = new BackupCalendar;
    cl->addWidget(calendar_);
    auto* legend = new QHBoxLayout;
    legend->setSpacing(14);
    for (const auto d : {OverviewData::Day::Saved, OverviewData::Day::Partial, OverviewData::Day::Failed,
                         OverviewData::Day::None}) {
        QPixmap swatch(12, 12);
        swatch.fill(Qt::transparent);
        QPainter p(&swatch);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(day_color(d));
        p.drawRoundedRect(QRectF(0, 0, 12, 12), 3, 3);
        p.end();
        auto* box = new QLabel;
        box->setPixmap(swatch);
        auto* text = new QLabel(day_text(d));
        text->setProperty("role", "muted");
        legend->addWidget(box);
        legend->addWidget(text);
    }
    legend->addStretch();
    cl->addLayout(legend);
    layout->addWidget(cal);
    layout->addStretch();
}

void OverviewView::set_data(const OverviewData& d) {
    warning_->setVisible(!d.warning.isEmpty());
    warning_text_->setText(d.warning);

    if (d.has_backup) {
        last_.value->setText(d.backup_when);
        last_.detail->setText(d.backup_detail);
    } else {
        last_.value->setText(tr("Not backed up yet"));
        last_.detail->setText(tr("Choose Back up now to save the first version."));
    }
    next_.value->setText(d.next_run);
    next_.detail->setText(d.next_detail);
    versions_.value->setText(d.versions);
    versions_.detail->setText(d.versions_detail + (d.size.isEmpty() ? QString() : "\n" + d.size) +
                              (d.size_detail.isEmpty() ? QString() : "\n" + d.size_detail));
    if (d.has_check) {
        health_.value->setText(d.check_ok ? tr("No problems found") : tr("Problems found"));
        health_.detail->setText(d.check_when + (d.check_detail.isEmpty() ? QString() : "\n" + d.check_detail));
    } else {
        health_.value->setText(tr("Not checked yet"));
        health_.detail->setText(tr("A check runs after a backup once a week, or choose Check now."));
    }
    calendar_->set_days(d.days);

    // One row per drive: its role, the drive, and its health (in colour when it has a problem).
    auto* grid = static_cast<QGridLayout*>(drives_list_->layout());
    while (QLayoutItem* item = grid->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    drives_box_->setVisible(!d.drives.empty());
    const Theme& t = theme();
    int row = 0;
    for (const OverviewData::Drive& drive : d.drives) {
        auto* role = new QLabel(drive.role);
        role->setProperty("role", "muted");
        auto* icon_label = new QLabel;
        icon_label->setPixmap(status_icon(drive.problem ? vaultik::jobs::RunStatus::Failed : vaultik::jobs::RunStatus::Success)
                                  .pixmap(16, 16));
        auto* name = new QLabel(drive.name);
        name->setTextFormat(Qt::PlainText);
        auto* status = new QLabel(drive.status);
        status->setTextFormat(Qt::PlainText);
        status->setWordWrap(true);
        if (drive.problem) status->setStyleSheet(QStringLiteral("color: %1;").arg(t.danger.name()));
        grid->addWidget(role, row, 0);
        grid->addWidget(icon_label, row, 1);
        grid->addWidget(name, row, 2);
        grid->addWidget(status, row, 3);
        ++row;
    }
    grid->setColumnStretch(3, 1);
}
