#pragma once

#include <QDialog>
#include <QImage>
#include <QPlainTextEdit>
#include <QWidget>

#include <functional>
#include <memory>

#include "vaultik/model/tree.hpp"
#include "vaultik/repo/repository.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QToolButton;
class SyntaxHighlighter;

// Text from a file's bytes: UTF-8 or UTF-16 with a byte order mark, UTF-8, else the system code page.
QString decode_text(const QByteArray& data);
// The first `limit` bytes of a file in a backup version, decrypted in memory.
QByteArray read_content(vaultik::Repository& repo, const vaultik::Node& node, std::uint64_t limit,
                        const std::function<bool()>& cancelled);
// The fixed-width font for code (Cascadia or Consolas when installed).
QFont code_font();

// A read-only text view with line numbers, used for previews.
class CodeView : public QPlainTextEdit {
    Q_OBJECT

public:
    explicit CodeView(QWidget* parent = nullptr);

    int gutter_width() const;
    void paint_gutter(QPaintEvent* event);
    // Highlights every match of the find bar, the current one stronger.
    void set_matches(const QList<QTextCursor>& all, const QTextCursor& current);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void update_gutter(const QRect& rect, int dy);
    void update_current_line();

    QWidget* gutter_ = nullptr;
    QList<QTextEdit::ExtraSelection> matches_;
};

// Shows a file from a backup version without restoring it: pictures, and text with line numbers,
// syntax colours for common languages and a find bar. The file is decrypted into memory only.
class PreviewWidget : public QWidget {
    Q_OBJECT

public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    // Shows `node` (a file) of the open repository. With `load_now` false (large files, cloud
    // storage) it offers a button instead of downloading at once.
    void show_file(std::shared_ptr<vaultik::Repository> repo, const vaultik::Node& node, const QString& name,
                   bool load_now);
    // A message instead of a file ("Pick a file to preview it").
    void show_message(const QString& title, const QString& text = {});
    void clear();
    void refresh_icons();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void load();
    void show_text(const QByteArray& bytes, bool truncated);
    void show_image(const QImage& image, const QString& format);
    void update_zoom();
    void find(bool backwards);
    void update_matches();
    void set_find_visible(bool visible);

    std::shared_ptr<vaultik::Repository> repo_;
    vaultik::Node node_;
    QString name_;

    QLabel* title_ = nullptr;
    QLabel* info_ = nullptr;
    QStackedWidget* pages_ = nullptr;
    // Message page
    QLabel* message_icon_ = nullptr;
    QLabel* message_ = nullptr;
    QPushButton* load_ = nullptr;
    // Text page
    QWidget* find_bar_ = nullptr;
    QLineEdit* find_text_ = nullptr;
    QLabel* find_count_ = nullptr;
    QToolButton* find_prev_ = nullptr;
    QToolButton* find_next_ = nullptr;
    QToolButton* find_close_ = nullptr;
    QCheckBox* match_case_ = nullptr;
    CodeView* code_ = nullptr;
    SyntaxHighlighter* highlighter_ = nullptr;  // owned by the code view's document
    QToolButton* find_button_ = nullptr;
    QCheckBox* wrap_ = nullptr;
    // Image page
    QScrollArea* scroll_ = nullptr;
    QLabel* picture_ = nullptr;
    QToolButton* fit_ = nullptr;
    QImage image_;
};

// A window around PreviewWidget, for previews outside the restore browser.
class PreviewDialog : public QDialog {
    Q_OBJECT

public:
    PreviewDialog(std::shared_ptr<vaultik::Repository> repo, const vaultik::Node& node, const QString& name,
                  const QString& subtitle, QWidget* parent = nullptr);
};

// Decodes a picture: Qt's own formats first, then (on Windows) the system's image codecs, which
// include JPEG, GIF, TIFF and ICO, and HEIC and WebP where Windows has them. The camera's
// orientation is applied, and pictures larger than `max_side` are scaled down while decoding.
// `format` receives a short name ("JPEG"). Null on failure.
QImage decode_image(const QByteArray& data, int max_side, QString* format);
