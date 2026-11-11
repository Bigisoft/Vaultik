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
    // In a Vaultik started as administrator, the engine runs with the user's own rights (see
    // user_rights_environment) unless this is called before start(): for work the user started
    // in a window that shows what it does (a restore, a disk image restore, installing programs).
    void keep_administrator_rights(bool keep = true) { keep_rights_ = keep; }

    // The environment for an engine process: the GUI's own, and in a Vaultik started as
    // administrator VAULTIK_USER_RIGHTS=1, which makes the engine run again with the user's own
    // rights before it reads anything (os::run_with_user_rights). Jobs, their commands and their
    // destinations come from the user's settings, which any program running as the user can
    // change, so they never get administrator rights without an approval.
    static QProcessEnvironment engine_environment(bool keep_rights = false);
    // Starts `program` detached (it goes on when Vaultik closes) with engine_environment().
    static bool start_detached(const QString& program, const QStringList& args, qint64* pid = nullptr);
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
    // Any other event before "done" (such as "app" of `vaultik apps`).
    void event(const QJsonObject& event);
    // status: success, partial, failed or cancelled. `event` is the engine's "done" event.
    void finished(const QString& status, const QJsonObject& event);

private:
    void read_output();
    void on_finished(int exit_code, QProcess::ExitStatus status);

    QProcess* process_ = nullptr;
    QByteArray buffer_;
    QString errors_;
    QJsonObject done_;
    bool keep_rights_ = false;
};
