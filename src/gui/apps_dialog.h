#pragma once

#include <QDialog>
#include <QHash>
#include <QStringList>

#include "vaultik/repo/s3_backend.hpp"

class QLabel;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class Runner;

// "Reinstall programs": the programs a version recorded that are not on this computer. Those
// winget knows can be ticked and installed, one after the other, as the signed-in user (winget
// asks for administrator approval per program when its installer needs it); the others are
// listed to install by hand. Both steps run `vaultik apps` in the engine process.
class AppsDialog : public QDialog {
    Q_OBJECT

public:
    // `repo_args`: "--job <id>" or "--repo <location>"; `snapshot`: the version's id (hex).
    AppsDialog(QStringList repo_args, QString snapshot, QString password, vaultik::CloudCredentials cloud,
               QWidget* parent = nullptr);
    ~AppsDialog() override;

    void reject() override;

private:
    enum class Step { Loading, Ready, Installing };

    void load();
    void install();
    void update_buttons();
    QList<QTreeWidgetItem*> ticked() const;

    QStringList repo_args_;
    QString snapshot_;
    QString password_;
    vaultik::CloudCredentials cloud_;
    Step step_ = Step::Loading;

    QLabel* intro_ = nullptr;
    QTreeWidget* list_ = nullptr;
    QTreeWidgetItem* winget_group_ = nullptr;
    QTreeWidgetItem* manual_group_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* all_ = nullptr;
    QPushButton* install_ = nullptr;
    Runner* runner_ = nullptr;
    QHash<QString, QTreeWidgetItem*> by_id_;
    int installed_ = 0;
    int failed_ = 0;
};
