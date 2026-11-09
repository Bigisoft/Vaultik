#include "preview.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QTextBlock>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>

#include "common.h"
#include "syntax.h"
#include "theme.h"

using namespace vaultik;

namespace {

constexpr std::uint64_t kTextLimit = 8u << 20;    // larger text files show their beginning
constexpr std::uint64_t kImageLimit = 96u << 20;  // larger pictures are not decoded
constexpr int kImageMaxSide = 4096;               // decoded pictures are scaled down to this
constexpr int kMaxHighlightedMatches = 5000;
constexpr qsizetype kHighlightLimit = 2 * 1024 * 1024;  // no syntax colours beyond this (characters)

enum Page { kMessagePage, kTextPage, kImagePage };

bool is_image_name(const QString& name) {
    static const QStringList kExtensions = {"jpg",  "jpeg", "jpe", "jfif", "png", "gif", "bmp", "dib", "tif",
                                            "tiff", "ico",  "cur", "webp", "heic", "heif", "avif", "svg", "wdp", "jxr"};
    return kExtensions.contains(QFileInfo(name).suffix().toLower());
}

// Text when it has no NUL bytes in its first 8 KiB, unless it starts with a UTF-16 byte order mark.
bool looks_like_text(const QByteArray& data) {
    if (data.startsWith("\xFF\xFE") || data.startsWith("\xFE\xFF")) return true;
    const qsizetype n = std::min<qsizetype>(data.size(), 8192);
    for (qsizetype i = 0; i < n; ++i)
        if (data[i] == '\0') return false;
    return true;
}

QString decode_text(const QByteArray& data) {
    if (data.startsWith("\xFF\xFE")) return QStringDecoder(QStringDecoder::Utf16LE)(data.mid(2));
    if (data.startsWith("\xFE\xFF")) return QStringDecoder(QStringDecoder::Utf16BE)(data.mid(2));
    const QByteArray body = data.startsWith("\xEF\xBB\xBF") ? data.mid(3) : data;
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8(body);
    if (!utf8.hasError()) return text;
    // Not UTF-8: most likely the computer's own code page (older Windows text files).
    return QStringDecoder(QStringDecoder::System)(body);
}

// The first `limit` bytes of a file's content, read from its chunks.
QByteArray read_content(Repository& repo, const Node& node, std::uint64_t limit, const std::function<bool()>& cancelled) {
    QByteArray out;
    out.reserve(static_cast<qsizetype>(std::min(node.size, limit)));
    for (const Id& chunk : node.content) {
        if (cancelled && cancelled()) throw Error("cancelled");
        const Bytes data = repo.load_blob(BlobType::Data, chunk);
        out.append(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
        if (static_cast<std::uint64_t>(out.size()) >= limit) {
            out.truncate(static_cast<qsizetype>(limit));
            break;
        }
    }
    return out;
}

QFont code_font() {
    QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const char* family : {"Cascadia Mono", "Cascadia Code", "Consolas"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            f.setFamily(QString::fromLatin1(family));
            break;
        }
    }
    f.setPointSizeF(10);
    f.setStyleHint(QFont::Monospace);
    return f;
}

class Gutter : public QWidget {
public:
    explicit Gutter(CodeView* view) : QWidget(view), view_(view) {}
    QSize sizeHint() const override { return {view_->gutter_width(), 0}; }

protected:
    void paintEvent(QPaintEvent* event) override { view_->paint_gutter(event); }

private:
    CodeView* view_;
};

}  // namespace

// ---- CodeView ----------------------------------------------------------------------------------

CodeView::CodeView(QWidget* parent) : QPlainTextEdit(parent) {
    gutter_ = new Gutter(this);
    setReadOnly(true);
    setFont(code_font());
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    setTabStopDistance(QFontMetricsF(font()).horizontalAdvance(QLatin1Char(' ')) * 4);
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { setViewportMargins(gutter_width(), 0, 0, 0); });
    connect(this, &QPlainTextEdit::updateRequest, this, &CodeView::update_gutter);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeView::update_current_line);
    setViewportMargins(gutter_width(), 0, 0, 0);
}

