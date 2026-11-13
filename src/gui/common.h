#pragma once

#include <QDateTime>
#include <QSize>
#include <QString>

#include <cstdint>
#include <functional>
#include <string>

class QIODevice;
class QWidget;

namespace vaultik {
struct RetentionPolicy;
namespace jobs {
struct FillForecast;
}
}

// UTF-8 conversions between Qt and the engine.
inline std::string u8(const QString& s) {
    return s.toStdString();
}

class QWidget;
// A warning box whose text is shown as plain text, never as markup: for messages that carry file
// names or text from a server.
void plain_warning(QWidget* parent, const QString& title, const QString& text);

// A page inside a frameless scroll area: the page keeps its natural size, and a window holding it
// can be made smaller than the page (which would otherwise set the window's minimum height and push
// it under the taskbar on small screens).
QWidget* scrolled(QWidget* page);

// Gives a top-level window the wanted size, but never more than the free area of its screen (the
// screen of `anchor` when given, without the taskbar), centred on that area, with a maximize button.
void fit_to_screen(QWidget* window, QSize wanted, const QWidget* anchor = nullptr);
// The same for a Yes/No question; true for Yes.
bool plain_question(QWidget* parent, const QString& title, const QString& text);

// Saves a file that only this account may read (a recovery key or kit): `write` fills a new file in
// the same folder, made readable by the owner alone, which then takes the place of `path`. So a
// file that was there keeps none of its own (perhaps looser) permissions, and a write that fails
// leaves the old file as it was, never half a file. `write` returns whether it wrote it all.
bool save_private_file(const QString& path, const std::function<bool(QIODevice&)>& write, bool text = false);

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
std::uint64_t from_datetime(const QDateTime& dt);

// Where a snapshot path ("C/Users/me/x.txt") came from on this computer ("C:\Users\me\x.txt"); empty
// when it has no sensible original location (a whole drive or share).
QString original_path(const QString& snapshot_path);

// When the backup drive fills up, in words: "At this rate the drive is full in about 6 weeks
// (grows 1.2 GiB a day)". Empty when there is nothing worth saying (too little history, shrinking).
QString fill_forecast_text(const vaultik::jobs::FillForecast& forecast);
// "in about 6 weeks", translated.
QString fill_time_words(double days);

// The name of a ready-made exclusion (jobs::exclusion_presets) in words: "Developer folders
// (node_modules, Python caches)".
QString exclusion_preset_label(const std::string& id);

// The cleanup rules in plain words: "Keeps every version of the last 7 days, then one per week...".
QString retention_summary(const vaultik::RetentionPolicy& policy);

// Runs `work` on a worker thread behind a modal progress dialog with a Cancel button, so the
// window stays responsive and nothing else touches the same repository meanwhile. Returns the
// error text, empty on success ("Cancelled." when `work` stopped because of the cancel).
QString run_busy(QWidget* parent, const QString& text,
                 const std::function<void(const std::function<bool()>& cancelled)>& work);
