#include "appearance.hpp"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>

static void initializeNativeResources() { Q_INIT_RESOURCE(native_appearance); }

namespace llavon::lora {
Appearance appearance() {
    if (QApplication::palette().color(QPalette::Window).lightness() < 128)
        return {QColor("#202124"), QColor("#292a2e"), QColor("#303136"), QColor("#f5f5f7"),
                QColor("#b0b1b8"), QColor("#46474e"), QColor("#409cff"), QColor("#263e5a"), QColor("#68ce91"), QColor("#ff827b")};
    return {QColor("#f5f5f7"), QColor("#ececef"), QColor("#ffffff"), QColor("#1d1d1f"),
            QColor("#6e6e73"), QColor("#dcdce0"), QColor("#007aff"), QColor("#e8f2ff"), QColor("#287a48"), QColor("#c9342c")};
}

void applyAppearance(QWidget* window) {
    initializeNativeResources();
    const auto c = appearance();
    auto palette = QApplication::palette();
    palette.setColor(QPalette::Window, c.background);
    palette.setColor(QPalette::WindowText, c.text);
    palette.setColor(QPalette::Base, c.surface);
    palette.setColor(QPalette::Text, c.text);
    palette.setColor(QPalette::Button, c.surface);
    palette.setColor(QPalette::ButtonText, c.text);
    palette.setColor(QPalette::Highlight, c.accent);
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text, c.secondary);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.secondary);
    palette.setColor(QPalette::PlaceholderText, c.secondary);
    window->setPalette(palette);
    QString css = QStringLiteral(R"(
        QWidget { color: $text; font-size: 13px; font-weight: 400; }
        QMainWindow, QScrollArea, QStackedWidget, QWidget#settingsForm, QWidget#trainingContent { background: $background; }
        QDialog#usageGuide { background: $background; }
        QWidget#sidebar { background: $sidebar; border-right: 1px solid $border; }
        QLabel[role="brand"] { font-size: 20px; font-weight: 600; }
        QLabel[role="title"] { font-size: 24px; font-weight: 600; }
        QLabel[role="guideAnnotation"] { color: $accent; font-size: 13px; font-weight: 500; }
        QLabel[role="guideExample"] { font-size: 19px; font-weight: 500; }
        QLabel[role="guideTopicTitle"] { font-size: 20px; font-weight: 600; }
        QLabel[role="guideKey"] { background: $surface; border: 1px solid $border; border-bottom-width: 2px; border-radius: 6px; padding: 6px 10px; font-weight: 500; }
        QFrame#guideShortcutRow, QFrame#guideHelpRow { border: none; border-bottom: 1px solid $border; }
        QLabel[role="section"] { font-size: 14px; font-weight: 600; }
        QLabel[role="muted"], QLabel[role="eyebrow"] { color: $secondary; }
        QLabel[role="eyebrow"] { font-size: 11px; }
        QLabel#notice { color: $secondary; font-size: 11px; }
        QListWidget#navigation { background: transparent; border: none; outline: none; }
        QListWidget#navigation::item { padding: 8px; border-radius: 7px; }
        QListWidget#navigation::item:hover { background: $surface; }
        QListWidget#navigation::item:selected { background: $accent; color: white; font-weight: 500; }
        QPushButton { padding: 4px 14px; min-height: 24px; border: 1px solid $border; border-radius: 7px; background: $surface; }
        QPushButton:hover { background: $hover; border-color: $secondary; }
        QPushButton:pressed { background: $selection; border-color: $accent; }
        QPushButton[primary="true"] { background: $accent; border-color: $accent; color: white; font-weight: 500; }
        QPushButton[primary="true"]:hover { background: $accentHover; border-color: $accentHover; }
        QPushButton[primary="true"]:pressed { background: $accentPressed; border-color: $accentPressed; }
        QPushButton[quiet="true"] { background: transparent; border-color: transparent; color: $secondary; }
        QPushButton[quiet="true"]:hover { color: $accent; background: $selection; }
        QPushButton[destructive="true"]:hover, QPushButton[destructive="true"]:pressed { color: $error; background: $dangerSurface; border-color: $error; }
        QPushButton:focus { border: 2px solid $accent; padding: 3px 13px; }
        QPushButton[primary="true"]:focus { border: 2px solid $focusBorder; }
        QPushButton[quiet="true"]:focus { color: $accent; border: 2px solid $accent; background: $selection; }
        QPushButton:disabled, QPushButton[primary="true"]:disabled, QPushButton[quiet="true"]:disabled, QPushButton[destructive="true"]:disabled { color: $disabledText; background: $sidebar; border: 1px solid $border; padding: 4px 14px; }
        QPushButton[quiet="true"]:disabled { background: transparent; border-color: transparent; }
        QPushButton[compact="true"] { padding: 4px 0px; }
        QPushButton[compact="true"]:focus { padding: 3px 0px; }
        QLineEdit, QComboBox, QSpinBox { padding: 4px 10px; min-height: 24px; border: 1px solid $border; border-radius: 7px; background: $surface; color: $text; selection-background-color: $accent; selection-color: white; }
        QLineEdit:hover, QComboBox:hover, QSpinBox:hover { border-color: $secondary; }
        QComboBox { padding-right: 32px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; border: none; width: 28px; }
        QComboBox:on { border-color: $accent; background: $selection; }
        QComboBox::down-arrow, QSpinBox::down-arrow { image: url(:/native/chevron-$variant.xpm); width: 9px; height: 5px; }
        QComboBox QAbstractItemView { background: $surface; color: $text; border: 1px solid $border; border-radius: 7px; padding: 4px; outline: none; selection-background-color: $selection; selection-color: $text; }
        QComboBox QAbstractItemView::item { padding: 4px 10px; border: none; border-radius: 4px; }
        QComboBox QAbstractItemView::item:selected { background: $selection; color: $text; }
        QComboBox QAbstractItemView::item:disabled { color: $disabledText; }
        QSpinBox { padding-right: 30px; }
        QSpinBox::up-arrow { image: url(:/native/up-$variant.xpm); width: 9px; height: 5px; }
        QSpinBox::up-button { subcontrol-origin: border; subcontrol-position: top right; width: 26px; border: none; border-top-right-radius: 7px; }
        QSpinBox::down-button { subcontrol-origin: border; subcontrol-position: bottom right; width: 26px; border: none; border-bottom-right-radius: 7px; }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover { background: $selection; }
        QSpinBox::up-button:pressed, QSpinBox::down-button:pressed { background: $border; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid $accent; padding: 3px 9px; }
        QComboBox:focus { padding-right: 31px; }
        QSpinBox:focus { padding-right: 29px; }
        QLineEdit:disabled, QComboBox:disabled, QSpinBox:disabled { background: $sidebar; color: $disabledText; border-color: $border; }
        QCheckBox { spacing: 8px; background: transparent; }
        QCheckBox:disabled { color: $disabledText; }
        QCheckBox::indicator { width: 16px; height: 16px; }
        QScrollBar:vertical { width: 10px; margin: 2px 0px; background: transparent; border: none; }
        QScrollBar:horizontal { height: 10px; margin: 0px 2px; background: transparent; border: none; }
        QScrollBar::handle:vertical { background: $scrollThumb; min-height: 24px; border-radius: 4px; }
        QScrollBar::handle:horizontal { background: $scrollThumb; min-width: 24px; border-radius: 4px; }
        QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: $secondary; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0px; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical, QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
        QTabWidget::pane { background: $background; border: none; }
        QTabBar::tab { background: $sidebar; padding: 8px 16px; margin: 4px 2px 14px 0; border: 1px solid transparent; border-radius: 6px; color: $secondary; }
        QTabBar::tab:selected { background: $surface; border-color: $border; color: $text; font-weight: 500; }
        QTabBar::tab:hover { color: $accent; }
        QFrame#settingRow, QFrame#phraseRow, QFrame#resourceRow, QFrame#quickStart { background: $surface; border: 1px solid $border; border-radius: 9px; }
        QLabel#phraseNumber { color: $secondary; font-size: 11px; }
        QLabel[validation="valid"] { color: $success; font-size: 12px; }
        QLabel[validation="invalid"] { color: $error; font-size: 12px; }
        QTableWidget { background: $surface; alternate-background-color: $background; border: 1px solid $border; border-radius: 8px; selection-background-color: $selection; selection-color: $text; }
        QHeaderView::section { background: $background; border: none; border-bottom: 1px solid $border; padding: 10px; color: $secondary; font-size: 11px; }
        QGroupBox { border: 1px solid $border; border-radius: 9px; margin-top: 12px; padding: 16px 12px 12px; background: $surface; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
        QPlainTextEdit { border: 1px solid $border; border-radius: 8px; background: $surface; padding: 10px; font-size: 12px; }
        QProgressBar { border: none; background: $border; border-radius: 3px; max-height: 6px; }
        QProgressBar::chunk { background: $accent; border-radius: 3px; }
        QGraphicsView#history { border: 1px solid $border; border-radius: 9px; background: $background; }
        QSplitter::handle { background: $border; }
    )");
    const QList<QPair<QString, QColor>> tokens{{"background", c.background}, {"sidebar", c.sidebar}, {"surface", c.surface},
        {"text", c.text}, {"secondary", c.secondary}, {"border", c.border}, {"accent", c.accent},
        {"selection", c.selection}, {"success", c.success}, {"error", c.error},
        {"accentHover", c.accent.lighter(110)}, {"accentPressed", c.accent.darker(115)},
        {"hover", c.background}, {"disabledText", c.secondary},
        {"focusBorder", c.background.lightness() < 128 ? QColor("#b5d7ff") : c.accent.darker(145)},
        {"scrollThumb", c.background.lightness() < 128 ? c.secondary.darker(130) : c.border.darker(115)},
        {"dangerSurface", c.background.lightness() < 128 ? QColor("#422b2d") : QColor("#fff0ef")}};
    // Replace derived tokens before their shorter prefixes (accentHover/accent).
    for (auto it = tokens.crbegin(); it != tokens.crend(); ++it) css.replace('$' + it->first, it->second.name());
    css.replace("$variant", c.background.lightness() < 128 ? "dark" : "light");
    window->setStyleSheet(css);
}