int CodeView::gutter_width() const {
    int digits = 1;
    for (int max = std::max(1, blockCount()); max >= 10; max /= 10) ++digits;
    return 16 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * std::max(digits, 3);
}

void CodeView::resizeEvent(QResizeEvent* event) {
    QPlainTextEdit::resizeEvent(event);
    const QRect cr = contentsRect();
    gutter_->setGeometry(QRect(cr.left(), cr.top(), gutter_width(), cr.height()));
}

void CodeView::update_gutter(const QRect& rect, int dy) {
    if (dy != 0) gutter_->scroll(0, dy);
    else gutter_->update(0, rect.y(), gutter_->width(), rect.height());
}

void CodeView::paint_gutter(QPaintEvent* event) {
    const Theme& t = theme();
    QPainter p(gutter_);
    p.fillRect(event->rect(), t.surface);
    p.setFont(font());
    const int current = textCursor().blockNumber();
    QTextBlock block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            p.setPen(block.blockNumber() == current ? t.text : t.text_faint);
            p.drawText(0, top, gutter_->width() - 10, fontMetrics().height(), Qt::AlignRight,
                       QString::number(block.blockNumber() + 1));
        }
        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
    }
}

void CodeView::update_current_line() {
    QList<QTextEdit::ExtraSelection> all = matches_;
    QTextEdit::ExtraSelection line;
    line.format.setBackground(theme().hover);
    line.format.setProperty(QTextFormat::FullWidthSelection, true);
    line.cursor = textCursor();
    line.cursor.clearSelection();
    all.prepend(line);
    setExtraSelections(all);
    gutter_->update();
}

void CodeView::set_matches(const QList<QTextCursor>& all, const QTextCursor& current) {
    const Theme& t = theme();
    matches_.clear();
    for (const QTextCursor& c : all) {
        QTextEdit::ExtraSelection s;
        s.cursor = c;
        s.format.setBackground(t.dark ? QColor(0x61, 0x51, 0x1F) : QColor(0xFF, 0xF1, 0xA8));
        matches_.append(s);
    }
    if (!current.isNull()) {
        QTextEdit::ExtraSelection s;
        s.cursor = current;
        s.format.setBackground(t.accent);
        s.format.setForeground(t.accent_text);
        matches_.append(s);
    }
    update_current_line();
}

