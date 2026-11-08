#pragma once

#include <QString>

#include <cstdint>
#include <string>

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
