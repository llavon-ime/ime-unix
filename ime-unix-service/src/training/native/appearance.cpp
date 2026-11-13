#include "appearance.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QPainter>
#include <QPainterPath>
#include <QWidget>

static void initializeNativeResources() { Q_INIT_RESOURCE(native_appearance); }

namespace {
QColor blend(const QColor& from, const QColor& to, double amount) {
    return QColor::fromRgbF(static_cast<float>(from.redF() + (to.redF() - from.redF()) * amount),
                            static_cast<float>(from.greenF() + (to.greenF() - from.greenF()) * amount),
                            static_cast<float>(from.blueF() + (to.blueF() - from.blueF()) * amount));
}
}

namespace llavon::lora {
namespace {
Theme currentTheme = Theme::Keycap;
}
Theme theme() { return currentTheme; }
void setTheme(Theme theme) { currentTheme = theme; }

QString kaiFamily() {
    // 教育部標準楷書 first: the face of Taiwanese school textbooks; then the
    // CNS 11643 正楷體 and the 標楷體 macOS and Windows users may have.
    static const QString family = [] {
        const auto installed = QFontDatabase::families();
        for (const auto* candidate : {"TW-MOE-Std-Kai", "TW-Kai", "BiauKai", "DFKai-SB", "Kaiti TC", "AR PL UKai TW", "AR PL UKai TW MBE"})
            if (installed.contains(QString::fromLatin1(candidate))) return QString::fromLatin1(candidate);
        return QString();
    }();
    return family;
}

QString displayFamily() { return theme() == Theme::Workbook ? kaiFamily() : QString(); }

Appearance appearance() {
    const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
    if (theme() == Theme::Workbook) {
        // Light: a school exercise book, green grid on white with the
        // teacher's red pen. Dark: the green classroom blackboard and chalk.
        if (dark)
            return {QColor("#1d2a22"), QColor("#18231d"), QColor("#23332a"), QColor("#f3f3ec"),
                    QColor("#b7c6bb"), QColor("#3f5647"), QColor("#f3f3ec"), QColor("#2b3d32"), QColor("#86d49e"), QColor("#ff8f85"),
                    QColor("#f3f3ec"), QColor("#7f978a"), QColor("#1d2a22"), QColor("#ff8f85"), QColor("#4c6a57")};
        return {QColor("#ffffff"), QColor("#eef5ec"), QColor("#ffffff"), QColor("#000000"),
                QColor("#4a5a4e"), QColor("#bcd9be"), QColor("#1f6b3a"), QColor("#e5f0e2"), QColor("#1f6b3a"), QColor("#b71c1c"),
                QColor("#1f6b3a"), QColor("#7b9781"), QColor("#ffffff"), QColor("#d32f2f"), QColor("#9ccaa0")};
    }
    // A Taiwanese keyboard: white keycaps on grey plastic, black legends, and
    // the red printed 注音 legend as the only colour.
    if (dark)
        return {QColor("#1b1d20"), QColor("#232529"), QColor("#2b2e33"), QColor("#ffffff"),
                QColor("#b4b9c1"), QColor("#3d4148"), QColor("#ffffff"), QColor("#363a40"), QColor("#6fcf94"), QColor("#ff7a70"),
                QColor("#ffffff"), QColor("#81878f"), QColor("#000000"), QColor("#ff6f61"), QColor("#101113")};
    return {QColor("#ffffff"), QColor("#eceef1"), QColor("#ffffff"), QColor("#000000"),
            QColor("#50565f"), QColor("#d3d7dd"), QColor("#000000"), QColor("#e3e6ea"), QColor("#1e7a46"), QColor("#c62828"),
            QColor("#000000"), QColor("#868c95"), QColor("#ffffff"), QColor("#c62828"), QColor("#b5bac2")};
}

QIcon appIcon() {
    initializeNativeResources();
    return QIcon(QStringLiteral(":/native/AppIcon.png"));
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
    palette.setColor(QPalette::Highlight, c.selection);
    palette.setColor(QPalette::HighlightedText, c.text);
    palette.setColor(QPalette::Disabled, QPalette::Text, c.secondary);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, c.secondary);
    palette.setColor(QPalette::PlaceholderText, c.secondary);
    window->setPalette(palette);
    QString css = QStringLiteral(R"(
        QWidget { color: $text; font-size: 13px; font-weight: 400; }
        QMainWindow, QScrollArea, QStackedWidget, QWidget#settingsForm, QWidget#trainingContent { background: $background; }
        QDialog#usageGuide, QDialog#setupWizard { background: $background; }
        QFrame#setupPresetCard { background: $surface; border: 1px solid $border; border-radius: 9px; }
        QFrame#setupPresetCard[selected="true"] { border: 2px solid $accent; }
        QFrame#setupPresetCard:hover { border-color: $secondary; }
        QRadioButton { spacing: 8px; background: transparent; }
        QRadioButton::indicator { width: 16px; height: 16px; border-radius: 9px; border: 1px solid $control; background: $surface; }
        QRadioButton::indicator:hover { border-color: $secondary; }
        QRadioButton::indicator:checked { border: 5px solid $accent; width: 8px; height: 8px; background: $onAccent; }
        QRadioButton:focus { color: $accentText; }
        QWidget#sidebar { background: $sidebar; border-right: 1px solid $border; }
        QLabel[role="brand"] { font-size: 22px; font-weight: 800; letter-spacing: 2px; }
        QLabel[role="brandTag"] { font-size: 11px; color: $secondary; letter-spacing: 1px; }
        QLabel[role="title"] { font-size: 26px; font-weight: 800; }
        QLabel[role="guideAnnotation"] { color: $accentText; font-size: 13px; font-weight: 500; }
        QLabel[role="guideExample"] { font-size: 19px; font-weight: 500; }
        QLabel[role="guideTopicTitle"] { font-size: 22px; font-weight: 800; }
        QLabel[role="guideKey"] { background: $surface; border: 1px solid $border; border-bottom-width: 2px; border-radius: 6px; padding: 6px 10px; font-weight: 500; }
        QFrame#guideShortcutRow, QFrame#guideHelpRow { border: none; border-bottom: 1px solid $border; }
        QLabel[role="section"] { font-size: 14px; font-weight: 700; }
        QLabel[role="muted"], QLabel[role="eyebrow"] { color: $secondary; }
        QLabel[role="eyebrow"] { font-size: 11px; }
        QLabel#notice { color: $secondary; font-size: 12px; padding: 4px 10px; }
        QLabel#notice[tone="error"] { color: $error; background: $dangerSurface; border-radius: 7px; }
        QPushButton#openGuide { text-align: left; padding-left: 10px; }
        QListWidget#navigation { background: transparent; border: none; outline: none; }
        QListWidget#navigation::item { padding: 8px; border-radius: 6px; }
        QListWidget#navigation::item:hover { background: $selection; }
        QListWidget#navigation::item:selected { background: $selection; color: $text; font-weight: 700; }
        QListWidget#navigation::item:disabled, QListWidget#navigation::item:disabled:hover { background: transparent; color: $secondary; padding: 14px 8px 2px 8px; }
        QPushButton { padding: 4px 14px; min-height: 24px; border: 1px solid $border; border-radius: 7px; background: $surface; }
        QPushButton:hover { background: $hover; border-color: $secondary; }
        QPushButton:pressed { background: $selection; border-color: $accent; }
        QPushButton[primary="true"] { background: $accent; border-color: $accent; color: $onAccent; font-weight: 600; }
        QPushButton[primary="true"]:hover { background: $accentHover; border-color: $accentHover; }
        QPushButton[primary="true"]:pressed { background: $accentPressed; border-color: $accentPressed; }
        QPushButton[quiet="true"] { background: transparent; border-color: transparent; color: $secondary; }
        QPushButton[quiet="true"]:hover { color: $accentText; background: $selection; }
        QPushButton[destructive="true"]:hover, QPushButton[destructive="true"]:pressed { color: $error; background: $dangerSurface; border-color: $error; }
        QPushButton:focus { border: 2px solid $focusRing; padding: 3px 13px; }
        QPushButton[primary="true"]:focus { border: 2px solid $focusBorder; }
        QPushButton[quiet="true"]:focus { color: $accentText; border: 2px solid $focusRing; background: $selection; }
        QPushButton:disabled, QPushButton[primary="true"]:disabled, QPushButton[quiet="true"]:disabled, QPushButton[destructive="true"]:disabled { color: $disabledText; background: $sidebar; border: 1px solid $border; padding: 4px 14px; }
        QPushButton[quiet="true"]:disabled { background: transparent; border-color: transparent; }
        QPushButton[disclosure="true"] { text-align: left; padding-left: 4px; color: $text; font-weight: 600; }
        QPushButton[segment="true"] { padding: 4px 6px; font-size: 12px; }
        QPushButton[segment="true"]:checked { background: $selection; border-color: $control; font-weight: 700; }
        QPushButton[compact="true"] { padding: 4px 0px; }
        QPushButton[compact="true"]:focus { padding: 3px 0px; }
        QLineEdit, QComboBox, QSpinBox { padding: 4px 10px; min-height: 24px; border: 1px solid $control; border-radius: 7px; background: $surface; color: $text; selection-background-color: $selection; selection-color: $text; }
        QLineEdit:hover, QComboBox:hover, QSpinBox:hover { border-color: $secondary; }
        QComboBox { padding-right: 32px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; border: none; width: 28px; }
        QComboBox:on { border-color: $focusRing; background: $selection; }
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
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid $focusRing; padding: 3px 9px; }
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
        QAbstractScrollArea::corner { background: transparent; border: none; }
        QTabWidget::pane { background: $background; border: none; }
        QTabWidget::tab-bar { alignment: left; }
        QTabBar::tab { background: $sidebar; padding: 8px 16px; margin: 4px 2px 14px 0; border: 1px solid transparent; border-radius: 6px; color: $secondary; }
        QTabBar::tab:selected { background: $surface; border-color: $border; color: $text; font-weight: 500; }
        QTabBar::tab:hover { color: $accentText; }
        QFrame#settingsCard, QFrame#phraseRow { background: $surface; border: 1px solid $border; border-radius: 9px; }
        QFrame#settingRow, QFrame#resourceRow { background: transparent; border: none; }
        QFrame#rowDivider { background: $border; border: none; }
        QLabel[role="help"] { color: $secondary; font-size: 12px; background: transparent; }
        QLabel#phraseNumber { color: $secondary; font-size: 11px; }
        QLabel[validation="valid"] { color: $success; font-size: 12px; }
        QLabel[validation="invalid"] { color: $error; font-size: 12px; }
        QTableWidget { background: $surface; border: 1px solid $border; border-radius: 8px; selection-background-color: $selection; selection-color: $text; }
        QTableWidget::item { padding-left: 8px; border-bottom: 1px solid $border; }
        QHeaderView::section { background: $background; border: none; border-bottom: 1px solid $border; padding: 10px 11px; color: $secondary; font-size: 11px; }
        QGroupBox { border: 1px solid $border; border-radius: 9px; margin-top: 12px; padding: 16px 12px 12px; background: $surface; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }
        QPlainTextEdit { border: 1px solid $border; border-radius: 8px; background: $surface; padding: 10px; font-size: 12px; }
        QProgressBar { border: none; background: $border; border-radius: 3px; max-height: 6px; }
        QProgressBar::chunk { background: $accent; border-radius: 3px; }
        QGraphicsView#history { border: 1px solid $border; border-radius: 9px; background: $background; }
        QSplitter::handle { background: $border; }
    )");
    if (theme() == Theme::Keycap) css += QStringLiteral(R"(
        QListWidget#navigation::item { background: $surface; border: 1px solid $border; border-bottom: 3px solid $edge; border-radius: 7px; padding: 6px 8px; margin: 3px 2px; }
        QListWidget#navigation::item:hover { border-color: $control; border-bottom-color: $edge; }
        QListWidget#navigation::item:selected { background: $selection; color: $text; font-weight: 700; border: 1px solid $control; margin: 5px 2px 1px 2px; }
        QListWidget#navigation::item:disabled, QListWidget#navigation::item:disabled:hover { background: transparent; border: none; margin: 0px; color: $secondary; padding: 14px 8px 2px 8px; }
        QPushButton { border: 1px solid $border; border-bottom: 3px solid $edge; padding: 4px 14px 2px 14px; }
        QPushButton:hover { background: $surface; border-color: $control; border-bottom-color: $edge; }
        QPushButton:pressed { background: $selection; border-bottom: 1px solid $control; padding: 6px 14px 2px 14px; }
        QPushButton[primary="true"] { background: $accent; border: 1px solid $accent; border-bottom: 3px solid $primaryEdge; color: $onAccent; }
        QPushButton[primary="true"]:hover { background: $accentHover; border-color: $accentHover; border-bottom-color: $primaryEdge; }
        QPushButton[primary="true"]:pressed { border-bottom: 1px solid $primaryEdge; padding: 6px 14px 2px 14px; }
        QPushButton[quiet="true"] { border: 1px solid transparent; padding: 4px 14px; }
        QPushButton[quiet="true"]:pressed { padding: 4px 14px; }
        QPushButton[disclosure="true"] { padding-left: 4px; }
        QPushButton:focus { border: 2px solid $focusRing; border-bottom: 3px solid $focusRing; padding: 3px 13px 2px 13px; }
        QPushButton[quiet="true"]:focus { border: 2px solid $focusRing; padding: 3px 13px; }
        QFrame#settingsCard, QFrame#phraseRow { border-radius: 8px; }
        QFrame#setupPresetCard { border-bottom: 3px solid $edge; border-radius: 8px; }
        QFrame#setupPresetCard[selected="true"] { border: 1px solid $control; background: $selection; }
        QLabel[role="guideKey"] { border-bottom: 3px solid $edge; }
        QPushButton[segment="true"] { padding: 4px 6px 2px 6px; }
        QPushButton[segment="true"]:checked { background: $selection; border: 1px solid $control; padding: 6px 6px 2px 6px; }
    )");
    else css += QStringLiteral(R"(
        QWidget#sidebar { border-right: 2px solid $edge; }
        QLabel[role="brand"] { font-family: "$kai"; font-size: 28px; font-weight: 400; letter-spacing: 0px; }
        QLabel[role="title"], QLabel[role="guideTopicTitle"], QLabel[role="section"] { font-family: "$kai"; font-weight: 400; }
        QLabel[role="title"] { font-size: 30px; }
        QLabel[role="guideTopicTitle"] { font-size: 27px; }
        QLabel[role="section"] { font-size: 18px; color: $accentText; }
        QListWidget#navigation::item { padding: 7px 10px; border: 2px solid transparent; border-radius: 17px; margin: 1px 0px; }
        QListWidget#navigation::item:hover { background: $selection; }
        QListWidget#navigation::item:selected { background: transparent; border: 2px solid $marker; color: $text; font-weight: 700; }
        QListWidget#navigation::item:disabled, QListWidget#navigation::item:disabled:hover { background: transparent; border: none; color: $secondary; padding: 14px 8px 2px 8px; }
        QPushButton { border-radius: 4px; }
        QFrame#settingsCard, QFrame#phraseRow { border: 1px solid $border; border-radius: 2px; }
        QFrame#rowDivider { background: transparent; border: none; border-top: 1px dashed $border; }
        QFrame#setupPresetCard { border-radius: 2px; }
        QFrame#setupPresetCard[selected="true"] { border: 2px solid $marker; }
        QRadioButton::indicator:checked { border: 5px solid $marker; width: 8px; height: 8px; background: $surface; }
        QLineEdit, QComboBox, QSpinBox { border-radius: 3px; }
        QComboBox QAbstractItemView { border-radius: 3px; }
        QTableWidget, QPlainTextEdit, QGraphicsView#history { border-radius: 2px; }
        QPushButton[segment="true"]:checked { background: $surface; border: 2px solid $marker; padding: 3px 5px; }
    )");
    const QList<QPair<QString, QColor>> tokens{{"background", c.background}, {"sidebar", c.sidebar}, {"surface", c.surface},
        {"text", c.text}, {"secondary", c.secondary}, {"border", c.border}, {"accent", c.accent},
        {"selection", c.selection}, {"success", c.success}, {"error", c.error},
        {"accentHover", blend(c.accent, c.background, 0.18)}, {"accentPressed", blend(c.accent, c.background, 0.32)},
        {"onAccent", c.onAccent}, {"marker", c.marker}, {"edge", c.edge}, {"primaryEdge", blend(c.accent, c.background, 0.42)},
        {"accentText", c.accentText}, {"control", c.control},
        {"focusRing", c.accent},
        {"hover", c.background}, {"disabledText", c.secondary},
        {"focusBorder", c.marker},
        {"scrollThumb", c.background.lightness() < 128 ? c.secondary.darker(130) : c.border.darker(115)},
        {"dangerSurface", c.background.lightness() < 128 ? QColor("#422b2d") : QColor("#fff0ef")}};
    // Replace derived tokens before their shorter prefixes (accentHover/accent).
    for (auto it = tokens.crbegin(); it != tokens.crend(); ++it) css.replace('$' + it->first, it->second.name());
    css.replace("$variant", c.background.lightness() < 128 ? "dark" : "light");
    if (const auto family = displayFamily(); family.isEmpty()) css.replace(QStringLiteral("font-family: \"$kai\";"), QString());
    else css.replace(QStringLiteral("$kai"), family);
    window->setStyleSheet(css);
}

