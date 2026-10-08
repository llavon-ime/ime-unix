#include "guide_illustration.hpp"
#include "appearance.hpp"
#include "controls.hpp"
#include "phrase_list.hpp"
#include "settings.hpp"

#include <QComboBox>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>

namespace llavon::lora {
namespace {
QLabel* caption(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    label->setProperty("role", "guideAnnotation");
    return label;
}
void prepare(QWidget* widget, int width) {
    widget->ensurePolished();
    widget->resize(width, widget->sizeHint().height());
    if (widget->layout()) widget->layout()->activate();
    for (auto* child : widget->findChildren<QWidget*>()) if (child->layout()) child->layout()->activate();
}
}

GuideIllustration::GuideIllustration(Kind kind, QWidget* parent) : QWidget(parent) {
    setObjectName(kind == Kind::MixedInput ? "guideMixedIllustration" :
        kind == Kind::Phrases ? "guidePhraseIllustration" : "guidePrivacyIllustration");
    setAccessibleName(QStringLiteral("唯讀介面示意，箭頭標示功能位置"));
    setProperty("illustrationOnly", true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Captions are regular accessible labels; color and arrows are not the only explanation.
    first_ = caption({}, this); second_ = caption({}, this);
    if (kind == Kind::MixedInput) {
        auto page = std::make_unique<SettingsPage>(false);
        page->revealField("smart_english");
        auto* field = page->findChild<QComboBox*>("setting_smart_english");
        field->setCurrentIndex(field->findData(true));
        excerpt_ = field->parentWidget();
        secondary_ = page->findChild<QPushButton*>("saveSettings");
        marked_ = {field, secondary_};
        source_ = std::move(page);
        first_->setText(QStringLiteral("① 開啟智慧中英混輸"));
        second_->setText(QStringLiteral("② 儲存並套用"));
        setMinimumHeight(218);
    } else if (kind == Kind::Phrases) {
        auto list = std::make_unique<PhraseList>(QString());
        list->loadText(QStringLiteral("李拉風 ㄌㄧˇ-ㄌㄚ-ㄈㄥ\n"));
        excerpt_ = list->findChild<QFrame*>("phraseRow");
        marked_ = {excerpt_->findChild<QLineEdit*>("phraseWord"), excerpt_->findChild<QComboBox*>("phraseReading_1")};
        source_ = std::move(list);
        first_->setText(QStringLiteral("① 輸入名字"));
        second_->setText(QStringLiteral("② 逐字確認讀音"));
        setMinimumHeight(258);
    } else {
        source_.reset(recordingCredentials()); excerpt_ = source_.get();
        for (const auto* name : {"unlock", "lock", "recording"}) source_->findChild<QPushButton*>(name)->hide();
        marked_ = {source_->findChild<QLineEdit*>("reviewPassword"), source_->findChild<QPushButton*>("setup")};
        first_->setText(QStringLiteral("① 設定資料密碼"));
        second_->setText(QStringLiteral("② 自行啟用收集"));
        setMinimumHeight(196);
    }
    // These widgets are rendered as an image, not interactive copies. In
    // particular a click on the illustrated consent button cannot collect data.
    source_->setAttribute(Qt::WA_DontShowOnScreen);
    if (excerpt_ != source_.get()) {
        // A hidden ancestor's layout must not squeeze this excerpt back down
        // to its own minimum width while QWidget::render polishes the tree.
        excerpt_->setParent(nullptr);
        excerpt_->setAttribute(Qt::WA_DontShowOnScreen);
        excerptOwner_.reset(excerpt_);
    }
}

GuideIllustration::~GuideIllustration() = default;

bool GuideIllustration::event(QEvent* event) {
    if (event->type() == QEvent::Resize || event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        dirty_ = true;
        update();
    }
    return QWidget::event(event);
}

void GuideIllustration::rebuild() {
    dirty_ = false;
    applyAppearance(source_.get());
    if (excerptOwner_) applyAppearance(excerptOwner_.get());
    const int inset = 16, contentWidth = std::max(200, width() - 2 * inset);
    prepare(excerpt_, contentWidth);
    const int rowHeight = excerpt_->height();
    const int footerHeight = secondary_ ? secondary_->sizeHint().height() : 0;
    const int gap = secondary_ ? 18 : 0;
    const int imageHeight = rowHeight + gap + footerHeight;
    imageRect_ = QRect(inset, 64, contentWidth, imageHeight);
    image_ = QPixmap(QSize(contentWidth, imageHeight) * devicePixelRatioF());
    image_.setDevicePixelRatio(devicePixelRatioF()); image_.fill(Qt::transparent);
    QPainter snapshot(&image_);
    excerpt_->render(&snapshot);
    QRect footer;
    if (secondary_) {
        secondary_->resize(secondary_->sizeHint());
        footer = QRect(QPoint(contentWidth - secondary_->width(), rowHeight + gap), secondary_->size());
        secondary_->render(&snapshot, footer.topLeft());
    }
    targets_.clear();
    for (auto* field : marked_) {
        const auto bounds = field == secondary_ ? footer : QRect(field->mapTo(excerpt_, QPoint()), field->size());
        targets_.append(bounds.translated(imageRect_.topLeft()));
    }
    const int captionWidth = std::min(280, contentWidth);
    first_->setGeometry(std::clamp(targets_[0].center().x() - captionWidth / 2, inset, width() - inset - captionWidth), 0, captionWidth, 32);
    second_->setGeometry(std::clamp(targets_[1].center().x() - captionWidth / 2, inset, width() - inset - captionWidth), imageRect_.bottom() + 36, captionWidth, 32);
    first_->setAlignment(Qt::AlignCenter); second_->setAlignment(Qt::AlignCenter);
    setMinimumHeight(imageRect_.bottom() + 70);
}

void GuideIllustration::paintEvent(QPaintEvent*) {
    if (dirty_) rebuild();
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.drawPixmap(imageRect_.topLeft(), image_);
    const auto colors = appearance();
    // Recompute endpoints from the actual widget rectangles on every resize.
    // Arrows live in the gutters, never on top of the text or controls.
    for (int index = 0; index < targets_.size(); ++index) {
        const auto target = targets_[index];
        const bool down = index == 0;
        const QPointF tip(target.center().x(), down ? target.top() - 5 : target.bottom() + 5);
        const QPointF start(target.center().x(), down ? first_->geometry().bottom() + 5 : second_->geometry().top() - 5);
        painter.setPen(QPen(colors.accent, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(start, tip);
        const double direction = down ? -1.0 : 1.0;
        painter.drawPolyline(QPolygonF{tip + QPointF(-4, direction * 6), tip, tip + QPointF(4, direction * 6)});
        painter.setPen(QPen(colors.accent, 1.2)); painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(QRectF(target).adjusted(-3, -3, 3, 3), 6, 6);
    }
}
} // namespace llavon::lora
