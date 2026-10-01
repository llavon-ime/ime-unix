#include "history_graph.hpp"
#include "appearance.hpp"

#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPainter>
#include <QSet>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

namespace llavon::lora {
namespace {
constexpr qreal nodeWidth = 228;
constexpr qreal nodeHeight = 142;
QString valueText(const QJsonValue& value) {
    return value.isString() ? value.toString() : QString::number(value.toDouble());
}
}

class HistoryNode final : public QGraphicsObject {
public:
    HistoryNode(QString id, QString title, QString subtitle, QString metrics, bool active, bool latest,
                std::function<void()> moved)
        : title_(std::move(title)), subtitle_(std::move(subtitle)), metrics_(std::move(metrics)),
          active_(active), latest_(latest), moved_(std::move(moved)) {
        setData(0, id);
        setFlags(ItemIsMovable | ItemIsSelectable | ItemIsFocusable | ItemSendsGeometryChanges);
        setAcceptHoverEvents(true);
        setCursor(Qt::OpenHandCursor);
        setToolTip(title_ + "\n" + subtitle_ + "\n" + metrics_ + QStringLiteral("\n拖曳可移動節點；方向鍵可微調位置。"));
    }
    ~HistoryNode() override;
    QRectF boundingRect() const override { return {-3, -3, nodeWidth + 6, nodeHeight + 6}; }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        painter->setRenderHint(QPainter::Antialiasing);
        const bool selected = isSelected() || hasFocus();
        const auto c = appearance();
        painter->setPen(Qt::NoPen); painter->setBrush(QColor(0, 0, 0, 10));
        painter->drawRoundedRect(QRectF(0, 2, nodeWidth, nodeHeight), 10, 10);
        painter->setPen(QPen(selected ? c.accent : c.border, selected ? 2 : 1));
        painter->setBrush(active_ ? c.selection : c.surface);
        painter->drawRoundedRect(QRectF(0, 0, nodeWidth, nodeHeight), 9, 9);
        auto font = painter->font(); font.setPixelSize(11); font.setWeight(QFont::Normal); painter->setFont(font);
        painter->setPen(c.secondary);
        painter->drawText(16, 25, data(0) == "base" ? QStringLiteral("原始版本") : "LoRA · " + data(0).toString());
        if (active_ || latest_) {
            painter->setPen(active_ ? c.accent : c.secondary);
            painter->drawText(QRectF(115, 10, 98, 22), Qt::AlignRight | Qt::AlignVCenter,
                              active_ ? QStringLiteral("● 目前套用") : QStringLiteral("最新訓練"));
        }
        font.setPixelSize(17); font.setWeight(QFont::DemiBold); painter->setFont(font);
        painter->setPen(c.text); painter->drawText(16, 54, title_);
        font.setPixelSize(12); font.setWeight(QFont::Normal); painter->setFont(font);
        painter->setPen(c.secondary);
        painter->drawText(16, 79, QFontMetrics(font).elidedText(subtitle_, Qt::ElideRight, 196));
        painter->setPen(c.border); painter->drawLine(16, 94, 212, 94);
        painter->setPen(c.secondary); painter->drawText(16, 119, metrics_);
        painter->setPen(Qt::NoPen); painter->setBrush(c.accent);
        painter->drawEllipse(QPointF(nodeWidth / 2, 0), 3, 3);
        painter->drawEllipse(QPointF(nodeWidth / 2, nodeHeight), 3, 3);
    }
protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override {
        const auto result = QGraphicsObject::itemChange(change, value);
        if (change == ItemPositionHasChanged && moved_) moved_();
        return result;
    }
private:
    QString title_, subtitle_, metrics_;
    bool active_, latest_;
    std::function<void()> moved_;
};

HistoryNode::~HistoryNode() = default;

HistoryGraph::HistoryGraph(QWidget* parent) : QGraphicsView(parent) {
    setScene(new QGraphicsScene(this));
    setObjectName("history");
    setAccessibleName(QStringLiteral("訓練版本節點圖"));
    setAccessibleDescription(QStringLiteral("拖曳空白平移、拖曳節點移動；Ctrl 加滾輪縮放。左右方括號鍵切換版本，方向鍵移動節點。"));
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(ScrollHandDrag);
    setTransformationAnchor(AnchorUnderMouse);
    setResizeAnchor(AnchorViewCenter);
    setMinimumHeight(280);
    connect(scene(), &QGraphicsScene::selectionChanged, this, [this] {
        if (!rebuilding_) emit runSelected(selectedId());
    });
}