// ---- PreviewWidget -----------------------------------------------------------------------------

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent) {
    title_ = new QLabel(this);
    title_->setProperty("role", "heading");
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    info_ = new QLabel(this);
    info_->setProperty("role", "muted");

    find_button_ = new QToolButton(this);
    find_button_->setToolTip(tr("Find in file (Ctrl+F)"));
    find_button_->setAccessibleName(tr("Find in file"));
    wrap_ = new QCheckBox(tr("Wrap lines"), this);
    fit_ = new QToolButton(this);
    fit_->setCheckable(true);
    fit_->setChecked(true);
    fit_->setText(tr("Fit"));
    fit_->setToolTip(tr("Fit the picture in the window, or show it at full size"));

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    auto* names = new QVBoxLayout;
    names->setSpacing(1);
    names->addWidget(title_);
    names->addWidget(info_);
    header->addLayout(names, 1);
    header->addWidget(wrap_);
    header->addWidget(find_button_);
    header->addWidget(fit_);

    pages_ = new QStackedWidget(this);

    // Message page.
    auto* message_page = new QWidget;
    auto* ml = new QVBoxLayout(message_page);
    ml->addStretch();
    message_icon_ = new QLabel;
    message_icon_->setAlignment(Qt::AlignCenter);
    message_ = new QLabel;
    message_->setAlignment(Qt::AlignCenter);
    message_->setWordWrap(true);
    message_->setProperty("role", "muted");
    load_ = new QPushButton(tr("Show preview"));
    load_->setProperty("role", "primary");
    connect(load_, &QPushButton::clicked, this, &PreviewWidget::load);
    auto* load_row = new QHBoxLayout;
    load_row->addStretch();
    load_row->addWidget(load_);
    load_row->addStretch();
    ml->addWidget(message_icon_);
    ml->addWidget(message_);
    ml->addLayout(load_row);
    ml->addStretch();
    pages_->addWidget(message_page);

    // Text page: find bar over the code view.
    auto* text_page = new QWidget;
    auto* tl = new QVBoxLayout(text_page);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(6);
    find_bar_ = new QWidget;
    auto* fl = new QHBoxLayout(find_bar_);
    fl->setContentsMargins(0, 0, 0, 0);
    find_text_ = new QLineEdit;
    find_text_->setPlaceholderText(tr("Find in file"));
    find_text_->setClearButtonEnabled(true);
    find_count_ = new QLabel;
    find_count_->setProperty("role", "muted");
    find_count_->setMinimumWidth(90);
    find_prev_ = new QToolButton;
    find_prev_->setToolTip(tr("Previous match (Shift+Enter)"));
    find_prev_->setAccessibleName(tr("Previous match"));
    find_next_ = new QToolButton;
    find_next_->setToolTip(tr("Next match (Enter)"));
    find_next_->setAccessibleName(tr("Next match"));
    match_case_ = new QCheckBox(tr("Match case"));
    find_close_ = new QToolButton;
    find_close_->setToolTip(tr("Close (Esc)"));
    find_close_->setAccessibleName(tr("Close find"));
    fl->addWidget(find_text_, 1);
    fl->addWidget(find_count_);
    fl->addWidget(find_prev_);
    fl->addWidget(find_next_);
    fl->addWidget(match_case_);
    fl->addWidget(find_close_);
    code_ = new CodeView;
    tl->addWidget(find_bar_);
    tl->addWidget(code_, 1);
    pages_->addWidget(text_page);

    // Image page.
    scroll_ = new QScrollArea;
    scroll_->setAlignment(Qt::AlignCenter);
    scroll_->setWidgetResizable(false);
    picture_ = new QLabel;
    picture_->setAlignment(Qt::AlignCenter);
    scroll_->setWidget(picture_);
    scroll_->viewport()->installEventFilter(this);
    pages_->addWidget(scroll_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(header);
    layout->addWidget(pages_, 1);

    connect(find_button_, &QToolButton::clicked, this, [this] { set_find_visible(!find_bar_->isVisible()); });
    connect(find_close_, &QToolButton::clicked, this, [this] { set_find_visible(false); });
    connect(find_text_, &QLineEdit::textChanged, this, &PreviewWidget::update_matches);
    connect(match_case_, &QCheckBox::toggled, this, &PreviewWidget::update_matches);
    connect(find_next_, &QToolButton::clicked, this, [this] { find(false); });
    connect(find_prev_, &QToolButton::clicked, this, [this] { find(true); });
    connect(find_text_, &QLineEdit::returnPressed, this, [this] {
        find(QGuiApplication::keyboardModifiers() & Qt::ShiftModifier);
    });
    connect(wrap_, &QCheckBox::toggled, this, [this](bool on) {
        code_->setLineWrapMode(on ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    });
    connect(fit_, &QToolButton::toggled, this, &PreviewWidget::update_zoom);

    auto* find_shortcut = new QShortcut(QKeySequence::Find, this);  // owned by this widget
    find_shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find_shortcut, &QShortcut::activated, this, [this] {
        if (pages_->currentIndex() == kTextPage) set_find_visible(true);
    });
    auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), find_bar_);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { set_find_visible(false); });

    refresh_icons();
    clear();
}

void PreviewWidget::refresh_icons() {
    find_button_->setIcon(icon(QStringLiteral("search_20_regular")));
    find_prev_->setIcon(icon(QStringLiteral("chevron_up_20_regular")));
    find_next_->setIcon(icon(QStringLiteral("chevron_down_20_regular")));
    find_close_->setIcon(icon(QStringLiteral("dismiss_20_regular")));
    load_->setIcon(icon(QStringLiteral("document_20_regular"), theme().accent_text));
    message_icon_->setPixmap(icon(QStringLiteral("document_20_regular"), theme().text_faint).pixmap(48, 48));
}

bool PreviewWidget::eventFilter(QObject* watched, QEvent* event) {
    // A fitted picture follows the size of the window.
    if (watched == scroll_->viewport() && event->type() == QEvent::Resize && fit_->isChecked()) update_zoom();
    return QWidget::eventFilter(watched, event);
}

