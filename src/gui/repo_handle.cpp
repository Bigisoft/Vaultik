#include "repo_handle.h"

#include <QPointer>
#include <QThread>

#include <algorithm>

#include "vaultik/crypto/crypto.hpp"
#include "vaultik/repo/backends.hpp"

using namespace vaultik;

void RepoHandle::open_async(QObject* context, std::string repository, std::string password, CloudCredentials cloud,
                            Done done) {
    struct Result {
        std::shared_ptr<Repository> repo;
        std::vector<SnapshotEntry> snapshots;
        QString error;
    };
    auto result = std::make_shared<Result>();
    QThread* thread = QThread::create([result, repository, password, cloud]() mutable {
        try {
            std::unique_ptr<Repository> repo = Repository::open(open_backend(repository, &cloud), password);
            for (const Id& id : repo->list_snapshot_ids()) {
                try {
                    result->snapshots.push_back({id, repo->load_snapshot(id)});
                } catch (const Error&) {
                    // unreadable snapshots are reported by verify
                }
            }
            std::sort(result->snapshots.begin(), result->snapshots.end(),
                      [](const SnapshotEntry& a, const SnapshotEntry& b) { return a.snapshot.time > b.snapshot.time; });
            result->repo = std::move(repo);
        } catch (const AuthError&) {
            result->error = QObject::tr("The password does not open this backup.");
        } catch (const std::exception& e) {
            result->error = QString::fromUtf8(e.what());
        }
        secure_wipe(password.data(), password.size());
    });
    QPointer<QObject> guard(context);
    QObject::connect(thread, &QThread::finished, context, [thread, result, guard, done = std::move(done)] {
        thread->deleteLater();
        if (guard) done(result->repo, std::move(result->snapshots), result->error);
    });
    thread->start();
}
