#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QStringList>

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
    void start(const QStringList& args);
    // Asks the engine to stop cleanly; kills it if it has not exited after a grace period.
    void cancel();

signals:
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