QIcon navigationIcon(int page) {
    // Small, consistent vector symbols, rendered locally at both desktop scales.
    // These are our own icons, not platform-private SF Symbols glyphs.
    QIcon icon;
    const QList<QColor> colors{QColor("#8e8e93"), QColor("#af52de"), QColor("#007aff"),
        QColor("#ff9500"), QColor("#34a86b"), QColor("#5856d6"), QColor("#8e8e93")};
    for (const int scale : {1, 2}) {
        QPixmap pixmap(26 * scale, 26 * scale); pixmap.setDevicePixelRatio(scale); pixmap.fill(Qt::transparent);
        QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen); p.setBrush(colors.value(page)); p.drawRoundedRect(QRectF(0, 0, 26, 26), 6, 6);
        p.setBrush(Qt::NoBrush); p.setPen(QPen(Qt::white, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (page == 3) {
            for (const int y : {8, 13, 18}) p.drawLine(6, y, 20, y);
            p.setBrush(colors.value(page)); p.drawEllipse(QPointF(10, 8), 2, 2); p.drawEllipse(QPointF(16, 13), 2, 2); p.drawEllipse(QPointF(11, 18), 2, 2);
        } else if (page == 4) {
            p.drawRoundedRect(QRectF(5, 5, 16, 13), 3, 3); p.drawLine(8, 18, 8, 22); p.drawLine(8, 22, 12, 18);
            p.drawLine(9, 9, 17, 9); p.drawLine(9, 13, 14, 13);
        } else if (page == 0) {
            p.drawRoundedRect(QRectF(6, 4, 14, 18), 2, 2);
            for (const int y : {9, 13, 17}) p.drawLine(10, y, 16, y);
        } else if (page == 1) {
            p.drawLine(6, 20, 6, 14); p.drawLine(13, 20, 13, 9); p.drawLine(20, 20, 20, 5);
        } else if (page == 2) {
            p.drawLine(13, 8, 13, 12); p.drawLine(7, 12, 19, 12); p.drawLine(7, 12, 7, 18); p.drawLine(19, 12, 19, 18);
            p.drawEllipse(QPointF(13, 5), 3, 3); p.drawEllipse(QPointF(7, 21), 3, 3); p.drawEllipse(QPointF(19, 21), 3, 3);
        } else if (page == 6) {
            p.drawEllipse(QPointF(13, 13), 8, 8); p.drawPoint(13, 8); p.drawLine(13, 12, 13, 18);
        } else {
            p.drawArc(QRectF(5, 5, 16, 16), 45 * 16, 285 * 16); p.drawLine(20, 5, 20, 10); p.drawLine(20, 10, 15, 10);
        }
        p.end(); icon.addPixmap(pixmap);
    }
    return icon;
}
}
