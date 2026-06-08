#include "mapgeometry.h"

#include <cmath>

#include <QtMath>

namespace MapGeometry {

double normalizeAngle(double angle)
{
    angle = std::fmod(angle + M_PI, 2.0 * M_PI);
    if (angle < 0.0) {
        angle += 2.0 * M_PI;
    }
    return angle - M_PI;
}

double mapToStandardAngle(double angle)
{
    return normalizeAngle(angle - M_PI_2);
}

double standardToMapAngle(double angle)
{
    return normalizeAngle(angle + M_PI_2);
}

QPointF mapToStandardPoint(const QPointF &mapPoint)
{
    return QPointF(mapPoint.y(), -mapPoint.x());
}

QPointF standardToMapPoint(const QPointF &stdPoint)
{
    return QPointF(-stdPoint.y(), stdPoint.x());
}

bool pointsAlmostEqual(const QPointF &a, const QPointF &b, double epsilon)
{
    return std::abs(a.x() - b.x()) < epsilon && std::abs(a.y() - b.y()) < epsilon;
}

QString formatNumber(double value, int precision)
{
    return QString::number(value, 'f', precision);
}

double polylineLength(const QList<QPointF> &polyline)
{
    if (polyline.size() < 2) {
        return 0.0;
    }
    double length = 0.0;
    for (int i = 1; i < polyline.size(); ++i) {
        const QPointF &a = polyline.at(i - 1);
        const QPointF &b = polyline.at(i);
        length += std::hypot(b.x() - a.x(), b.y() - a.y());
    }
    return length;
}

} // namespace MapGeometry
