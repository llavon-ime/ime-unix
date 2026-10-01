#pragma once

#include <QGraphicsView>
#include <QJsonArray>
#include <QMap>

namespace llavon::lora {

class HistoryNode;
class HistoryGraph final : public QGraphicsView {
    Q_OBJECT
public:
    explicit HistoryGraph(QWidget* parent = nullptr);
    void setRuns(const QJsonArray& runs, const QString& activeModel);
    QString selectedId() const;
    void selectRun(const QString& id);
    void zoomBy(double factor);
    void resetView();
    void arrangeNodes();
    int nodeCount() const { return static_cast<int>(nodes_.size()); }
signals:
    void runSelected(const QString& id);
    void zoomChanged(int percent);
protected:
    void showEvent(QShowEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void drawBackground(QPainter* painter, const QRectF& rect) override;
private:
    void updateEdges();
    QMap<QString, HistoryNode*> nodes_;
    QMap<QString, QString> parents_;
    QList<QGraphicsPathItem*> edges_;
    QJsonArray runs_;
    QString activeModel_;
    bool rebuilding_ = false;
    bool firstShow_ = true;
};

} // namespace llavon::lora
