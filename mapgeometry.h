#ifndef MAPGEOMETRY_H
#define MAPGEOMETRY_H

#include <QList>
#include <QPointF>
#include <QString>

namespace MapGeometry {

double normalizeAngle(double angle);
double mapToStandardAngle(double angle);
double standardToMapAngle(double angle);
QPointF mapToStandardPoint(const QPointF &mapPoint);
QPointF standardToMapPoint(const QPointF &stdPoint);
bool pointsAlmostEqual(const QPointF &a, const QPointF &b, double epsilon = 1e-6);
QString formatNumber(double value, int precision = 3);
double polylineLength(const QList<QPointF> &polyline);

} // namespace MapGeometry

#endif // MAPGEOMETRY_H
