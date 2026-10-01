#pragma once
#include <QCheckBox>
#include <QComboBox>

namespace llavon::lora {
// Keep standard Qt keyboard/accessibility behavior, with one desktop popup
// presentation on Cocoa and xcb. Scrolling the page never changes a choice.
class NativeComboBox final : public QComboBox {
public:
    explicit NativeComboBox(QWidget* parent = nullptr);
    ~NativeComboBox() override;
    void showPopup() override;
protected:
    void wheelEvent(QWheelEvent* event) override;
};

class NativeCheckBox final : public QCheckBox {
public:
    explicit NativeCheckBox(const QString& text, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent* event) override;
};
}
