#include "common.h"

#include <QDateTime>

#include "vaultik/platform/os.hpp"

QString format_bytes(std::uint64_t bytes) {
    static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
    double v = static_cast<double>(bytes);
    int unit = 0;
    while (v >= 1024.0 && unit < 5) {
        v /= 1024.0;
        ++unit;
    }
    if (unit == 0) return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(v, 0, 'f', v < 10 ? 2 : (v < 100 ? 1 : 0)).arg(kUnits[unit]);
}

QString format_time(std::uint64_t filetime) {
    if (filetime < vaultik::os::kUnixEpochTicks) return QStringLiteral("-");
    const qint64 ms = static_cast<qint64>((filetime - vaultik::os::kUnixEpochTicks) / 10000);
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString format_duration(double seconds) {
    if (seconds < 60) return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
    const auto total = static_cast<qint64>(seconds);
    if (total < 3600) return QStringLiteral("%1 min %2 s").arg(total / 60).arg(total % 60);
    return QStringLiteral("%1 h %2 min").arg(total / 3600).arg((total % 3600) / 60);
}
