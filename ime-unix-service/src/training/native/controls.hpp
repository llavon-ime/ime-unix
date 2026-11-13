#pragma once
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>

namespace llavon::lora {
// Shared by the real data page and its read-only guide illustration.
QWidget* recordingCredentials(QWidget* parent = nullptr);

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

// An on/off setting that applies to its whole row; still a QCheckBox for
// keyboard and accessibility behavior.
class NativeSwitch final : public QCheckBox {
public:
    explicit NativeSwitch(QWidget* parent = nullptr);
    QSize sizeHint() const override { return {40, 24}; }
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent* event) override;
    bool hitButton(const QPoint& position) const override { return rect().contains(position); }
};

// The page heading. In the workbook look each character sits in its own
// 田字格 square, as when practising characters in a Taiwanese exercise book.
class PageTitle final : public QLabel {
public:
    explicit PageTitle(QWidget* parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    int cell() const;
};

class NativeCheckBox final : public QCheckBox {
public:
    explicit NativeCheckBox(const QString& text, QWidget* parent = nullptr);
protected:
    void paintEvent(QPaintEvent* event) override;
};
}
