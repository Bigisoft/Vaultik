#pragma once

#include <QHash>
#include <QString>
#include <QTranslator>

class QApplication;

// Translates the frontend from a JSON file of English text -> translation (translations/<lang>.json,
// built into the program and kept up to date with tools/extract_strings.py). The same English text
// gets the same translation everywhere, including Qt's own button texts.
class JsonTranslator : public QTranslator {
public:
    bool load_json(const QString& path);
    QString translate(const char* context, const char* source, const char* disambiguation = nullptr,
                      int n = -1) const override;
    bool isEmpty() const override { return texts_.isEmpty(); }

private:
    QHash<QString, QString> texts_;
};

// The language chosen in Settings ("auto", "en" or "bg"), and the one in use: "auto" follows
// Windows' display language when Vaultik has it, otherwise English.
QString chosen_language();
QString effective_language();

// Installs the translation for the effective language (call once, before any window is made).
void install_translation(QApplication& app);
