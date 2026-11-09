#pragma once

#include <QDateTime>
#include <QString>

#include <cstdint>
#include <functional>
#include <string>

class QWidget;

namespace vaultik {
struct RetentionPolicy;
}

// UTF-8 conversions between Qt and the engine.
inline std::string u8(const QString& s) {
    return s.toStdString();
}

inline QString qs(const std::string& s) {
    return QString::fromStdString(s);
}

// "1.23 GiB", like the command line.
QString format_bytes(std::uint64_t bytes);
// FILETIME ticks to local "2026-11-08 14:48".
QString format_time(std::uint64_t filetime);
QString format_duration(double seconds);
// "1 file" / "12 files", with thousands separators.
QString count_of(std::uint64_t n, const QString& one, const QString& many);
// FILETIME ticks to a local date and time (invalid for 0).
QDateTime to_datetime(std::uint64_t filetime);

// Where a snapshot path ("C/Users/me/x.txt") came from on this computer ("C:\Users\me\x.txt"); empty
// when it has no sensible original location (a whole drive or share).
QString original_path(const QString& snapshot_path);

// The cleanup rules in plain words: "Keeps every version of the last 7 days, then one per week...".
QString retention_summary(const vaultik::RetentionPolicy& policy);

// Runs `work` on a worker thread behind a modal progress dialog with a Cancel button, so the
// window stays responsive and nothing else touches the same repository meanwhile. Returns the
// error text, empty on success ("Cancelled." when `work` stopped because of the cancel).
QString run_busy(QWidget* parent, const QString& text,
                 const std::function<void(const std::function<bool()>& cancelled)>& work);