QString HistoryGraph::selectedId() const {
    const auto selected = scene()->selectedItems();
    return selected.isEmpty() ? QString() : selected.first()->data(0).toString();
}

void HistoryGraph::selectRun(const QString& id) {
    if (auto* node = nodes_.value(id)) {
        rebuilding_ = true;
        scene()->clearSelection();
        node->setSelected(true);
        node->setFocus();
        rebuilding_ = false;
        ensureVisible(node, 24, 24);
        emit runSelected(id);
    }
}

void HistoryGraph::setRuns(const QJsonArray& runs, const QString& activeModel) {
    if (!nodes_.isEmpty() && runs == runs_ && activeModel == activeModel_) return;
    const bool initial = nodes_.isEmpty();
    const auto selected = selectedId();
    const auto scrollX = horizontalScrollBar()->value();
    const auto scrollY = verticalScrollBar()->value();
    QMap<QString, QPointF> positions;
    for (auto it = nodes_.cbegin(); it != nodes_.cend(); ++it) positions.insert(it.key(), it.value()->pos());
    rebuilding_ = true;
    scene()->clear(); nodes_.clear(); parents_.clear(); edges_.clear();
    runs_ = runs; activeModel_ = activeModel;
    auto* root = new HistoryNode("base", "Base model", QStringLiteral("原始模型 · 所有分支的起點"),
                                 QStringLiteral("選取可回復預設模型"), activeModel.isEmpty(), false, [this] { updateEdges(); });
    scene()->addItem(root); nodes_.insert("base", root);
    for (const auto value : runs) {
        const auto run = value.toObject(); const auto id = valueText(run.value("id"));
        if (id == "base" || nodes_.contains(id)) continue;
        const QMap<QString, QString> strengths{{"ultra-low", QStringLiteral("極低")}, {"low", QStringLiteral("低")},
            {"medium", QStringLiteral("中")}, {"high", QStringLiteral("高")}, {"advanced", QStringLiteral("進階")}};
        auto* node = new HistoryNode(id, QStringLiteral("訓練 #%1").arg(id),
            strengths.value(run.value("strength").toString(), QStringLiteral("個人化訓練")) + " · " +
                run.value("completed_at").toString().left(16).replace('T', ' '),
            QStringLiteral("+%1 / 累計 %2 筆 · %3 步").arg(valueText(run.value("record_count")),
                valueText(run.value("cumulative_count")), valueText(run.value("optimizer_steps"))),
            !activeModel.isEmpty() && run.value("model_path").toString() == activeModel,
            value == runs.first(), [this] { updateEdges(); });
        scene()->addItem(node); nodes_.insert(id, node);
        auto parent = valueText(run.value("parent_id"));
        parents_.insert(id, parent == "0" ? "base" : parent);
    }
    // Avoid invalid ancestry creating cycles or detached nodes.
    for (auto it = parents_.begin(); it != parents_.end(); ++it) {
        QSet<QString> visited{it.key()}; auto ancestor = it.value();
        while (ancestor != "base" && nodes_.contains(ancestor) && !visited.contains(ancestor)) {
            visited.insert(ancestor); ancestor = parents_.value(ancestor, "base");
        }
        if (!nodes_.contains(it.value()) || visited.contains(ancestor)) it.value() = "base";
    }
    for (qsizetype index = 0; index < parents_.size(); ++index) {
        auto* edge = scene()->addPath({}, QPen(appearance().secondary, 1.5));
        edge->setZValue(-1); edge->setAcceptedMouseButtons(Qt::NoButton); edges_.append(edge);
    }
    rebuilding_ = false;
    arrangeNodes();
    for (auto it = positions.cbegin(); it != positions.cend(); ++it)
        if (nodes_.contains(it.key())) nodes_[it.key()]->setPos(it.value());
    updateEdges();
    if (initial) selectRun("base");
    else if (nodes_.contains(selected)) selectRun(selected);
    else emit runSelected({});
    if (initial) resetView();
    else { horizontalScrollBar()->setValue(scrollX); verticalScrollBar()->setValue(scrollY); }
}