void PreviewWidget::clear() {
    repo_.reset();
    node_ = Node{};
    name_.clear();
    image_ = QImage();
    picture_->clear();
    delete highlighter_;  // also detaches it from the document
    highlighter_ = nullptr;
    code_->clear();
    code_->set_matches({}, {});
    show_message(tr("Preview"), tr("Pick a file to see what it holds."));
}

void PreviewWidget::show_message(const QString& title, const QString& text) {
    title_->setText(title);
    info_->clear();
    message_->setText(text);
    message_icon_->setPixmap(icon(QStringLiteral("document_20_regular"), theme().text_faint).pixmap(48, 48));
    load_->hide();
    wrap_->hide();
    find_button_->hide();
    fit_->hide();
    pages_->setCurrentIndex(kMessagePage);
}

void PreviewWidget::show_file(std::shared_ptr<Repository> repo, const Node& node, const QString& name, bool load_now) {
    clear();
    repo_ = std::move(repo);
    node_ = node;
    name_ = name;
    if (node.kind != NodeKind::File) {
        show_message(name, node.kind == NodeKind::Dir ? tr("A folder. Pick a file inside it to preview it.")
                                                       : tr("A link: it is stored without content."));
        return;
    }
    show_message(name, tr("%1. Showing it downloads and decrypts it into memory only.").arg(format_bytes(node.size)));
    title_->setText(name);
    info_->setText(format_bytes(node.size));
    if (node.size == 0) {
        show_message(name, tr("An empty file."));
        return;
    }
    if (load_now) {
        load();
    } else {
        load_->show();
    }
}

void PreviewWidget::load() {
    if (!repo_) return;
    const bool image = is_image_name(name_);
    if (image && node_.size > kImageLimit) {
        show_message(name_, tr("This picture is too large to preview (%1).").arg(format_bytes(node_.size)));
        return;
    }
    const std::uint64_t limit = image ? kImageLimit : kTextLimit;
    QByteArray bytes;
    QImage picture;
    QString format;
    const std::shared_ptr<Repository> repo = repo_;
    const Node node = node_;
    const QString error = run_busy(this, tr("Opening %1...").arg(name_), [&](const std::function<bool()>& cancelled) {
        // Other files: the beginning is enough to tell text from binary; only text is read further.
        bytes = read_content(*repo, node, image ? limit : 65536, cancelled);
        if (!image && looks_like_text(bytes) && node.size > static_cast<std::uint64_t>(bytes.size()))
            bytes = read_content(*repo, node, limit, cancelled);
        if (image || !looks_like_text(bytes)) {
            picture = decode_image(bytes, kImageMaxSide, &format);
            if (!picture.isNull()) bytes.clear();  // the decoded picture is all that is kept
        }
    });
    if (!error.isEmpty()) {
        show_message(name_, error);
        title_->setText(name_);
        return;
    }
    if (!picture.isNull()) {
        show_image(picture, format);
    } else if (looks_like_text(bytes)) {
        show_text(bytes, node.size > static_cast<std::uint64_t>(bytes.size()));
    } else {
        show_message(name_, tr("No preview for this kind of file. Restore it to open it with its program."));
        info_->setText(format_bytes(node.size));
    }
}

void PreviewWidget::show_text(const QByteArray& bytes, bool truncated) {
    const QString text = decode_text(bytes);
    delete highlighter_;
    highlighter_ = nullptr;
    code_->setPlainText(text);
    QString language;
    if (text.size() <= kHighlightLimit) {
        highlighter_ = SyntaxHighlighter::for_file(name_, code_->document());
        if (highlighter_) language = highlighter_->language();
    }
    const QLocale locale;
    QStringList parts;
    parts << (language.isEmpty() ? tr("Text") : language);
    parts << count_of(static_cast<std::uint64_t>(code_->blockCount()), tr("line"), tr("lines"));
    parts << format_bytes(node_.size);
    if (truncated) parts << tr("showing the first %1").arg(format_bytes(static_cast<std::uint64_t>(bytes.size())));
    title_->setText(name_);
    info_->setText(parts.join(QStringLiteral("  ·  ")));
    wrap_->show();
    find_button_->show();
    fit_->hide();
    set_find_visible(false);
    pages_->setCurrentIndex(kTextPage);
    code_->moveCursor(QTextCursor::Start);
}

