#pragma once

#include <QDialog>

#include <string>
#include <vector>

#include "vaultik/jobs/computers.hpp"
#include "vaultik/repo/s3_backend.hpp"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

// A backup destination to look at: a repository the user's jobs use, with what opens it.
struct ComputerSource {
    QString title;           // where it is, and which jobs use it
    std::string repository;
    std::string password;    // empty with `error` set: the user gave none
    vaultik::CloudCredentials cloud;
    QString error;
};

// "All computers": for each repository, every computer backing up into it (jobs/computers.hpp),
// with how its backups are doing. Repositories are read on worker threads.
class ComputersDialog : public QDialog {
    Q_OBJECT
public:
    ComputersDialog(std::vector<ComputerSource> sources, QWidget* parent = nullptr);
    ~ComputersDialog() override;

private:
    void load();
    void fill(int index, const std::vector<vaultik::jobs::Computer>& computers, const QString& error);
    void show_details();
    // The computer of the current row, or null; `source` gets its source's index.
    const vaultik::jobs::Computer* current(int* source) const;
    void update_buttons();
    void ask_backup();
    void forget();

    std::vector<ComputerSource> sources_;
    std::vector<std::vector<vaultik::jobs::Computer>> computers_;  // per source
    int pending_ = 0;
    int generation_ = 0;  // counts loads, to drop the results of an older one
    QTreeWidget* tree_ = nullptr;
    QPlainTextEdit* details_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* ask_ = nullptr;
    QPushButton* forget_ = nullptr;
    QLabel* summary_ = nullptr;
};
