#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSvgRenderer>
#include <QTemporaryDir>

#include <memory>

namespace {

Theme g_theme;
QColor g_system_accent;

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

double luminance(const QColor& c) {
    return 0.2126 * c.redF() + 0.7152 * c.greenF() + 0.0722 * c.blueF();
}

Theme make_theme(bool dark, QColor accent) {
    Theme t;
    t.dark = dark;
    if (!accent.isValid()) accent = QColor(0x00, 0x67, 0xC0);  // Windows' default blue
    if (dark) {
        t.window = QColor(0x20, 0x20, 0x20);
        t.sidebar = QColor(0x1C, 0x1C, 0x1C);
        t.surface = QColor(0x2B, 0x2B, 0x2B);
        t.hover = QColor(0x32, 0x32, 0x32);
        t.pressed = QColor(0x3A, 0x3A, 0x3A);
        t.border = QColor(0x36, 0x36, 0x36);
        t.border_strong = QColor(0x48, 0x48, 0x48);
        t.text = QColor(0xF3, 0xF3, 0xF3);
        t.text_muted = QColor(0xC5, 0xC5, 0xC5);
        t.text_faint = QColor(0x8A, 0x8A, 0x8A);
        // Windows uses a lighter shade of the accent on dark backgrounds.
        while (luminance(accent) < 0.35) accent = mix(accent, Qt::white, 0.25);
        t.success = QColor(0x6C, 0xCB, 0x5F);
        t.warning = QColor(0xFC, 0xE1, 0x00);
        t.danger = QColor(0xFF, 0x99, 0xA4);
        t.neutral = QColor(0x9D, 0x9D, 0x9D);
    } else {
        t.window = QColor(0xF3, 0xF3, 0xF3);
        t.sidebar = QColor(0xEB, 0xEB, 0xEB);
        t.surface = QColor(0xFF, 0xFF, 0xFF);
        t.hover = QColor(0xF5, 0xF5, 0xF5);
        t.pressed = QColor(0xEB, 0xEB, 0xEB);
        t.border = QColor(0xE5, 0xE5, 0xE5);
        t.border_strong = QColor(0xC9, 0xC9, 0xC9);
        t.text = QColor(0x1B, 0x1B, 0x1B);
        t.text_muted = QColor(0x5D, 0x5D, 0x5D);
        t.text_faint = QColor(0x8E, 0x8E, 0x8E);
        while (luminance(accent) > 0.45) accent = mix(accent, Qt::black, 0.2);
        t.success = QColor(0x0F, 0x7B, 0x0F);
        t.warning = QColor(0x9D, 0x5D, 0x00);
        t.danger = QColor(0xC4, 0x2B, 0x1C);
        t.neutral = QColor(0x8A, 0x8A, 0x8A);
    }
    t.accent = accent;
    t.accent_hover = dark ? mix(accent, Qt::white, 0.12) : mix(accent, Qt::black, 0.1);
    t.accent_text = luminance(accent) > 0.55 ? QColor(Qt::black) : QColor(Qt::white);
    t.accent_soft = mix(t.surface, accent, dark ? 0.28 : 0.14);
    return t;
}

QPalette make_palette(const Theme& t) {
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.hover);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.surface);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::BrightText, t.danger);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.accent_text);
    p.setColor(QPalette::Accent, t.accent);
    p.setColor(QPalette::Link, t.accent);
    p.setColor(QPalette::LinkVisited, t.accent);
    p.setColor(QPalette::PlaceholderText, t.text_faint);
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Light, t.surface);
    p.setColor(QPalette::Midlight, t.border);
    p.setColor(QPalette::Mid, t.border_strong);
    p.setColor(QPalette::Dark, t.border_strong);
    p.setColor(QPalette::Shadow, t.dark ? Qt::black : QColor(0, 0, 0, 60));
    for (const auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, t.text_faint);
    p.setColor(QPalette::Disabled, QPalette::Highlight, t.border_strong);
    return p;
}

QString hex(const QColor& c) {
    return c.name(QColor::HexRgb);
}

