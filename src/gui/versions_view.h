#pragma once

#include <QWidget>

#include <set>
#include <vector>

#include "repo_handle.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// A job's backup versions as a timeline: grouped by day, week and month, each with what changed,
// its size, its pin and note, and whether the next cleanup will remove it. Actions are signalled;
// the main window carries them out.
class VersionsView : public QWidget {
    Q_OBJECT

public:
    explicit VersionsView(QWidget* parent = nullptr);

    // `leaving`: the versions the job's cleanup rules will remove next time. `rules`: a sentence
    // describing those rules, shown under the list.
    void set_versions(const std::vector<SnapshotEntry>& newest_first, const std::set<vaultik::Id>& leaving,
                      const QString& rules);
    // A message instead of the list ("Opening the backup...", an error).
    void set_message(const QString& text);
    void refresh_icons();

signals:
    void restore_requested(int index);
    void pin_requested(int index, bool pin);
    void note_requested(int index);
    void delete_requested(int index);

private:
    int selected_index() const;
    void update_buttons();
    void context_menu(const QPoint& pos);

    QTreeWidget* tree_ = nullptr;
    QLabel* message_ = nullptr;
    QLabel* footer_ = nullptr;
    QPushButton* restore_ = nullptr;
    QPushButton* pin_ = nullptr;
    QPushButton* note_ = nullptr;
    QPushButton* delete_ = nullptr;
    std::vector<SnapshotEntry> versions_;
    std::set<vaultik::Id> leaving_;
};
