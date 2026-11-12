#pragma once

#include <QString>

class QWidget;

// Saves the recovery kit of a backup (see jobs/recovery_kit.hpp) as a PDF or a web page the user
// picks, readable by this account only, then offers to open it for printing. `key` is the recovery
// key as shown, `location` the backup's location.
void print_recovery_kit(QWidget* parent, const QString& key, const QString& location);
