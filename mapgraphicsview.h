#ifndef MAPGRAPHICSVIEW_H
#define MAPGRAPHICSVIEW_H

#include <QGraphicsView>
#include <QPoint>
#include <QPointF>

class MapGraphicsView : public QGraphicsView
{
    Q_OBJECT

public:
    explicit MapGraphicsView(QWidget *parent = nullptr);

    void setAltModifierZoomEnabled(bool enabled);
    bool isAltModifierZoomEnabled() const { return m_wheelZoomEnabled; }
    void zoomByFactor(double factor);
    double zoomFactor() const { return m_zoomFactor; }

signals:
    void scenePointClicked(const QPointF &scenePos, Qt::MouseButton button, Qt::KeyboardModifiers modifiers);
    void mouseMovedOnScene(const QPointF &scenePos);

protected:
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void applyZoom(double factor);

    double m_zoomFactor = 1.0;
    const double m_minZoom = 0.2;
    const double m_maxZoom = 8.0;
    bool m_wheelZoomEnabled = true;
    bool m_isPanning = false;
    QPoint m_lastMousePos;
};

#endif // MAPGRAPHICSVIEW_H