QByteArray colored_svg(const QString& name, const QColor& color) {
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray svg = f.readAll();
    // The Fluent files carry no colour; give the root element one.
    svg.replace("<svg ", QByteArray("<svg fill=\"") + hex(color).toLatin1() + "\" ");
    return svg;
}

// Small recoloured icons for the style sheet (arrows of combo and spin boxes), written as files
// because style sheets take file names. Released when the application quits: Qt objects must not
// outlive QApplication (destroying them afterwards makes Qt adopt the main thread anew and leak).
std::unique_ptr<QTemporaryDir> g_sheet_icons;

QTemporaryDir& sheet_icons() {
    if (!g_sheet_icons) g_sheet_icons = std::make_unique<QTemporaryDir>();
    return *g_sheet_icons;
}

QString sheet_icon(const QString& name, const QColor& color) {
    const QString path = sheet_icons().filePath(name + "-" + hex(color).mid(1) + ".svg");
    if (!QFile::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(colored_svg(name, color));
    }
    return QDir::fromNativeSeparators(path);
}

QString style_sheet(const Theme& t) {
    const QString chevron_down = sheet_icon(QStringLiteral("chevron_down_20_regular"), t.text_muted);
    const QString chevron_up = sheet_icon(QStringLiteral("chevron_up_20_regular"), t.text_muted);
    QString s = QStringLiteral(R"(
QMainWindow, QDialog { background: @window; }
QWidget#Sidebar { background: @sidebar; }
QWidget#Content { background: @window; }
QLabel { color: @text; }
QLabel[role="title"] { font-size: 20pt; font-weight: 600; }
QLabel[role="heading"] { font-size: 13pt; font-weight: 600; }
QLabel[role="brand"] { font-size: 15pt; font-weight: 700; }
QLabel[role="muted"] { color: @muted; }
QLabel[role="faint"] { color: @faint; }
QLabel[role="section"] { color: @muted; font-weight: 600; }

QFrame[role="card"] { background: @surface; border: 1px solid @border; border-radius: 8px; }

QPushButton {
    background: @surface; color: @text; border: 1px solid @strong; border-radius: 5px;
    padding: 5px 14px; min-height: 20px;
}
QPushButton:hover { background: @hover; }
QPushButton:pressed { background: @pressed; }
QPushButton:disabled { color: @faint; border-color: @border; }
QPushButton[role="primary"] { background: @accent; color: @accenttext; border: 1px solid @accent; }
QPushButton[role="primary"]:hover { background: @accenthover; border-color: @accenthover; }
QPushButton[role="primary"]:disabled { background: @strong; border-color: @strong; color: @surface; }
QPushButton[role="flat"], QToolButton {
    background: transparent; border: none; border-radius: 5px; padding: 6px 10px; color: @text; text-align: left;
}
QPushButton[role="flat"]:hover, QToolButton:hover { background: @hover; }
QPushButton[role="flat"]:pressed, QToolButton:pressed { background: @pressed; }
QPushButton::menu-indicator, QToolButton::menu-indicator { image: none; width: 0; }

QLineEdit, QComboBox, QSpinBox, QTimeEdit, QPlainTextEdit {
    background: @surface; color: @text; border: 1px solid @strong; border-radius: 5px;
    padding: 4px 8px; selection-background-color: @accent; selection-color: @accenttext;
}
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QTimeEdit:focus, QPlainTextEdit:focus { border: 1px solid @accent; }
QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled, QTimeEdit:disabled { color: @faint; border-color: @border; }
QComboBox { padding-right: 26px; }
QComboBox::drop-down { border: none; width: 26px; subcontrol-origin: padding; subcontrol-position: center right; }
QComboBox::down-arrow { image: url(@chevrondown); width: 12px; height: 12px; }
QComboBox QAbstractItemView {
    background: @surface; color: @text; border: 1px solid @border; outline: 0; padding: 4px;
    selection-background-color: @hover; selection-color: @text;
}
QSpinBox, QTimeEdit { padding-right: 22px; }
QSpinBox::up-button, QTimeEdit::up-button {
    subcontrol-origin: border; subcontrol-position: top right; width: 20px; border: none; background: transparent;
}
QSpinBox::down-button, QTimeEdit::down-button {
    subcontrol-origin: border; subcontrol-position: bottom right; width: 20px; border: none; background: transparent;
}
QSpinBox::up-button:hover, QSpinBox::down-button:hover, QTimeEdit::up-button:hover, QTimeEdit::down-button:hover { background: @hover; }
QSpinBox::up-arrow, QTimeEdit::up-arrow { image: url(@chevronup); width: 10px; height: 10px; }
QSpinBox::down-arrow, QTimeEdit::down-arrow { image: url(@chevrondown); width: 10px; height: 10px; }

QTabWidget::pane { border: none; top: -1px; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab {
    background: transparent; color: @muted; border: none; border-bottom: 2px solid transparent;
    padding: 8px 2px; margin-right: 22px;
}
QTabBar::tab:hover { color: @text; }
QTabBar::tab:selected { color: @text; border-bottom: 2px solid @accent; }

QTreeView, QListView, QTableView {
    background: @surface; color: @text; border: 1px solid @border; border-radius: 8px; padding: 4px; outline: 0;
}
QTreeView::item, QListView::item, QTableView::item { padding: 5px 4px; border: none; }
QTreeView::item:hover, QListView::item:hover { background: @hover; }
QTreeView::item:selected, QListView::item:selected { background: @accentsoft; color: @text; }
QListView#JobList { background: transparent; border: none; padding: 0; }
QListView#JobList::item { padding: 0; }
QListView#JobList::item:hover, QListView#JobList::item:selected { background: transparent; }
QHeaderView { background: transparent; }
QHeaderView::section {
    background: @surface; color: @muted; border: none; border-bottom: 1px solid @border; padding: 6px 6px;
}

QGroupBox {
    background: @surface; border: 1px solid @border; border-radius: 8px; margin-top: 24px; padding: 14px 12px 12px 12px;
}
QGroupBox::title { subcontrol-origin: margin; left: 2px; padding: 0 4px; color: @text; font-weight: 600; }

QProgressBar { background: @border; border: none; border-radius: 3px; max-height: 6px; min-height: 6px; text-align: center; }
QProgressBar::chunk { background: @accent; border-radius: 3px; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: @strong; border-radius: 3px; min-height: 30px; min-width: 30px; }
QScrollBar::handle:hover { background: @faint; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: none; }

QMenu { background: @surface; color: @text; border: 1px solid @border; border-radius: 8px; padding: 4px; }
QMenu::item { padding: 6px 28px 6px 10px; border-radius: 4px; }
QMenu::item:selected { background: @hover; }
QMenu::item:disabled { color: @faint; }
QMenu::separator { height: 1px; background: @border; margin: 4px 8px; }
QMenu::icon { padding-left: 6px; }

QToolTip { background: @surface; color: @text; border: 1px solid @border; padding: 4px 8px; }
QStatusBar { background: @window; color: @muted; }
QStatusBar::item { border: none; }
QSplitter::handle { background: @border; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QCheckBox, QRadioButton { spacing: 8px; color: @text; }
)");
    const std::pair<const char*, QString> tokens[] = {
        {"@window", hex(t.window)},          {"@sidebar", hex(t.sidebar)},
        {"@surface", hex(t.surface)},        {"@hover", hex(t.hover)},
        {"@pressed", hex(t.pressed)},        {"@border", hex(t.border)},
        {"@strong", hex(t.border_strong)},   {"@text", hex(t.text)},
        {"@muted", hex(t.text_muted)},       {"@faint", hex(t.text_faint)},
        {"@accenttext", hex(t.accent_text)}, {"@accenthover", hex(t.accent_hover)},
        {"@accentsoft", hex(t.accent_soft)}, {"@accent", hex(t.accent)},
        {"@chevrondown", chevron_down},      {"@chevronup", chevron_up},
    };
    for (const auto& [key, value] : tokens) s.replace(QLatin1String(key), value);
    return s;
}

// Draws a recoloured SVG at the exact size asked for, so icons stay sharp at any scale.
class SvgIconEngine : public QIconEngine {
public:
    SvgIconEngine(QByteArray normal, QByteArray disabled) : normal_(std::move(normal)), disabled_(std::move(disabled)) {}

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
        QSvgRenderer renderer(mode == QIcon::Disabled ? disabled_ : normal_);
        renderer.render(painter, rect);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1.0);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        QPixmap pm(size * scale);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            paint(&p, QRect(QPoint(0, 0), pm.size()), mode, state);
        }
        pm.setDevicePixelRatio(scale);
        return pm;
    }

    QIconEngine* clone() const override { return new SvgIconEngine(normal_, disabled_); }

