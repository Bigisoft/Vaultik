#include "drive_watcher.h"

#include <QCoreApplication>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dbt.h>
#endif

DriveWatcher::DriveWatcher(QObject* parent) : QObject(parent) {
    QCoreApplication::instance()->installNativeEventFilter(this);
}

DriveWatcher::~DriveWatcher() {
    if (QCoreApplication::instance() != nullptr) QCoreApplication::instance()->removeNativeEventFilter(this);
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
