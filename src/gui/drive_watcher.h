#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QSet>
#include <QStringList>

// Tells when drives are connected (Windows: the WM_DEVICECHANGE volume arrival that Windows
// sends to every top-level window, so the app needs one, even hidden; Linux: new mount points in
// /proc/self/mountinfo).
class DriveWatcher : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit DriveWatcher(QObject* parent = nullptr);
    ~DriveWatcher() override;

    bool nativeEventFilter(const QByteArray& type, void* message, qintptr* result) override;

signals:
    // The roots of the drives just connected ("F:\"), a moment after Windows reported them.
    void connected(const QStringList& roots);

#ifndef Q_OS_WIN
private:
    int mountinfo_ = -1;
    QSet<QString> points_;
#endif
};
