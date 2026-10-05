#pragma once

#include <QPixmap>
#include <QWidget>
#include <memory>

class QLabel;

namespace llavon::lora {

// Render the real widgets, but never expose their editing/consent actions.
// The example has its own lifetime and is not part of the settings workspace.
class GuideIllustration final : public QWidget {
    Q_OBJECT
public:
    enum class Kind { MixedInput, Phrases, Personalization };
    explicit GuideIllustration(Kind kind, QWidget* parent = nullptr);
    ~GuideIllustration() override;
    QList<QRect> targetRects() const { return targets_; }
protected:
    void paintEvent(QPaintEvent* event) override;
    bool event(QEvent* event) override;
private:
    void rebuild();
    std::unique_ptr<QWidget> source_;
    std::unique_ptr<QWidget> excerptOwner_;
    QWidget* excerpt_ = nullptr;
    QWidget* secondary_ = nullptr;
    QList<QWidget*> marked_;
    QList<QRect> targets_;
    QLabel *first_, *second_;
    QPixmap image_;
    QRect imageRect_;
    bool dirty_ = true;
};
} // namespace llavon::lora
