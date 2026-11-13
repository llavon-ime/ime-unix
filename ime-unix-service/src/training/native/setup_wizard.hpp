#pragma once

#include <QDialog>
#include <QWidget>

class QAbstractButton;
class QButtonGroup;
class QCheckBox;
class QLabel;
class QPushButton;
class QStackedWidget;

namespace llavon::lora {

class SettingsPage;

// The main keys of a keyboard with the 注音 symbol each one types.
class KeyboardPreview final : public QWidget {
    Q_OBJECT
public:
    explicit KeyboardPreview(QWidget* parent = nullptr);
    void setLayoutName(const QString& name);
    // The symbol a key types first in this layout, or empty.
    QString symbol(QChar key) const;
    QSize sizeHint() const override { return {560, 236}; }
    QSize minimumSizeHint() const override { return {420, 180}; }
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    QString layout_ = QStringLiteral("standard");
};

// First-run setup: the three choices that change everyday typing. Every
// choice is written through the settings page at once, like the page itself.
class SetupWizard final : public QDialog {
    Q_OBJECT
public:
    explicit SetupWizard(SettingsPage* settings, QWidget* parent = nullptr);
    // A first launch: setup not seen yet and no settings saved before.
    static bool shouldOffer();
    void restart();
signals:
    void pageRequested(const QString& page);
public slots:
    void done(int result) override;
protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void sync();
    void updateStep();
    void describeLayout(const QString& layout);
    SettingsPage* settings_;
    QStackedWidget* steps_;
    QLabel* progress_;
    QWidget* stepBar_;
    QLabel* layoutNote_ = nullptr;
    QLabel* presetNote_ = nullptr;
    QPushButton *previous_, *next_, *skip_;
    QButtonGroup *layouts_, *presets_;
    QCheckBox* smart_ = nullptr;
    KeyboardPreview* preview_ = nullptr;
};

} // namespace llavon::lora
