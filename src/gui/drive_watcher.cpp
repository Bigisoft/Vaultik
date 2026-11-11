#include "drive_watcher.h"

#include <QCoreApplication>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbt.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <QSocketNotifier>

namespace {

// The mount points in /proc/self/mountinfo (field 5, with its octal escapes undone).
QSet<QString> mount_points(int fd) {
    QSet<QString> out;
    QByteArray text;
    char buf[16384];
    ::lseek(fd, 0, SEEK_SET);
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n <= 0) break;
        text.append(buf, n);
    }
    for (const QByteArray& line : text.split('\n')) {
        const QList<QByteArray> f = line.split(' ');
        if (f.size() < 5) continue;
        QByteArray point;
        const QByteArray& raw = f[4];
        for (qsizetype i = 0; i < raw.size(); ++i) {
            if (raw[i] == '\\' && i + 3 < raw.size()) {
                point.append(static_cast<char>(((raw[i + 1] - '0') << 6) | ((raw[i + 2] - '0') << 3) | (raw[i + 3] - '0')));
                i += 3;
            } else {
                point.append(raw[i]);
            }
        }
        out.insert(QString::fromUtf8(point));
    }
    return out;
}

}  // namespace
#endif

DriveWatcher::DriveWatcher(QObject* parent) : QObject(parent) {
    QCoreApplication::instance()->installNativeEventFilter(this);
#ifndef Q_OS_WIN
    // Linux: the kernel marks /proc/self/mountinfo (POLLPRI) whenever something is mounted or
    // unmounted, e.g. when the desktop (udisks2) mounts a plugged-in drive under /run/media.
    mountinfo_ = ::open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mountinfo_ < 0) return;
    points_ = mount_points(mountinfo_);
    auto* notifier = new QSocketNotifier(mountinfo_, QSocketNotifier::Exception, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] {
        const QSet<QString> now = mount_points(mountinfo_);
        QStringList added;
        for (const QString& p : now)
            if (!points_.contains(p)) added << p;
        points_ = now;
        // A moment for the file system to settle, as on Windows.
        if (!added.isEmpty()) QTimer::singleShot(3000, this, [this, added] { emit connected(added); });
    });
#endif
}

DriveWatcher::~DriveWatcher() {
    if (QCoreApplication::instance() != nullptr) QCoreApplication::instance()->removeNativeEventFilter(this);
#ifndef Q_OS_WIN
    if (mountinfo_ >= 0) ::close(mountinfo_);
#endif
}

bool DriveWatcher::nativeEventFilter(const QByteArray& type, void* message, qintptr*) {
#ifdef Q_OS_WIN
    if (type != "windows_generic_MSG") return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg->message != WM_DEVICECHANGE || msg->wParam != DBT_DEVICEARRIVAL || msg->lParam == 0) return false;
    const auto* header = reinterpret_cast<const DEV_BROADCAST_HDR*>(msg->lParam);
    if (header->dbch_devicetype != DBT_DEVTYP_VOLUME) return false;
    const DWORD mask = reinterpret_cast<const DEV_BROADCAST_VOLUME*>(msg->lParam)->dbcv_unitmask;
    QStringList roots;
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) roots << QStringLiteral("%1:\\").arg(QChar('A' + i));
    // Every top-level window gets the message; report it once. Give Windows a moment to finish
    // mounting the file system before anyone looks at it.
    static DWORD last_time = 0;
    static DWORD last_mask = 0;
    if (msg->time == last_time && mask == last_mask) return false;
    last_time = msg->time;
    last_mask = mask;
    if (!roots.isEmpty()) QTimer::singleShot(3000, this, [this, roots] { emit connected(roots); });
#else
    (void)type;
    (void)message;
#endif
    return false;
}
