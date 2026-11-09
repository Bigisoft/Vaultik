#include "compare_dialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBlock>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "common.h"
#include "preview.h"
#include "theme.h"
#include "vaultik/util/line_diff.hpp"

using namespace vaultik;

namespace {

class DiffGutter : public QWidget {
public:
    explicit DiffGutter(DiffPane* pane) : QWidget(pane), pane_(pane) {}
    QSize sizeHint() const override { return {pane_->gutter_width(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { pane_->paint_gutter(event); }

private:
    DiffPane* pane_;
};

constexpr int kIndexRole = Qt::UserRole + 1;
constexpr qint64 kTextLimit = 8 << 20;  // larger files are not compared line by line

QStringList lines_of(const QString& text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString& l : lines)
        if (l.endsWith(QLatin1Char('\r'))) l.chop(1);
    if (!lines.isEmpty() && lines.back().isEmpty()) lines.removeLast();  // the final newline
    return lines;
}

bool looks_binary(const QByteArray& data) {
    return data.left(8192).contains('\0') && !data.startsWith("\xFF\xFE") && !data.startsWith("\xFE\xFF");
}

}  // namespace

// ---- DiffPane ----------------------------------------------------------------------------------

DiffPane::DiffPane(QWidget* parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(code_font());
    setFrameShape(QFrame::NoFrame);
    gutter_ = new DiffGutter(this);
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& rect, int dy) {
        if (dy != 0) gutter_->scroll(0, dy);
        else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
    });
    setViewportMargins(gutter_width(), 0, 0, 0);
}

int DiffPane::gutter_width() const {
    int digits = 3;
    for (int n = static_cast<int>(numbers_.size()); n >= 1000; n /= 10) ++digits;
    return 14 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void DiffPane::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    place_gutter();
}

void DiffPane::place_gutter() {
    const QRect r = contentsRect();
    gutter_->setGeometry(QRect(r.left(), r.top(), gutter_width(), r.height()));
}

void DiffPane::paint_gutter(QPaintEvent* event) {
    const Theme& t = theme();
    QPainter p(gutter_);
    p.fillRect(event->rect(), t.surface);
    p.setPen(t.text_faint);
    p.setFont(font());
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    const int h = fontMetrics().height();
    while (block.isValid() && top <= event->rect().bottom()) {
        const int row = block.blockNumber();
        if (row < static_cast<int>(numbers_.size()) && numbers_[static_cast<std::size_t>(row)] > 0)
            p.drawText(0, top, gutter_->width() - 8, h, Qt::AlignRight, QString::number(numbers_[static_cast<std::size_t>(row)]));
        block = block.next();
        top += qRound(blockBoundingRect(block).height());
    }
}

void DiffPane::set_rows(const QStringList& text, const std::vector<int>& numbers, const std::vector<Row>& kinds) {
    numbers_ = numbers;
    setPlainText(text.join(QLatin1Char('\n')));
    setViewportMargins(gutter_width(), 0, 0, 0);
    place_gutter();

    const Theme& t = theme();
    QColor removed = t.danger, added = t.success, filler = t.hover;
    removed.setAlpha(t.dark ? 70 : 45);
    added.setAlpha(t.dark ? 70 : 45);
    QList<QTextEdit::ExtraSelection> marks;
    QTextBlock block = document()->firstBlock();
    for (std::size_t i = 0; i < kinds.size() && block.isValid(); ++i, block = block.next()) {
        if (kinds[i] == Row::Same) continue;
        QTextEdit::ExtraSelection s;
        s.cursor = QTextCursor(block);
        s.format.setProperty(QTextFormat::FullWidthSelection, true);
        s.format.setBackground(kinds[i] == Row::Removed ? removed : kinds[i] == Row::Added ? added : filler);
        marks.push_back(s);
    }
    setExtraSelections(marks);
}

// ---- TextDiffDialog --------------------------------------------------------------------------------

