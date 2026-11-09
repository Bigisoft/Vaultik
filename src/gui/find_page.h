#pragma once

#include <QWidget>

#include <memory>
#include <vector>

#include "repo_handle.h"
#include "vaultik/ops/history.hpp"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

// The versions of one file or folder: only those where it changed (or all), each compared with what
// is on this computer now, with "Restore this version".
class ItemHistoryView : public QWidget {
    Q_OBJECT

public:
    explicit ItemHistoryView(QWidget* parent = nullptr);

    void set_source(std::shared_ptr<vaultik::Repository> repo, std::vector<SnapshotEntry> versions);
    // Loads the versions of `path` (a snapshot path such as "C/Users/me/report.docx").
    void show_item(const QString& path);
    void clear();
    void refresh_icons();

signals:
    void restore_requested(int snapshot_index, const QString& path);

private:
    void fill();
    int selected_version() const;

    std::shared_ptr<vaultik::Repository> repo_;
    std::vector<SnapshotEntry> versions_;
    QString path_;
    std::vector<vaultik::ItemVersion> history_;

    QLabel* title_ = nullptr;
    QLabel* location_ = nullptr;
    QCheckBox* all_ = nullptr;
    QTreeWidget* tree_ = nullptr;
    QPushButton* preview_ = nullptr;
    QPushButton* restore_ = nullptr;
};

// "Find a file": a name search through every version, and the chosen item's history below.
class FindPage : public QWidget {
    Q_OBJECT

public:
    explicit FindPage(QWidget* parent = nullptr);

    // The backup to search; a null repository means it is not open (yet).
    void set_source(std::shared_ptr<vaultik::Repository> repo, std::vector<SnapshotEntry> versions);
    void refresh_icons();

signals:
    void restore_requested(int snapshot_index, const QString& path);

private:
    void search();

    std::shared_ptr<vaultik::Repository> repo_;
    std::vector<SnapshotEntry> versions_;
    QLineEdit* query_ = nullptr;
    QPushButton* search_ = nullptr;
    QLabel* status_ = nullptr;
    QTreeWidget* results_ = nullptr;
    ItemHistoryView* history_ = nullptr;
};