private:
    QByteArray normal_;
    QByteArray disabled_;
};

QHash<QString, QIcon>& icon_cache() {
    static QHash<QString, QIcon> cache;
    return cache;
}

bool system_is_dark() {
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void apply(QApplication& app) {
    g_theme = make_theme(system_is_dark(), g_system_accent);
    icon_cache().clear();
    app.setPalette(make_palette(g_theme));
    app.setStyleSheet(style_sheet(g_theme));
}

}  // namespace

const Theme& theme() {
    return g_theme;
}

ThemeNotifier* ThemeNotifier::instance() {
    // Owned by the application, so it goes away with it rather than after it.
    static ThemeNotifier* notifier = new ThemeNotifier(qApp);
    return notifier;
}

void apply_theme(QApplication& app) {
    // The Windows accent colour, read from the system palette before ours replaces it.
    g_system_accent = QApplication::palette().color(QPalette::Accent);
    if (QStyle* fusion = QStyleFactory::create(QStringLiteral("Fusion"))) app.setStyle(fusion);  // the app owns it

    QFont font = app.font();
    for (const char* family : {"Segoe UI Variable Text", "Segoe UI"}) {
        if (QFontDatabase::hasFamily(QString::fromLatin1(family))) {
            font.setFamily(QString::fromLatin1(family));
            break;
        }
    }
    font.setPointSizeF(10);
    app.setFont(font);

    apply(app);
    // Icons and the style sheet's icon folder hold Qt objects; release them while Qt is still up.
    ThemeNotifier::instance();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [] {
        icon_cache().clear();
        icon_cache().squeeze();
        g_sheet_icons.reset();
    });
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app, [&app] {
        apply(app);
        emit ThemeNotifier::instance()->changed();
    });
}