void HistoryGraph::arrangeNodes() {
    qreal leaf = 0;
    std::function<qreal(const QString&, int)> place = [&](const QString& id, int depth) {
        QStringList children;
        for (auto it = parents_.cbegin(); it != parents_.cend(); ++it) if (it.value() == id) children.append(it.key());
        std::sort(children.begin(), children.end(), [](const QString& a, const QString& b) { return a.toLongLong() < b.toLongLong(); });
        qreal x = 0;
        if (children.isEmpty()) { x = leaf * 266; ++leaf; }
        else {
            qreal first = 0, last = 0;
            for (qsizetype index = 0; index < children.size(); ++index) {
                last = place(children[index], depth + 1);
                if (!index) first = last;
            }
            x = (first + last) / 2;
        }
        nodes_[id]->setPos(x, depth * 194);
        return x;
    };
    if (nodes_.contains("base")) place("base", 0);
    updateEdges();
}

void HistoryGraph::updateEdges() {
    if (rebuilding_) return;
    qsizetype index = 0;
    for (auto it = parents_.cbegin(); it != parents_.cend(); ++it) {
        const auto from = nodes_[it.value()]->pos() + QPointF(nodeWidth / 2, nodeHeight);
        const auto to = nodes_[it.key()]->pos() + QPointF(nodeWidth / 2, 0);
        QPainterPath path(from);
        const auto bend = std::max(qreal(30), std::abs(to.y() - from.y()) / 2);
        path.cubicTo(from + QPointF(0, bend), to - QPointF(0, bend), to);
        edges_[index++]->setPath(path);
    }
    // Extra space is intentional: the canvas can be panned in every direction.
    scene()->setSceneRect(scene()->itemsBoundingRect().adjusted(-600, -400, 600, 400));
}

void HistoryGraph::zoomBy(double factor) {
    const auto current = transform().m11();
    const auto next = std::clamp(current * factor, 0.6, 2.5);
    scale(next / current, next / current);
    emit zoomChanged(qRound(next * 100));
}

void HistoryGraph::resetView() {
    resetTransform();
    if (!nodes_.isEmpty()) {
        const auto bounds = scene()->itemsBoundingRect().adjusted(-30, -30, 30, 30);
        const auto factor = std::clamp(std::min(viewport()->width() / bounds.width(), viewport()->height() / bounds.height()), 0.6, 1.0);
        scale(factor, factor);
        auto center = bounds.center();
        if (bounds.height() * factor > viewport()->height())
            center.setY(bounds.top() + viewport()->height() / (2 * factor));
        centerOn(center);
    }
    emit zoomChanged(qRound(transform().m11() * 100));
}

void HistoryGraph::wheelEvent(QWheelEvent* event) {
    if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::MetaModifier)) {
        zoomBy(std::pow(1.0015, event->angleDelta().y())); event->accept();
    } else QGraphicsView::wheelEvent(event);
}

void HistoryGraph::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_BracketLeft || event->key() == Qt::Key_BracketRight) {
        const auto ids = nodes_.keys();
        if (!ids.isEmpty()) {
            const auto offset = event->key() == Qt::Key_BracketRight ? 1 : -1;
            const auto next = (ids.indexOf(selectedId()) + offset + ids.size()) % ids.size();
            selectRun(ids[next]);
        }
        return;
    }
    if (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal) { zoomBy(1.25); return; }
    if (event->key() == Qt::Key_Minus) { zoomBy(0.8); return; }
    if (event->key() == Qt::Key_0) { resetView(); return; }
    QPointF delta;
    switch (event->key()) {
    case Qt::Key_Left: delta.setX(-10); break;
    case Qt::Key_Right: delta.setX(10); break;
    case Qt::Key_Up: delta.setY(-10); break;
    case Qt::Key_Down: delta.setY(10); break;
    default: break;
    }
    if (!delta.isNull() && nodes_.contains(selectedId())) { nodes_[selectedId()]->moveBy(delta.x(), delta.y()); return; }
    QGraphicsView::keyPressEvent(event);
}

void HistoryGraph::drawBackground(QPainter* painter, const QRectF& rect) {
    painter->fillRect(rect, appearance().background);
    painter->setPen(QPen(appearance().border, 1));
    constexpr int spacing = 24;
    const auto left = std::floor(rect.left() / spacing) * spacing;
    const auto top = std::floor(rect.top() / spacing) * spacing;
    for (qreal x = left; x < rect.right(); x += spacing)
        for (qreal y = top; y < rect.bottom(); y += spacing) painter->drawPoint(QPointF(x, y));
}

void HistoryGraph::showEvent(QShowEvent* event) {
    QGraphicsView::showEvent(event);
    if (firstShow_) { resetView(); firstShow_ = false; }
}

} // namespace llavon::lora
