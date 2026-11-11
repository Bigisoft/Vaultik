#include "app_settings.h"

#include <QCoreApplication>

#include <algorithm>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>

#include "vaultik/jobs/admin_jobs.hpp"

using namespace vaultik;

namespace {

#ifdef Q_OS_WIN
constexpr const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
#else
// Linux: an XDG autostart entry, which every desktop (also SteamOS' Desktop Mode, KDE Plasma)
// starts at sign-in. In a Flatpak, the real ~/.config (the manifest grants xdg-config/autostart).
QString autostart_file() {
    QString base = qEnvironmentVariable("FLATPAK_ID").isEmpty() ? qEnvironmentVariable("XDG_CONFIG_HOME") : QString();
    if (base.isEmpty()) base = QDir::homePath() + QStringLiteral("/.config");
    return base + QStringLiteral("/autostart/vaultik.desktop");
}

// One argument of a desktop entry's Exec line, quoted as the specification asks.
QString exec_quote(const QString& arg) {
    QString out;
    for (const QChar c : arg) {
        if (c == QLatin1Char('"') || c == QLatin1Char('`') || c == QLatin1Char('$') || c == QLatin1Char('\\'))
            out += QLatin1Char('\\');
        out += c;
    }
    out.replace(QLatin1Char('%'), QStringLiteral("%%"));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

// How to start this program again later: the AppImage file rather than its temporary mount, or
// the Flatpak.
QString launch_command() {
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (!appimage.isEmpty()) return exec_quote(appimage);
    const QString flatpak = qEnvironmentVariable("FLATPAK_ID");
    if (!flatpak.isEmpty()) return QStringLiteral("flatpak run ") + exec_quote(flatpak);
    return exec_quote(QCoreApplication::applicationFilePath());
}
#endif

}  // namespace

AppSettings AppSettings::load() {
    QSettings s;
    AppSettings a;
    const auto flag = [&](const char* key, bool fallback) { return s.value(QString::fromLatin1(key), fallback).toBool(); };
    a.tray_icon = flag("tray/icon", a.tray_icon);
    a.tray_status = flag("tray/status", a.tray_status);
    a.close_to_tray = flag("tray/close", a.close_to_tray);
    a.minimize_to_tray = flag("tray/minimize", a.minimize_to_tray);
    a.single_click_toggle = flag("tray/single_click", a.single_click_toggle);
    a.notify_failures = flag("notify/failures", a.notify_failures);
    a.notify_overdue = flag("notify/overdue", a.notify_overdue);
    a.require_hello = flag("security/hello", a.require_hello);
    a.check_updates = flag("updates/check", a.check_updates);
    a.remind_after_days = std::clamp(s.value(QStringLiteral("notify/remind_after_days"), a.remind_after_days).toInt(), 0, 365);
    return a;
}

void AppSettings::save() const {
    QSettings s;
    s.setValue(QStringLiteral("tray/icon"), tray_icon);
    s.setValue(QStringLiteral("tray/status"), tray_status);
    s.setValue(QStringLiteral("tray/close"), close_to_tray);
    s.setValue(QStringLiteral("tray/minimize"), minimize_to_tray);
    s.setValue(QStringLiteral("tray/single_click"), single_click_toggle);
    s.setValue(QStringLiteral("notify/failures"), notify_failures);
    s.setValue(QStringLiteral("notify/overdue"), notify_overdue);
    s.setValue(QStringLiteral("security/hello"), require_hello);
    s.setValue(QStringLiteral("updates/check"), check_updates);
    s.setValue(QStringLiteral("notify/remind_after_days"), remind_after_days);
}

StartupSetting startup_setting() {
    StartupSetting out;
#ifdef Q_OS_WIN
    const QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    const QString command = run.value(QStringLiteral("Vaultik")).toString();
    out.enabled = !command.isEmpty();
    out.hidden = command.contains(QStringLiteral("--tray"));
#else
    QFile file(autostart_file());
    if (file.open(QIODevice::ReadOnly)) {
        const QString text = QString::fromUtf8(file.read(64 * 1024));
        out.enabled = !text.contains(QStringLiteral("\nHidden=true")) && !text.contains(QStringLiteral("\nX-GNOME-Autostart-enabled=false"));
        out.hidden = text.contains(QStringLiteral("--tray"));
    }
#endif
    return out;
}

void set_startup_setting(const StartupSetting& setting) {
#ifdef Q_OS_WIN
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (setting.enabled) {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(QStringLiteral("Vaultik"), "\"" + exe + "\"" + (setting.hidden ? QStringLiteral(" --tray") : QString()));
    } else {
        run.remove(QStringLiteral("Vaultik"));
    }
#else
    const QString path = autostart_file();
    if (!setting.enabled) {
        QFile::remove(path);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    const QString entry = QStringLiteral("[Desktop Entry]\nType=Application\nName=Vaultik\nComment=Backups\nExec=") +
                          launch_command() + (setting.hidden ? QStringLiteral(" --tray") : QString()) +
                          QStringLiteral("\nIcon=io.github.bigisoft.Vaultik\nTerminal=false\nX-GNOME-Autostart-enabled=true\n");
    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(entry.toUtf8());
        file.commit();
    }
#endif
}

int overdue_after_days(const jobs::Job& job, int remind_after_days) {
    switch (job.schedule.kind) {
        case jobs::Schedule::Kind::Hourly: return job.schedule.every_hours <= 12 ? 1 : 2;
        case jobs::Schedule::Kind::Daily: return 2;
        case jobs::Schedule::Kind::Weekly: {
            // The longest gap between the chosen weekdays, plus a day.
            int days = 0;
            int longest = 0;
            for (int i = 0; i < 14; ++i) {
                if (job.schedule.weekdays & (1u << (i % 7))) {
                    longest = std::max(longest, days);
                    days = 1;
                } else {
                    ++days;
                }
            }
            return std::max(1, longest) + 1;
        }
        case jobs::Schedule::Kind::Manual: return remind_after_days;
    }
    return remind_after_days;
}

std::uint64_t last_saved(const jobs::JobStore& store, const jobs::Job& job) {
    const auto records = jobs::combined_history(store, job.id, 200);
    for (auto it = records.rbegin(); it != records.rend(); ++it)
        if (it->kind == jobs::RunKind::Backup &&
            (it->status == jobs::RunStatus::Success || it->status == jobs::RunStatus::Partial))
            return it->started;
    return 0;
}
