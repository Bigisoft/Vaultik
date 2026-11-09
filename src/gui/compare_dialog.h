#pragma once

#include <QDialog>
#include <QPlainTextEdit>

#include <memory>
#include <vector>

#include "repo_handle.h"
#include "vaultik/ops/diff.hpp"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// One side of a side-by-side text comparison: the lines with their own line numbers in a gutter
// (blank where the other side has lines this one has not), and coloured rows for what changed.
class DiffPane : public QPlainTextEdit {
    Q_OBJECT

public:
    enum class Row { Same, Removed, Added, Filler };

    explicit DiffPane(QWidget* parent = nullptr);
    // `numbers` holds each row's line number (0: no line, a filler row).
    void set_rows(const QStringList& text, const std::vector<int>& numbers, const std::vector<Row>& kinds);
    int gutter_width() const;
    void paint_gutter(QPaintEvent* event);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void place_gutter();

    QWidget* gutter_ = nullptr;
    std::vector<int> numbers_;
};

// Two versions of a text file side by side, the removed lines on the left and the added ones on
// the right, with Next and Previous to jump between the changes.
class TextDiffDialog : public QDialog {
    Q_OBJECT

public:
    TextDiffDialog(const QString& title, const QString& left_title, const QString& right_title, const QString& old_text,
                   const QString& new_text, QWidget* parent = nullptr);

private:
    void jump(bool forward);

    DiffPane* left_ = nullptr;
    DiffPane* right_ = nullptr;
    std::vector<int> change_rows_;  // the first row of each block of changes
};

// What changed between two versions of a job: added, removed and changed files and folders, with a
// filter, and the differences of a changed text file side by side.
class CompareDialog : public QDialog {
    Q_OBJECT

public:
    CompareDialog(std::shared_ptr<vaultik::Repository> repo, const SnapshotEntry& older, const SnapshotEntry& newer,
                  vaultik::DiffResult diff, QWidget* parent = nullptr);

private:
    void fill();
    void show_differences();

    std::shared_ptr<vaultik::Repository> repo_;
    SnapshotEntry older_, newer_;
    vaultik::DiffResult diff_;
    QLineEdit* filter_ = nullptr;
    QComboBox* kind_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QPushButton* differences_ = nullptr;
};
