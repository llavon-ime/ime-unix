#include "controls.hpp"
#include "appearance.hpp"
#include <QApplication>
#include <QGuiApplication>
#include <QListView>
#include <QPainter>
#include <QProxyStyle>
#include <QPointer>
#include <QScreen>
#include <QStyledItemDelegate>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QWheelEvent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <algorithm>
#include <utility>

namespace llavon::lora {
namespace {
class PopupStyle final : public QProxyStyle {
public:
    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget, QStyleHintReturn* data) const override {
        if (hint == SH_ComboBox_Popup) return 0;
        if (hint == SH_ComboBox_ListMouseTracking) return 1;
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
        if (hint == SH_ComboBox_ListMouseTracking_Current) return 1;
#endif
        return QProxyStyle::styleHint(hint, option, widget, data);
    }
};
PopupStyle* sharedPopupStyle() {
    // Each proxy otherwise constructs its own platform style. Share one across
    // the controls, owned by QApplication so it outlives their popup views.
    static QPointer<PopupStyle> style;
    if (!style) { style = new PopupStyle; style->setParent(qApp); }
    return style;
}
void checkmark(QPainter& painter, const QRectF& rect, const QColor& color) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(QPolygonF{QPointF(rect.left() + rect.width() * .23, rect.top() + rect.height() * .51),
        QPointF(rect.left() + rect.width() * .43, rect.top() + rect.height() * .71),
        QPointF(rect.left() + rect.width() * .78, rect.top() + rect.height() * .30)});
}
class ChoiceDelegate final : public QStyledItemDelegate {
public:
    explicit ChoiceDelegate(QComboBox* combo) : QStyledItemDelegate(combo), combo_(combo) {}
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto size = QStyledItemDelegate::sizeHint(option, index);
        // Popup width is bounded by NativeComboBox. Long labels must not create
        // an invisible horizontal extent beyond the clickable viewport.
        return QSize(1, std::max(34, size.height()));
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto item = option; initStyleOption(&item, index);
        item.text = item.fontMetrics.elidedText(item.text, Qt::ElideRight, std::max(0, item.rect.width() - 48));
        const auto* style = item.widget ? item.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, item.widget);
        if (index.row() == combo_->currentIndex()) {
            painter->save();
            checkmark(*painter, QRectF(option.rect.right() - 26, option.rect.center().y() - 8, 16, 16),
                option.state.testFlag(QStyle::State_Selected) ? option.palette.color(QPalette::HighlightedText) : appearance().accent);
            painter->restore();
        }
    }
private:
    QComboBox* combo_;
};
}

QWidget* recordingCredentials(QWidget* parent) {
    auto* content = new QWidget(parent);
    auto* row = new QHBoxLayout(content); row->setContentsMargins(0, 0, 0, 0);
    for (const auto& entry : {std::pair{"reviewPassword", QStringLiteral("資料密碼")},
             std::pair{"confirmation", QStringLiteral("再次輸入密碼")}}) {
        auto* input = new QLineEdit; input->setObjectName(entry.first);
        input->setPlaceholderText(entry.second); input->setAccessibleName(entry.second);
        input->setEchoMode(QLineEdit::Password); input->setMinimumHeight(32);
        row->addWidget(input, 1);
    }
    for (const auto& entry : {std::pair{"unlock", QStringLiteral("解鎖檢視")},
             std::pair{"setup", QStringLiteral("設定並收集")}, std::pair{"lock", QStringLiteral("鎖定")},
             std::pair{"recording", QStringLiteral("暫停收集")}}) {
        auto* action = new QPushButton(entry.second); action->setObjectName(entry.first);
        action->setAutoDefault(false); action->setMinimumHeight(34);
        action->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        if (QString::fromLatin1(entry.first) == "unlock" || QString::fromLatin1(entry.first) == "setup") action->setProperty("primary", true);
        row->addWidget(action);
    }
    row->addStretch();
    return content;
}

NativeComboBox::NativeComboBox(QWidget* parent) : QComboBox(parent) {
    setStyle(sharedPopupStyle());
    auto* list = new QListView(this); list->setUniformItemSizes(true); list->setMouseTracking(true);
    list->setTextElideMode(Qt::ElideRight); list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); list->setFrameShape(QFrame::NoFrame);
    setView(list); setItemDelegate(new ChoiceDelegate(this));
    connect(list, &QAbstractItemView::entered, list, [list](const QModelIndex& index) {
        if (index.flags().testFlag(Qt::ItemIsEnabled) && index.flags().testFlag(Qt::ItemIsSelectable)) list->setCurrentIndex(index);
    });
    setMaxVisibleItems(8); setMinimumHeight(34); setFocusPolicy(Qt::StrongFocus);
    connect(this, &QComboBox::currentIndexChanged, this, [this] { setToolTip(currentText()); });
}
NativeComboBox::~NativeComboBox() {
    // Qt 6.4 can send FocusOut while destroying the popup view. Clear its
    // focus before deleting popup children, while their style is still alive.
    hidePopup(); view()->clearFocus(); setStyle(nullptr);
}
void NativeComboBox::showPopup() {
    auto width = this->width();
    for (int i = 0; i < count(); ++i) width = std::max(width, fontMetrics().horizontalAdvance(itemText(i)) + 56);
    const auto* screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
    const auto available = screen ? screen->availableGeometry().width() - 24 : 460;
    const auto maximum = std::min(std::max(this->width(), 460), available);
    view()->setFixedWidth(std::clamp(width, std::min(this->width(), maximum), maximum));
    QComboBox::showPopup();
    view()->setMouseTracking(true);
}
void NativeComboBox::wheelEvent(QWheelEvent* event) {
    if (view()->isVisible()) QComboBox::wheelEvent(event);
    else event->ignore();
}

NativeCheckBox::NativeCheckBox(const QString& text, QWidget* parent) : QCheckBox(text, parent) {
    setMinimumHeight(30); setFocusPolicy(Qt::StrongFocus);
}
void NativeCheckBox::paintEvent(QPaintEvent*) {
    QStyleOptionButton option; initStyleOption(&option);
    QStylePainter painter(this); painter.drawControl(QStyle::CE_CheckBox, option);
    const QRectF indicator = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
    const auto c = appearance();
    const bool active = isEnabled() && (hasFocus() || underMouse());
    const auto border = !isEnabled() ? c.border : isChecked() || active ? c.accent : c.secondary;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(border, 1)); painter.setBrush(isChecked() ? (isEnabled() ? c.accent : c.border) : c.surface);
    painter.drawRoundedRect(indicator.adjusted(.5, .5, -.5, -.5), 4, 4);
    if (checkState() == Qt::PartiallyChecked) {
        painter.setPen(QPen(isEnabled() ? c.accent : c.secondary, 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(indicator.left() + 4, indicator.center().y()), QPointF(indicator.right() - 4, indicator.center().y()));
    } else if (isChecked()) checkmark(painter, indicator, isEnabled() ? QColor(Qt::white) : c.secondary);
}
}
