#pragma once

#include <QString>

class QWidget;

// Windows Hello (face, fingerprint, PIN or a security key) as a check before backups are opened,
// so whoever finds the computer unlocked cannot restore from them. Elsewhere it is unavailable.
namespace hello {

enum class Result { Verified, Cancelled, Unavailable, Failed };

// Whether Windows Hello is set up for this account.
bool available();
// Asks for Windows Hello over `window`, showing `message`. Waits for the answer while the
// application keeps drawing.
Result verify(QWidget* window, const QString& message);

}  // namespace hello
