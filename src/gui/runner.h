#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>

#include "vaultik/repo/s3_backend.hpp"

// Runs the vaultik command-line engine as a child process with --json and turns its event
// lines into signals. Backups and restores run out of process so their resource limits (CPU
// cap, memory cap, priorities) never apply to the UI, and a crash cannot take the UI down.
class Runner : public QObject {
    Q_OBJECT

public:
    explicit Runner(QObject* parent = nullptr);
    ~Runner() override;

    // Path of vaultik(.exe) next to the GUI executable.
    static QString engine_path();

    bool running() const;
    // `password`, when given, reaches the engine as VAULTIK_PASSWORD (for a repository opened
    // without a saved job), and `cloud` keys as AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY and
    // AWS_REGION (and its storage class as VAULTIK_S3_STORAGE_CLASS). Inherited values of these are
    // always dropped.
    void start(const QStringList& args, const QString& password = {}, const vaultik::CloudCredentials& cloud = {});
    // Asks the engine to stop cleanly; kills it if it has not exited after a grace period.
    void cancel();
    // Pauses the running backup between two pieces of data, or lets it go on.
    void set_paused(bool paused);

signals:
    // A "progress" event of a backup, or "check_progress" and the final "check" of a health check.
    void progress(const QJsonObject& event);
    void warning(const QString& message);
    // status: success, partial, failed or cancelled. `event` is the engine's "done" event.
    void finished(const QString& status, const QJsonObject& event);

private:
    void read_output();
    void on_finished(int exit_code, QProcess::ExitStatus status);

    QProcess* process_ = nullptr;
    QByteArray buffer_;
    QString errors_;
    QJsonObject done_;
};
