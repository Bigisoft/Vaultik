#pragma once

#include <QDialog>

#include <memory>
#include <vector>

#include "repo_handle.h"
#include "vaultik/jobs/growth.hpp"
#include "vaultik/jobs/job.hpp"

class QComboBox;
class QLabel;
class QTreeWidget;

// "What grew": the folders that added the most new data to a job's backup over a period (see
// jobs/growth.hpp), with a button to leave a folder out from now on where a ready-made exclusion
// (or a well-known folder such as Downloads) fits. Works on the job's open repository.
class GrewDialog : public QDialog {
    Q_OBJECT

public:
    GrewDialog(const vaultik::jobs::JobStore& store, vaultik::jobs::Job job,
               std::shared_ptr<vaultik::Repository> repo, std::vector<SnapshotEntry> snapshots,
               QWidget* parent = nullptr);

    // Whether an exclusion was added (the job was saved).
    bool job_changed() const { return changed_; }

private:
    void compute();
    void show_report();
    // Adds the patterns to the job's exclusions and saves it; false when the user said no or it failed.
    bool add_exclusion(const QString& what, const std::vector<std::string>& patterns);

    const vaultik::jobs::JobStore& store_;
    vaultik::jobs::Job job_;
    std::shared_ptr<vaultik::Repository> repo_;
    std::vector<std::pair<vaultik::Id, vaultik::Snapshot>> snapshots_;
    vaultik::jobs::GrowthReport report_;
    bool changed_ = false;

    QComboBox* period_ = nullptr;
    QLabel* summary_ = nullptr;
    QTreeWidget* folders_ = nullptr;
    QTreeWidget* presets_ = nullptr;
    QLabel* presets_title_ = nullptr;
};
