#pragma once

#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>

#include <vector>

// Colours source code in the preview. A small rule-based highlighter, not a full parser: keywords,
// types, strings, numbers, comments (also across lines), preprocessor lines and, for markup, tags
// and attributes. The language comes from the file name; unknown files are shown plain.
class SyntaxHighlighter : public QSyntaxHighlighter {
public:
    // Returns null when the file name does not name a known language.
    static SyntaxHighlighter* for_file(const QString& file_name, QTextDocument* document);

    // The language's display name ("C++", "Python").
    QString language() const { return language_; }

protected:
    void highlightBlock(const QString& text) override;

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat format;
        int group = 0;  // which capture group is coloured
    };
    struct Block {
        QRegularExpression start;
        QRegularExpression end;
        QTextCharFormat format;
    };

    explicit SyntaxHighlighter(QTextDocument* document);

    QString language_;
    std::vector<Rule> rules_;
    std::vector<Block> blocks_;  // multi-line comments and strings; block state = index + 1
};