QIcon navigationIcon(int page) {
    // Line glyphs in the text colour, like the line-art icon. Our own
    // drawings, not SF Symbols.
    QIcon icon;
    const auto draw = [page](QColor color, int scale) {
        QPixmap pixmap(26 * scale, 26 * scale); pixmap.setDevicePixelRatio(scale); pixmap.fill(Qt::transparent);
        QPainter p(&pixmap); p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(Qt::NoBrush); p.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (page == 3) {
            for (const int y : {8, 13, 18}) p.drawLine(5, y, 21, y);
            p.setBrush(color);
            p.drawEllipse(QPointF(10, 8), 2.4, 2.4); p.drawEllipse(QPointF(17, 13), 2.4, 2.4); p.drawEllipse(QPointF(8, 18), 2.4, 2.4);
        } else if (page == 4) {
            p.drawRoundedRect(QRectF(4, 5, 18, 13), 3, 3); p.drawLine(8, 18, 8, 22); p.drawLine(8, 22, 12, 18);
            p.drawLine(8, 10, 18, 10); p.drawLine(8, 14, 14, 14);
        } else if (page == 0) {
            p.drawRoundedRect(QRectF(6, 3.5, 14, 19), 2, 2);
            for (const int y : {9, 13, 17}) p.drawLine(10, y, 16, y);
        } else if (page == 1) {
            p.drawLine(6, 21, 6, 14); p.drawLine(13, 21, 13, 9); p.drawLine(20, 21, 20, 5);
        } else if (page == 2) {
            p.drawLine(13, 8, 13, 12); p.drawLine(7, 12, 19, 12); p.drawLine(7, 12, 7, 17); p.drawLine(19, 12, 19, 17);
            p.drawEllipse(QPointF(13, 5.5), 2.6, 2.6); p.drawEllipse(QPointF(7, 20), 2.6, 2.6); p.drawEllipse(QPointF(19, 20), 2.6, 2.6);
        } else if (page == 6) {
            p.drawEllipse(QPointF(13, 13), 8.5, 8.5); p.drawPoint(QPointF(13, 8.5)); p.drawLine(13, 12, 13, 18);
        } else {
            p.drawArc(QRectF(5, 5, 16, 16), 45 * 16, 285 * 16); p.drawLine(20, 4, 20, 10); p.drawLine(20, 10, 14, 10);
        }
        p.end();
        return pixmap;
    };
    for (const int scale : {1, 2}) {
        icon.addPixmap(draw(appearance().text, scale));
        icon.addPixmap(draw(appearance().text, scale), QIcon::Selected);
    }
    return icon;
}
}
