// vaultik-gui: the Qt 6 Widgets frontend. It manages saved jobs and their schedules, shows
// progress and history, and browses snapshots for restores. The backup work itself runs in the
// vaultik engine process.

#include <QApplication>

#include "main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Vaultik"));
    QApplication::setOrganizationName(QStringLiteral("Vaultik"));
    QApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    MainWindow window;
    window.show();
    return QApplication::exec();
}