QIcon icon(const QString& name, const QColor& color) {
    const QColor c = color.isValid() ? color : g_theme.text;
    const QString key = name + hex(c);
    QHash<QString, QIcon>& cache = icon_cache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return *it;
    if (cache.size() > 500) cache.clear();
    QIcon result(new SvgIconEngine(colored_svg(name, c), colored_svg(name, g_theme.text_faint)));  // QIcon owns it
    cache.insert(key, result);
    return result;
}

QColor status_color(vaultik::jobs::RunStatus status) {
    switch (status) {
        case vaultik::jobs::RunStatus::Success: return g_theme.success;
        case vaultik::jobs::RunStatus::Partial: return g_theme.warning;
        case vaultik::jobs::RunStatus::Failed: return g_theme.danger;
        case vaultik::jobs::RunStatus::Cancelled: return g_theme.neutral;
    }
    return g_theme.neutral;
}

QIcon status_icon(vaultik::jobs::RunStatus status) {
    switch (status) {
        case vaultik::jobs::RunStatus::Success: return icon(QStringLiteral("checkmark_circle_20_filled"), g_theme.success);
        case vaultik::jobs::RunStatus::Partial: return icon(QStringLiteral("warning_20_filled"), g_theme.warning);
        case vaultik::jobs::RunStatus::Failed: return icon(QStringLiteral("dismiss_circle_20_filled"), g_theme.danger);
        case vaultik::jobs::RunStatus::Cancelled: return icon(QStringLiteral("subtract_circle_20_filled"), g_theme.neutral);
    }
    return {};
}
