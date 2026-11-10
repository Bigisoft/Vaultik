#pragma once

#include <QString>

#include <functional>

class QObject;
class QWidget;

// The answer of "vaultik update-check": whether a newer Vaultik has been released.
struct UpdateResult {
    bool ok = false;     // GitHub answered
    bool newer = false;  // a newer release exists
    QString version;     // its version ("0.7.0")
    QString url;         // its page on GitHub
    QString error;       // why the check failed, when !ok
};

// Asks the engine without blocking; `done` gets the answer on the GUI thread (not when `owner`
// is gone by then).
void check_for_update(QObject* owner, std::function<void(const UpdateResult&)> done);

// The once-a-day check: due when it is on in the settings and the last one was a day ago or more.
bool update_check_due();
// Remembers when the last check ran and what it found (a newer release shown in Settings).
void remember_update_check(const UpdateResult& result);
// The newer release the last check found, if any (its version is still newer than this one).
UpdateResult known_update();

// The answer of a check started by hand: a message, with a button to open the release page.
void show_update_result(QWidget* parent, const UpdateResult& result);
