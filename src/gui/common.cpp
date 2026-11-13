#include "common.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QEventLoop>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <exception>

#include "vaultik/jobs/forecast.hpp"
#include "vaultik/ops/prune.hpp"
#include "vaultik/platform/os.hpp"

QWidget* scrolled(QWidget* page) {
    auto* area = new QScrollArea;
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

void fit_to_screen(QWidget* window, QSize wanted, const QWidget* anchor) {
    window->setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    QScreen* screen = anchor != nullptr && anchor->screen() != nullptr ? anchor->screen() : QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        window->resize(wanted);
        return;
    }
    // Room for the title bar and frame, which the size does not include.
    const QRect free = screen->availableGeometry();
    const QSize size(std::min(wanted.width(), free.width() - 40), std::min(wanted.height(), free.height() - 60));
    window->resize(size);
    window->move(free.center() - QPoint(size.width() / 2, size.height() / 2));
}

void plain_warning(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Warning, title, QString(), QMessageBox::Ok, parent);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    box.exec();
}

bool plain_question(QWidget* parent, const QString& title, const QString& text) {
    QMessageBox box(QMessageBox::Question, title, QString(), QMessageBox::Yes | QMessageBox::No, parent);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    return box.exec() == QMessageBox::Yes;
}

bool save_private_file(const QString& path, const std::function<bool(QIODevice&)>& write, bool text) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | (text ? QIODevice::Text : QIODevice::NotOpen))) return false;
    // (QSaveFile gives the new file the old one's permissions; on Windows a file in the user's
    // folders is private already.)
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!write(f) || f.error() != QFileDevice::NoError) {
        f.cancelWriting();
        return false;
    }
    return f.commit();  // false, with the new file removed, when it could not be finished
}

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

QString count_of(std::uint64_t n, const QString& one, const QString& many) {
    return QLocale().toString(static_cast<qulonglong>(n)) + " " + (n == 1 ? one : many);
}

QString fill_time_words(double days) {
    const auto [phrase, n] = vaultik::jobs::fill_time_phrase(days);
    // The phrases are fixed English texts with their translations in the translation file.
    return QObject::tr(phrase.c_str()).replace(QStringLiteral("%1"), QLocale().toString(n));
}

QString fill_forecast_text(const vaultik::jobs::FillForecast& f) {
    using State = vaultik::jobs::FillForecast::State;
    const QString growth = format_bytes(static_cast<std::uint64_t>(f.growth_per_day > 0 ? f.growth_per_day : 0));
    switch (f.state) {
        case State::Fills:
            return QObject::tr("At this rate the drive is full %1 (grows %2 a day)").arg(fill_time_words(f.days), growth);
        case State::NoSpaceInfo: return QObject::tr("The backup grows about %1 a day").arg(growth);
        case State::Steady: return QObject::tr("The backup is not filling up the drive");
        case State::NoData:
        case State::Shrinking: return {};
    }
    return {};
}

QDateTime to_datetime(std::uint64_t filetime) {
    if (filetime < vaultik::os::kUnixEpochTicks) return {};
    const qint64 ms = static_cast<qint64>((filetime - vaultik::os::kUnixEpochTicks) / 10000);
    return QDateTime::fromMSecsSinceEpoch(ms);
}

std::uint64_t from_datetime(const QDateTime& dt) {
    return vaultik::os::kUnixEpochTicks + static_cast<std::uint64_t>(dt.toMSecsSinceEpoch()) * 10000;
}

QString format_time(std::uint64_t filetime) {
    const QDateTime dt = to_datetime(filetime);
    return dt.isValid() ? dt.toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("-");
}

QString format_duration(double seconds) {
    if (seconds < 60) return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
    const auto total = static_cast<qint64>(seconds);
    if (total < 3600) return QStringLiteral("%1 min %2 s").arg(total / 60).arg(total % 60);
    return QStringLiteral("%1 h %2 min").arg(total / 3600).arg((total % 3600) / 60);
}

