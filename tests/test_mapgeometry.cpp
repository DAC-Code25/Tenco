#include "mapgeometry.h"

#include <QtMath>
#include <QtTest>

class MapGeometryTest : public QObject
{
    Q_OBJECT

private slots:
    void normalizesAnglesToSignedPiRange();
    void convertsBetweenMapAndStandardCoordinates();
    void measuresPolylineLength();
};

void MapGeometryTest::normalizesAnglesToSignedPiRange()
{
    QVERIFY(qAbs(MapGeometry::normalizeAngle(3.0 * M_PI) + M_PI) < 1e-9);
    QVERIFY(qAbs(MapGeometry::normalizeAngle(-3.0 * M_PI) + M_PI) < 1e-9);
    QVERIFY(qAbs(MapGeometry::normalizeAngle(M_PI_2) - M_PI_2) < 1e-9);
}

void MapGeometryTest::convertsBetweenMapAndStandardCoordinates()
{
    const QPointF mapPoint(2.0, 5.0);
    const QPointF stdPoint = MapGeometry::mapToStandardPoint(mapPoint);
    QCOMPARE(stdPoint, QPointF(5.0, -2.0));
    QCOMPARE(MapGeometry::standardToMapPoint(stdPoint), mapPoint);
}

void MapGeometryTest::measuresPolylineLength()
{
    const QList<QPointF> polyline{QPointF(0.0, 0.0), QPointF(3.0, 4.0), QPointF(6.0, 8.0)};
    QCOMPARE(MapGeometry::polylineLength(polyline), 10.0);
    QCOMPARE(MapGeometry::formatNumber(1.23456, 2), QStringLiteral("1.23"));
}

QTEST_MAIN(MapGeometryTest)
#include "test_mapgeometry.moc"
