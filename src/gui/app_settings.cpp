#include "app_settings.h"

#include <QCoreApplication>

#include <algorithm>
#include <QDir>
#include <QSettings>

#include "vaultik/jobs/admin_jobs.hpp"

using namespace vaultik;

namespace {

constexpr const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";

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
    s.setValue(QStringLiteral("notify/remind_after_days"), remind_after_days);
}

StartupSetting startup_setting() {
    StartupSetting out;
#ifdef Q_OS_WIN
    const QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    const QString command = run.value(QStringLiteral("Vaultik")).toString();
    out.enabled = !command.isEmpty();
    out.hidden = command.contains(QStringLiteral("--tray"));
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
    (void)setting;
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
