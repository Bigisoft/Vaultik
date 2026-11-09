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
    a.notify_failures = s.value(QStringLiteral("notify/failures"), a.notify_failures).toBool();
    a.notify_overdue = s.value(QStringLiteral("notify/overdue"), a.notify_overdue).toBool();
    a.remind_after_days = std::clamp(s.value(QStringLiteral("notify/remind_after_days"), a.remind_after_days).toInt(), 0, 365);
    return a;
}

void AppSettings::save() const {
    QSettings s;
    s.setValue(QStringLiteral("notify/failures"), notify_failures);
    s.setValue(QStringLiteral("notify/overdue"), notify_overdue);
    s.setValue(QStringLiteral("notify/remind_after_days"), remind_after_days);
}

bool starts_with_windows() {
#ifdef Q_OS_WIN
    const QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    return run.contains(QStringLiteral("Vaultik"));
#else
    return false;
#endif
}

void set_start_with_windows(bool on) {
#ifdef Q_OS_WIN
    QSettings run(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (on) {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue(QStringLiteral("Vaultik"), "\"" + exe + "\" --tray");
    } else {
        run.remove(QStringLiteral("Vaultik"));
    }
#else
    (void)on;
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
        if (it->status == jobs::RunStatus::Success || it->status == jobs::RunStatus::Partial) return it->started;
    return 0;
}
