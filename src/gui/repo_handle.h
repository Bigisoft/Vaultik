#pragma once

#include <QObject>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "vaultik/crypto/hash.hpp"
#include "vaultik/jobs/computers.hpp"
#include "vaultik/model/snapshot.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/repo/repository.hpp"
#include "vaultik/repo/s3_backend.hpp"

struct SnapshotEntry {
    vaultik::Id id{};
    vaultik::Snapshot snapshot;
};

// The same list in the engine's form (for history, search and retention).
std::vector<vaultik::SnapshotRecord> records_of(const std::vector<SnapshotEntry>& entries);

// Opens a job's repository on a worker thread (key derivation and index loading take a moment,
// longer on a USB disk) and lists its snapshots, newest first. The opened repository is then
// used from the UI thread only.
class RepoHandle : public QObject {
    Q_OBJECT

public:
    using Done = std::function<void(std::shared_ptr<vaultik::Repository>, std::vector<SnapshotEntry>, QString error)>;

    // Calls `done` on the UI thread. `context` going away cancels the callback. `cloud` holds the
    // keys for a cloud storage location.
    static void open_async(QObject* context, std::string repository, std::string password,
                           vaultik::CloudCredentials cloud, Done done);

    // Opens the repository on a worker thread without loading its index, and reads the computers
    // backing up there (jobs/computers.hpp). The repository is closed again on that thread.
    using ComputersDone = std::function<void(std::vector<vaultik::jobs::Computer>, QString error)>;
    static void computers_async(QObject* context, std::string repository, std::string password,
                                vaultik::CloudCredentials cloud, ComputersDone done);
};
