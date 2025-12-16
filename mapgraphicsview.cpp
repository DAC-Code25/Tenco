#include "mapgraphicsview.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
#include <cmath>
#include <algorithm>
#include <QtMath>

namespace {
constexpr double kZoomStep = 1.2;
}

MapGraphicsView::MapGraphicsView(QWidget *parent)
    : QGraphicsView(parent)
{
    setRenderHint(QPainter::Antialiasing, true);
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::NoDrag);
    setCursor(Qt::ArrowCursor);
    viewport()->setCursor(Qt::ArrowCursor);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
}

void MapGraphicsView::setAltModifierZoomEnabled(bool enabled)
{
    m_wheelZoomEnabled = enabled;
}

void MapGraphicsView::wheelEvent(QWheelEvent *event)
{
    if (!m_wheelZoomEnabled) {
        QGraphicsView::wheelEvent(event);
        return;
    }

    const QPoint delta = event->angleDelta();
    if (delta.y() == 0) {
        event->ignore();
        return;
    }

    const double steps = static_cast<double>(delta.y()) / 120.0;
    const double factor = std::pow(kZoomStep, steps);
    applyZoom(factor);
    event->accept();
}

void MapGraphicsView::zoomByFactor(double factor)
{
    applyZoom(factor);
}


void MapGraphicsView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_isPanning = true;
        m_lastMousePos = event->pos();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF scenePos = mapToScene(event->position().toPoint());
#else
    const QPointF scenePos = mapToScene(event->pos());
#endif
    emit scenePointClicked(scenePos, event->button(), event->modifiers());
    QGraphicsView::mousePressEvent(event);
}

void MapGraphicsView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_isPanning = false;
        viewport()->setCursor(Qt::ArrowCursor);
    }
    QGraphicsView::mouseReleaseEvent(event);
}

void MapGraphicsView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_isPanning) {
        const QPoint delta = event->pos() - m_lastMousePos;
        m_lastMousePos = event->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
    } else {
        viewport()->setCursor(Qt::ArrowCursor);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF scenePos = mapToScene(event->position().toPoint());
#else
    const QPointF scenePos = mapToScene(event->pos());
#endif
    emit mouseMovedOnScene(scenePos);
    QGraphicsView::mouseMoveEvent(event);
}


void MapGraphicsView::applyZoom(double factor)
{
    const double newZoom = std::clamp(m_zoomFactor * factor, m_minZoom, m_maxZoom);
    const double effectiveFactor = newZoom / m_zoomFactor;
    if (!qFuzzyCompare(effectiveFactor, 1.0)) {
        scale(effectiveFactor, effectiveFactor);
        m_zoomFactor = newZoom;
    }
}







