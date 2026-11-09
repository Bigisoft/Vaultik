#pragma once

// Vaultik's own look: a light or dark theme that follows the Windows setting, with the Windows
// accent colour, drawn over Qt's neutral Fusion style with a palette and a style sheet. Icons are
// Microsoft's Fluent UI System Icons (src/gui/icons), recoloured to the theme and drawn as vectors
// at any display scale.

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QString>

#include "vaultik/jobs/job.hpp"

class QApplication;
class QWidget;

struct Theme {
    bool dark = false;
    QColor window;         // the app background
    QColor sidebar;        // the job list column
    QColor surface;        // cards, lists, inputs
    QColor hover;          // a row or button under the mouse
    QColor pressed;
    QColor border;         // card and list outlines
    QColor border_strong;  // input and button outlines
    QColor text;
    QColor text_muted;     // secondary text
    QColor text_faint;     // hints, disabled
    QColor accent;         // from Windows
    QColor accent_hover;
    QColor accent_text;    // text on the accent colour
    QColor accent_soft;    // selected rows
    QColor success;
    QColor warning;
    QColor danger;
    QColor neutral;
};

// The current theme (valid after apply_theme).
const Theme& theme();

// Sets the style, palette, font and style sheet on `app`, and keeps them in step with the Windows
// light/dark setting.
void apply_theme(QApplication& app);

// Emits changed() after the theme switched (light <-> dark), so windows can redraw their icons.
class ThemeNotifier : public QObject {
    Q_OBJECT

public:
    // Created as a child of the application on first use.
    static ThemeNotifier* instance();

private:
    using QObject::QObject;

signals:
    void changed();
};

// A Fluent icon by file name without ".svg" (e.g. "add_20_regular"), in `color` (default: the
// text colour; disabled: the faint colour).
QIcon icon(const QString& name, const QColor& color = QColor());

// Gives a widget a style sheet role ("primary", "flat", "muted", ...) and restyles it, which a
// widget that was already styled (a dialog button box's buttons) needs.
void set_role(QWidget* widget, const char* role);

// The coloured status icon of a run.
QIcon status_icon(vaultik::jobs::RunStatus status);
QColor status_color(vaultik::jobs::RunStatus status);
