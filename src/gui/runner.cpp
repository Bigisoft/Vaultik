#include "runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QTimer>

Runner::Runner(QObject* parent) : QObject(parent) {}

Runner::~Runner() {
    if (running()) {
        process_->kill();
        process_->waitForFinished(3000);
    }
}

QString Runner::engine_path() {
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("vaultik.exe");
#else
    const QString name = QStringLiteral("vaultik");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

bool Runner::running() const {
    return process_ != nullptr && process_->state() != QProcess::NotRunning;
}

void Runner::start(const QStringList& args, const QString& password) {
    if (running()) return;
    if (process_ != nullptr) process_->deleteLater();
    buffer_.clear();
    errors_.clear();
    done_ = {};

    process_ = new QProcess(this);
    connect(process_, &QProcess::readyReadStandardOutput, this, &Runner::read_output);
    connect(process_, &QProcess::readyReadStandardError, this,
            [this] { errors_ += QString::fromUtf8(process_->readAllStandardError()); });
    connect(process_, &QProcess::finished, this, &Runner::on_finished);
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) {
            QJsonObject ev{{"event", "done"},
                           {"status", "failed"},
                           {"message", tr("Cannot start the backup engine %1").arg(engine_path())}};
            emit finished(QStringLiteral("failed"), ev);
        }
    });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("VAULTIK_PASSWORD"));
    if (!password.isEmpty()) env.insert(QStringLiteral("VAULTIK_PASSWORD"), password);
    process_->setProcessEnvironment(env);
    process_->start(engine_path(), QStringList(args) << QStringLiteral("--json"));
}

void Runner::cancel() {
    if (!running()) return;
    process_->write("cancel\n");
    QProcess* p = process_;
    QTimer::singleShot(15000, p, [p] {
        if (p->state() != QProcess::NotRunning) p->kill();
    });
}

void Runner::read_output() {
    buffer_ += process_->readAllStandardOutput();
    for (;;) {
        const qsizetype nl = buffer_.indexOf('\n');
        if (nl < 0) break;
        const QByteArray line = buffer_.left(nl).trimmed();
        buffer_.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        const QJsonObject ev = QJsonDocument::fromJson(line).object();
        const QString type = ev.value("event").toString();
        if (type == "progress") {
            emit progress(ev);
        } else if (type == "warning") {
            emit warning(ev.value("message").toString());
        } else if (type == "done") {
            done_ = ev;
        }
    }
}

void Runner::on_finished(int exit_code, QProcess::ExitStatus status) {
    read_output();
    QJsonObject ev = done_;
    QString result = ev.value("status").toString();
    if (result.isEmpty()) {
        // The engine died without reporting (killed, crashed): describe it from what we have.
        result = QStringLiteral("failed");
        QString msg = errors_.trimmed();
        if (msg.isEmpty())
            msg = status == QProcess::CrashExit ? tr("The backup engine stopped unexpectedly.")
                                                : tr("The backup engine exited with code %1.").arg(exit_code);
        ev = QJsonObject{{"event", "done"}, {"status", result}, {"message", msg}};
    }
    emit finished(result, ev);
}
