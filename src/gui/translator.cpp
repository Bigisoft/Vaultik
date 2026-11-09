#include "translator.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QSettings>

namespace {

constexpr const char* kLanguages[] = {"bg"};  // besides English, the source language

}  // namespace

bool JsonTranslator::load_json(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    texts_.clear();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QString value = it.value().toString();
        if (!value.isEmpty()) texts_.insert(it.key(), value);
    }
    return !texts_.isEmpty();
}

QString JsonTranslator::translate(const char*, const char* source, const char*, int) const {
    if (source == nullptr) return {};
    return texts_.value(QString::fromUtf8(source));  // empty: Qt keeps the English text
}

QString chosen_language() {
    const QString v = QSettings().value(QStringLiteral("ui/language"), QStringLiteral("auto")).toString();
    return v.isEmpty() ? QStringLiteral("auto") : v;
}

QString effective_language() {
    const QString chosen = chosen_language();
    if (chosen != QLatin1String("auto")) return chosen;
    for (const QString& ui : QLocale::system().uiLanguages()) {
        for (const char* lang : kLanguages)
            if (ui.startsWith(QLatin1String(lang))) return QString::fromLatin1(lang);
        if (ui.startsWith(QLatin1String("en"))) break;  // English comes first: keep it
    }
    return QStringLiteral("en");
}

void install_translation(QApplication& app) {
    const QString lang = effective_language();
    // Dates and numbers in the same language as the words when one was chosen.
    if (chosen_language() == QLatin1String("en")) QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
    if (lang == QLatin1String("en")) return;
    if (chosen_language() != QLatin1String("auto")) QLocale::setDefault(QLocale(lang));
    static JsonTranslator translator;
    if (translator.load_json(QStringLiteral(":/translations/%1.json").arg(lang))) app.installTranslator(&translator);
}
