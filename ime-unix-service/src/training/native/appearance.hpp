#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

class QWidget;

namespace llavon::lora {
struct Appearance {
    QColor background, sidebar, surface, text, secondary, border, accent, selection, success, error;
};
Appearance appearance();
void applyAppearance(QWidget* window);
QIcon navigationIcon(int page);
}
