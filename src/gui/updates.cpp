#include "updates.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QUrl>

#include "app_settings.h"
#include "runner.h"
#include "vaultik/jobs/update_check.hpp"
#include "vaultik/version.hpp"

void check_for_update(QObject* owner, std::function<void(const UpdateResult&)> done) {
    auto* process = new QProcess(owner);
    QPointer<QObject> alive(owner);
    QObject::connect(process, &QProcess::finished, owner, [process, alive, done](int, QProcess::ExitStatus) {
        UpdateResult r;
        r.error = QObject::tr("Vaultik could not check for a new version.");
        // One JSON object per line; the last "done" one has the answer.
        for (const QByteArray& line : process->readAllStandardOutput().split('\n')) {
            const QJsonObject ev = QJsonDocument::fromJson(line.trimmed()).object();
            if (ev.value(QStringLiteral("event")).toString() != QStringLiteral("done")) continue;
            if (ev.value(QStringLiteral("status")).toString() == QStringLiteral("success")) {
                r.ok = true;
                r.error.clear();
                r.newer = ev.value(QStringLiteral("update")).toBool();
                r.version = ev.value(QStringLiteral("version")).toString();
                r.url = ev.value(QStringLiteral("url")).toString();
            } else {
                r.error = ev.value(QStringLiteral("message")).toString();
            }
        }
        process->deleteLater();
        if (alive) done(r);
    });
    process->setProcessEnvironment(Runner::engine_environment());
    process->start(Runner::engine_path(), {QStringLiteral("update-check"), QStringLiteral("--json")});
}

bool update_check_due() {
    if (!AppSettings::load().check_updates) return false;
    const QDateTime last = QSettings().value(QStringLiteral("updates/last_check")).toDateTime();
    return !last.isValid() || last.secsTo(QDateTime::currentDateTimeUtc()) >= 24 * 3600 ||
           last > QDateTime::currentDateTimeUtc();
}

void remember_update_check(const UpdateResult& result) {
    QSettings s;
    s.setValue(QStringLiteral("updates/last_check"), QDateTime::currentDateTimeUtc());
    if (!result.ok) return;
    s.setValue(QStringLiteral("updates/version"), result.newer ? result.version : QString());
    s.setValue(QStringLiteral("updates/url"), result.newer ? result.url : QString());
}

UpdateResult known_update() {
    QSettings s;
    UpdateResult r;
    r.version = s.value(QStringLiteral("updates/version")).toString();
    r.url = s.value(QStringLiteral("updates/url")).toString();
    // Shown only while it is newer than this copy, and only a page of Vaultik's own releases.
    r.ok = r.newer = !r.version.isEmpty() &&
                     vaultik::jobs::newer_version(r.version.toStdString(), VAULTIK_VERSION) &&
                     r.url.startsWith(QStringLiteral("https://github.com/Bigisoft/Vaultik/releases/"));
    return r;
}

void show_update_result(QWidget* parent, const UpdateResult& r) {
    QMessageBox box(parent);
    box.setWindowTitle(QObject::tr("Check for a new version"));
    if (!r.ok) {
        box.setIcon(QMessageBox::Warning);
        box.setText(r.error.isEmpty() ? QObject::tr("Vaultik could not check for a new version.") : r.error);
        box.exec();
        return;
    }
    if (!r.newer) {
        box.setIcon(QMessageBox::Information);
        box.setText(QObject::tr("Vaultik %1 is the newest version.").arg(QStringLiteral(VAULTIK_VERSION)));
        box.exec();
        return;
    }
    box.setIcon(QMessageBox::Information);
    box.setText(QObject::tr("Vaultik %1 is available (this is %2).").arg(r.version, QStringLiteral(VAULTIK_VERSION)));
    box.setInformativeText(QObject::tr("Download it from its page on GitHub and install it over this one: "
                                       "jobs, settings and backups are kept."));
    QPushButton* open = box.addButton(QObject::tr("Open the release page"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Close);
    box.exec();
    if (box.clickedButton() == open) QDesktopServices::openUrl(QUrl(r.url));
}
