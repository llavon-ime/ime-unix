#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QWidget;

namespace llavon::lora {
// The settings app's two looks: a Taiwanese keyboard (default), or a school
// exercise book, which needs a 楷書 face to look right.
enum class Theme { Keycap, Workbook };
Theme theme();
void setTheme(Theme theme);

struct Appearance {
    // accent fills carry onAccent text; accentText is for text and rings on
    // surfaces; control outlines inputs; marker is the one signature colour
    // (keycap legend red, or the teacher's red pen); edge is a keycap's lower
    // edge, or the workbook's grid line. Each meets WCAG 2.2 (4.5:1 text, 3:1
    // boundaries).
    QColor background, sidebar, surface, text, secondary, border, accent, selection, success, error, accentText, control,
        onAccent, marker, edge;
};
Appearance appearance();
void applyAppearance(QWidget* window);
QIcon navigationIcon(int page);
QIcon appIcon();
// The installed 楷書 for the workbook look, or empty when there is none.
QString kaiFamily();
// kaiFamily() while the workbook look is on, otherwise empty.
QString displayFamily();
}