TextDiffDialog::TextDiffDialog(const QString& title, const QString& left_title, const QString& right_title,
                               const QString& old_text, const QString& new_text, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(title);
    resize(1200, 760);
    const QStringList a = lines_of(old_text);
    const QStringList b = lines_of(new_text);

    // Rows for both sides: equal lines side by side; a block of removed and added lines paired up,
    // the shorter side filled with blank rows.
    QStringList left_text, right_text;
    std::vector<int> left_num, right_num;
    std::vector<DiffPane::Row> left_kind, right_kind;
    int removed = 0, added = 0;
    const auto push = [&](const QString* l, int ln, const QString* r, int rn, DiffPane::Row lk, DiffPane::Row rk) {
        left_text << (l ? *l : QString());
        right_text << (r ? *r : QString());
        left_num.push_back(ln);
        right_num.push_back(rn);
        left_kind.push_back(lk);
        right_kind.push_back(rk);
    };
    std::vector<int> dels, ins;
    const auto flush = [&] {
        if (dels.empty() && ins.empty()) return;
        change_rows_.push_back(static_cast<int>(left_text.size()));
        const std::size_t rows = std::max(dels.size(), ins.size());
        for (std::size_t i = 0; i < rows; ++i) {
            const bool hl = i < dels.size(), hr = i < ins.size();
            push(hl ? &a[dels[i]] : nullptr, hl ? dels[i] + 1 : 0, hr ? &b[ins[i]] : nullptr, hr ? ins[i] + 1 : 0,
                 hl ? DiffPane::Row::Removed : DiffPane::Row::Filler, hr ? DiffPane::Row::Added : DiffPane::Row::Filler);
        }
        removed += static_cast<int>(dels.size());
        added += static_cast<int>(ins.size());
        dels.clear();
        ins.clear();
    };
    const auto edits = line_diff(a, b);
    QString note;
    if (edits) {
        for (const DiffEdit& e : *edits) {
            if (e.op == DiffOp::Delete) {
                dels.push_back(static_cast<int>(e.old_line));
            } else if (e.op == DiffOp::Insert) {
                ins.push_back(static_cast<int>(e.new_line));
            } else {
                flush();
                const int i = static_cast<int>(e.old_line), j = static_cast<int>(e.new_line);
                push(&a[i], i + 1, &b[j], j + 1, DiffPane::Row::Same, DiffPane::Row::Same);
            }
        }
    } else {
        note = tr("These versions differ too much to line up; each is shown whole.");
        for (int i = 0; i < a.size(); ++i) dels.push_back(i);
        for (int j = 0; j < b.size(); ++j) ins.push_back(j);
    }
    flush();

    left_ = new DiffPane;
    right_ = new DiffPane;
    left_->set_rows(left_text, left_num, left_kind);
    right_->set_rows(right_text, right_num, right_kind);
    // Scroll together.
    for (auto [from, to] : {std::pair{left_, right_}, std::pair{right_, left_}}) {
        connect(from->verticalScrollBar(), &QScrollBar::valueChanged, to->verticalScrollBar(), &QScrollBar::setValue);
        connect(from->horizontalScrollBar(), &QScrollBar::valueChanged, to->horizontalScrollBar(), &QScrollBar::setValue);
    }

    const auto titled = [](const QString& text, QWidget* body) {
        auto* box = new QFrame;
        box->setProperty("role", "card");
        auto* l = new QVBoxLayout(box);
        l->setContentsMargins(1, 8, 1, 1);
        auto* label = new QLabel(text);
        label->setProperty("role", "section");
        label->setContentsMargins(10, 0, 0, 4);
        l->addWidget(label);
        l->addWidget(body, 1);
        return box;
    };
    auto* panes = new QHBoxLayout;
    panes->setSpacing(8);
    panes->addWidget(titled(left_title, left_), 1);
    panes->addWidget(titled(right_title, right_), 1);

    auto* summary = new QLabel(removed == 0 && added == 0 ? tr("The text is the same (only line endings or the "
                                                               "encoding differ).")
                                                          : tr("%1 removed, %2 added").arg(count_of(removed, tr("line"), tr("lines")),
                                                                                           count_of(added, tr("line"), tr("lines"))) +
                                                                (note.isEmpty() ? QString() : "  " + note));
    summary->setProperty("role", "muted");
    auto* prev = new QPushButton(icon(QStringLiteral("chevron_up_20_regular")), tr("Previous change"));
    auto* next = new QPushButton(icon(QStringLiteral("chevron_down_20_regular")), tr("Next change"));
    prev->setEnabled(!change_rows_.empty());
    next->setEnabled(!change_rows_.empty());
    connect(prev, &QPushButton::clicked, this, [this] { jump(false); });
    connect(next, &QPushButton::clicked, this, [this] { jump(true); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(summary, 1);
    bottom->addWidget(prev);
    bottom->addWidget(next);
    bottom->addWidget(buttons);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(panes, 1);
    layout->addLayout(bottom);
    if (!change_rows_.empty()) QMetaObject::invokeMethod(this, [this] { jump(true); }, Qt::QueuedConnection);
}

void TextDiffDialog::jump(bool forward) {
    if (change_rows_.empty()) return;
    const int current = left_->textCursor().blockNumber();
    int target = forward ? change_rows_.front() : change_rows_.back();
    if (forward) {
        for (int r : change_rows_)
            if (r > current) {
                target = r;
                break;
            }
    } else {
        for (auto it = change_rows_.rbegin(); it != change_rows_.rend(); ++it)
            if (*it < current) {
                target = *it;
                break;
            }
    }
    for (DiffPane* pane : {left_, right_}) {
        QTextCursor c(pane->document()->findBlockByNumber(target));
        pane->setTextCursor(c);
        pane->centerCursor();
    }
}

// ---- CompareDialog --------------------------------------------------------------------------------

CompareDialog::CompareDialog(std::shared_ptr<Repository> repo, const SnapshotEntry& older, const SnapshotEntry& newer,
                             DiffResult diff, QWidget* parent)
    : QDialog(parent), repo_(std::move(repo)), older_(older), newer_(newer), diff_(std::move(diff)) {
    setWindowTitle(tr("Compare versions"));
    resize(1000, 640);

    auto* heading = new QLabel(tr("From %1 to %2").arg(format_time(older_.snapshot.time), format_time(newer_.snapshot.time)));
    heading->setProperty("role", "heading");
    auto* counts = new QLabel(tr("%1 added, %2 removed, %3 changed")
                                  .arg(QLocale().toString(static_cast<qulonglong>(diff_.added)),
                                       QLocale().toString(static_cast<qulonglong>(diff_.removed)),
                                       QLocale().toString(static_cast<qulonglong>(diff_.changed))) +
                              (diff_.truncated ? "  " + tr("(only the first %1 are listed)")
                                                            .arg(QLocale().toString(static_cast<qulonglong>(diff_.changes.size())))
                                               : QString()) +
                              "  ·  " + tr("A folder that was added or removed is one line."));
    counts->setProperty("role", "muted");

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter by name or folder"));
    filter_->setClearButtonEnabled(true);
    kind_ = new QComboBox;
    kind_->addItems({tr("All changes"), tr("Added"), tr("Removed"), tr("Changed")});
    auto* filters = new QHBoxLayout;
    filters->addWidget(filter_, 1);
    filters->addWidget(kind_);

    list_ = new QTreeWidget;
    list_->setRootIsDecorated(false);
    list_->setIndentation(0);
    list_->setUniformRowHeights(true);
    list_->setSortingEnabled(true);
    list_->setHeaderLabels({tr("Change"), tr("Name"), tr("Folder"), tr("Before"), tr("After")});
    list_->header()->setStretchLastSection(false);
    list_->header()->setSectionResizeMode(2, QHeaderView::Stretch);

    differences_ = new QPushButton(tr("Show differences"));
    differences_->setToolTip(tr("The changed lines of a text file side by side"));
    differences_->setEnabled(false);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->addButton(differences_, QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(heading);
    layout->addWidget(counts);
    layout->addSpacing(4);
    layout->addLayout(filters);
    layout->addWidget(list_, 1);
    layout->addWidget(buttons);

    connect(filter_, &QLineEdit::textChanged, this, &CompareDialog::fill);
    connect(kind_, &QComboBox::currentIndexChanged, this, &CompareDialog::fill);
    connect(list_, &QTreeWidget::itemSelectionChanged, this, [this] {
        const QTreeWidgetItem* item = list_->selectedItems().value(0, nullptr);
        bool text_change = false;
        if (item != nullptr) {
            const Change& c = diff_.changes[item->data(0, kIndexRole).toULongLong()];
            text_change = c.kind == Change::Kind::Changed && c.node_kind == NodeKind::File;
        }
        differences_->setEnabled(text_change);
    });
    connect(list_, &QTreeWidget::itemDoubleClicked, this, [this] {
        if (differences_->isEnabled()) show_differences();
    });
    connect(differences_, &QPushButton::clicked, this, &CompareDialog::show_differences);
    fill();
}

void CompareDialog::fill() {
    const QString needle = filter_->text().trimmed();
    const int kind = kind_->currentIndex();  // 0 all, 1 added, 2 removed, 3 changed
    const Theme& t = theme();
    list_->setSortingEnabled(false);
    list_->clear();
    std::vector<QTreeWidgetItem*> items;
    for (std::size_t i = 0; i < diff_.changes.size(); ++i) {
        const Change& c = diff_.changes[i];
        if ((kind == 1 && c.kind != Change::Kind::Added) || (kind == 2 && c.kind != Change::Kind::Removed) ||
            (kind == 3 && c.kind != Change::Kind::Changed))
            continue;
        const QString path = qs(c.path);
        if (!needle.isEmpty() && !path.contains(needle, Qt::CaseInsensitive)) continue;
        const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
        const QString name = path.mid(slash + 1);
        const QString folder = slash > 0 ? original_path(path.left(slash)) : QString();
        auto* item = new QTreeWidgetItem;
        const bool dir = c.node_kind == NodeKind::Dir;
        switch (c.kind) {
            case Change::Kind::Added:
                item->setText(0, dir ? tr("Folder added") : tr("Added"));
                item->setIcon(0, icon(QStringLiteral("add_20_regular"), t.success));
                break;
            case Change::Kind::Removed:
                item->setText(0, dir ? tr("Folder removed") : tr("Removed"));
                item->setIcon(0, icon(QStringLiteral("delete_20_regular"), t.danger));
                break;
            case Change::Kind::Changed:
                item->setText(0, tr("Changed"));
                item->setIcon(0, icon(QStringLiteral("edit_20_regular"), t.accent));
                break;
        }
        item->setText(1, name);
        item->setIcon(1, icon(dir ? QStringLiteral("folder_20_regular") : QStringLiteral("document_20_regular")));
        item->setText(2, folder.isEmpty() ? path.left(std::max<qsizetype>(slash, 0)) : folder);
        if (!dir && c.kind != Change::Kind::Added) item->setText(3, format_bytes(c.old_size));
        if (!dir && c.kind != Change::Kind::Removed) item->setText(4, format_bytes(c.new_size));
        item->setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(4, Qt::AlignRight | Qt::AlignVCenter);
        item->setData(0, kIndexRole, static_cast<qulonglong>(i));
        items.push_back(item);
    }
    for (QTreeWidgetItem* item : items) list_->addTopLevelItem(item);
    list_->setSortingEnabled(true);
    for (int c : {0, 1, 3, 4}) list_->resizeColumnToContents(c);
}

void CompareDialog::show_differences() {
    const QTreeWidgetItem* item = list_->selectedItems().value(0, nullptr);
    if (item == nullptr) return;
    const Change& c = diff_.changes[item->data(0, kIndexRole).toULongLong()];
    if (c.old_size > kTextLimit || c.new_size > kTextLimit) {
        QMessageBox::information(this, windowTitle(), tr("This file is too large to compare line by line."));
        return;
    }
    QByteArray before, after;
    const QString error = run_busy(this, tr("Reading both versions..."), [&](const std::function<bool()>& cancelled) {
        before = read_content(*repo_, c.old_node, kTextLimit, cancelled);
        after = read_content(*repo_, c.new_node, kTextLimit, cancelled);
    });
    if (!error.isEmpty()) {
        plain_warning(this, windowTitle(), error);
        return;
    }
    if (looks_binary(before) || looks_binary(after)) {
        QMessageBox::information(this, windowTitle(),
                                 tr("This is not a text file, so it cannot be compared line by line. Restore both "
                                    "versions to compare them in a program that opens such files."));
        return;
    }
    const QString name = qs(c.path).section(QLatin1Char('/'), -1);
    TextDiffDialog dialog(tr("%1: differences").arg(name), format_time(older_.snapshot.time),
                          format_time(newer_.snapshot.time), decode_text(before), decode_text(after), this);
    dialog.exec();
}
