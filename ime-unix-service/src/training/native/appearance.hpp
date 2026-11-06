#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QWidget;

namespace llavon::lora {
struct Appearance {
    // accent fills carry white text; accentText is for text and rings on surfaces;
    // control outlines inputs. Each meets WCAG 2.2 (4.5:1 text, 3:1 boundaries).
    QColor background, sidebar, surface, text, secondary, border, accent, selection, success, error, accentText, control;
};
Appearance appearance();
void applyAppearance(QWidget* window);
QIcon navigationIcon(int page);
}