void PreviewWidget::show_image(const QImage& image, const QString& format) {
    image_ = image;
    title_->setText(name_);
    info_->setText(QStringLiteral("%1  ·  %2 × %3  ·  %4")
                       .arg(format.isEmpty() ? tr("Picture") : format)
                       .arg(image.width())
                       .arg(image.height())
                       .arg(format_bytes(node_.size)));
    wrap_->hide();
    find_button_->hide();
    fit_->show();
    pages_->setCurrentIndex(kImagePage);
    update_zoom();
}

void PreviewWidget::update_zoom() {
    if (image_.isNull()) return;
    fit_->setText(fit_->isChecked() ? tr("Fit") : tr("100%"));
    const qreal dpr = devicePixelRatioF();
    QPixmap pm = QPixmap::fromImage(image_);
    if (fit_->isChecked()) {
        const QSize area = scroll_->viewport()->size() * dpr - QSize(8, 8);
        if (pm.width() > area.width() || pm.height() > area.height())
            pm = pm.scaled(area, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    pm.setDevicePixelRatio(dpr);
    picture_->setPixmap(pm);
    picture_->resize(pm.deviceIndependentSize().toSize());
}

void PreviewWidget::set_find_visible(bool visible) {
    find_bar_->setVisible(visible);
    if (visible) {
        find_text_->setFocus();
        find_text_->selectAll();
        update_matches();
    } else {
        code_->set_matches({}, {});
        code_->setFocus();
    }
}

void PreviewWidget::update_matches() {
    const QString needle = find_text_->text();
    QList<QTextCursor> all;
    int total = 0;
    if (!needle.isEmpty()) {
        const QTextDocument::FindFlags flags = match_case_->isChecked() ? QTextDocument::FindCaseSensitively
                                                                         : QTextDocument::FindFlags();
        QTextCursor c(code_->document());
        while (true) {
            c = code_->document()->find(needle, c, flags);
            if (c.isNull()) break;
            ++total;
            if (all.size() < kMaxHighlightedMatches) all.append(c);
        }
    }
    QTextCursor current;
    if (!all.isEmpty()) {
        // The first match at or after the cursor becomes the current one.
        const int from = code_->textCursor().selectionStart();
        current = all.front();
        for (const QTextCursor& c : all) {
            if (c.selectionStart() >= from) {
                current = c;
                break;
            }
        }
        code_->setTextCursor(current);
    }
    code_->set_matches(all, current);
    find_count_->setText(needle.isEmpty() ? QString()
                         : total == 0     ? tr("No matches")
                                          : count_of(static_cast<std::uint64_t>(total), tr("match"), tr("matches")));
}

void PreviewWidget::find(bool backwards) {
    const QString needle = find_text_->text();
    if (needle.isEmpty()) return;
    QTextDocument::FindFlags flags = match_case_->isChecked() ? QTextDocument::FindCaseSensitively : QTextDocument::FindFlags();
    if (backwards) flags |= QTextDocument::FindBackward;
    QTextCursor from = code_->textCursor();
    if (backwards) from.setPosition(from.selectionStart());
    QTextCursor found = code_->document()->find(needle, from, flags);
    if (found.isNull()) {  // wrap around
        QTextCursor edge(code_->document());
        edge.movePosition(backwards ? QTextCursor::End : QTextCursor::Start);
        found = code_->document()->find(needle, edge, flags);
    }
    if (found.isNull()) return;
    code_->setTextCursor(found);
    code_->centerCursor();
    update_matches();
}

// ---- PreviewDialog -----------------------------------------------------------------------------

PreviewDialog::PreviewDialog(std::shared_ptr<Repository> repo, const Node& node, const QString& name,
                             const QString& subtitle, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(subtitle.isEmpty() ? name : name + "  ·  " + subtitle);
    resize(960, 720);
    auto* preview = new PreviewWidget(this);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 14, 16, 12);
    layout->addWidget(preview, 1);
    layout->addWidget(buttons);
    preview->show_file(std::move(repo), node, name, true);
}