QString original_path(const QString& snapshot_path) {
    const QStringList parts = snapshot_path.split('/', Qt::SkipEmptyParts);
#ifdef Q_OS_WIN
    if (parts.isEmpty()) return {};
    if (parts[0] == "UNC") return parts.size() < 4 ? QString() : "\\\\" + parts.mid(1).join('\\');
    if (parts[0].size() != 1 || parts.size() < 2) return {};
    return parts[0] + ":\\" + parts.mid(1).join('\\');
#else
    if (parts.isEmpty()) return {};
    return "/" + parts.join('/');
#endif
}

QString exclusion_preset_label(const std::string& id) {
    if (id == "temp") return QObject::tr("Temporary files");
    if (id == "caches") return QObject::tr("Browser and app caches");
    if (id == "dev") return QObject::tr("Developer folders (node_modules, Python caches)");
    if (id == "clutter") return QObject::tr("Windows clutter (thumbnails, page file)");
    if (id == "recycle") return QObject::tr("Recycle bin");
    return qs(id);
}

QString retention_summary(const vaultik::RetentionPolicy& p) {
    using vaultik::RetentionPolicy;
    QString text;
    if (p.no_rules()) {
        text = QObject::tr("Every version is kept.");
    } else {
        QStringList parts;
        if (p.within_days > 0) parts << QObject::tr("every version from the last %1").arg(count_of(p.within_days, QObject::tr("day"), QObject::tr("days")));
        if (p.last > 0) parts << QObject::tr("the newest %1").arg(count_of(p.last, QObject::tr("version"), QObject::tr("versions")));
        if (p.hourly > 0) parts << QObject::tr("one per hour for %1").arg(count_of(p.hourly, QObject::tr("hour"), QObject::tr("hours")));
        if (p.daily > 0) parts << QObject::tr("one per day for %1").arg(count_of(p.daily, QObject::tr("day"), QObject::tr("days")));
        if (p.weekly > 0) parts << QObject::tr("one per week for %1").arg(count_of(p.weekly, QObject::tr("week"), QObject::tr("weeks")));
        if (p.monthly > 0) parts << QObject::tr("one per month for %1").arg(count_of(p.monthly, QObject::tr("month"), QObject::tr("months")));
        if (p.yearly > 0) parts << QObject::tr("one per year for %1").arg(count_of(p.yearly, QObject::tr("year"), QObject::tr("years")));
        QString list = parts.size() > 1 ? parts.mid(0, parts.size() - 1).join(QStringLiteral(", ")) + QObject::tr(" and ") + parts.last()
                                        : parts.join(QString());
        text = QObject::tr("Keeps %1.").arg(list);
    }
    if (p.max_bytes > 0)
        text += " " + QObject::tr("Older versions are also removed to keep the backup under %1.").arg(format_bytes(p.max_bytes));
    if (!p.empty()) text += " " + QObject::tr("Pinned versions are always kept.");
    return text;
}

QString run_busy(QWidget* parent, const QString& text,
                 const std::function<void(const std::function<bool()>& cancelled)>& work) {
    std::atomic<bool> cancel{false};
    QString error;
    // Owned here and deleted after it finished; the dialog lives on the stack.
    QThread* thread = QThread::create([&] {
        try {
            work([&] { return cancel.load(); });
        } catch (const std::exception& e) {
            error = QString::fromUtf8(e.what());
        }
    });
    QProgressDialog dialog(text, QObject::tr("Cancel"), 0, 0, parent);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setMinimumDuration(400);  // quick work finishes without a flash of a dialog
    // Cancel only asks: the dialog stays up (and the window blocked) until the work has really
    // stopped, as it may still be using a repository nothing else may touch meanwhile.
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    QEventLoop loop;
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QObject::connect(&dialog, &QProgressDialog::canceled, &dialog, [&] {
        cancel = true;
        dialog.setLabelText(QObject::tr("Stopping..."));
        dialog.setCancelButton(nullptr);
        dialog.show();  // a canceled dialog hides itself: keep it showing
    });
    thread->start();
    loop.exec();
    thread->wait();
    delete thread;
    // Work that noticed the cancel stopped with an error; work that finished anyway succeeded.
    if (cancel && !error.isEmpty()) error = QObject::tr("Cancelled.");
    return error;
}
