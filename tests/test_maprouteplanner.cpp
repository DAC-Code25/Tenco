#include "maprouteplanner.h"

#include <QtTest>

class MapRoutePlannerTest : public QObject
{
    Q_OBJECT

private slots:
    void arcPenaltyCanChangeSelectedPath();
    void reverseRouteRetainsSingleRelationIdentity();
};

void MapRoutePlannerTest::arcPenaltyCanChangeSelectedPath()
{
    const QList<MapRoutePlanner::Edge> edges{
        {1, 1, 2, 1.0, true},
        {2, 2, 4, 1.0, true},
        {3, 1, 3, 2.0, false},
        {4, 3, 4, 2.0, false},
    };

    ConfigManager::RoutePlanningConfig preferShortArcs;
    preferShortArcs.minEdgeCost = 0.001;
    preferShortArcs.edgePenalty = 0.0;
    preferShortArcs.arcPenalty = 0.0;
    QCOMPARE(MapRoutePlanner::findPathIds(1, 4, edges, preferShortArcs), QList<int>({1, 2}));

    ConfigManager::RoutePlanningConfig avoidArcs;
    avoidArcs.minEdgeCost = 0.001;
    avoidArcs.edgePenalty = 0.0;
    avoidArcs.arcPenalty = 2.0;
    QCOMPARE(MapRoutePlanner::findPathIds(1, 4, edges, avoidArcs), QList<int>({3, 4}));
}

void MapRoutePlannerTest::reverseRouteRetainsSingleRelationIdentity()
{
    const QList<MapRoutePlanner::Edge> edges{
        {10, 1, 2, 1.0, true, true},
        {11, 2, 3, 1.0, false, true},
        {12, 3, 1, 10.0, false, false}
    };
    ConfigManager::RoutePlanningConfig config;
    QCOMPARE(MapRoutePlanner::findPathIds(3, 1, edges, config), QList<int>({11, 10}));
    const auto route = MapRoutePlanner::findRoute(3, 1, edges, config);
    QCOMPARE(route.size(), 2);
    QVERIFY(route[0].reversed);
    QVERIFY(route[1].reversed);
    QCOMPARE(route[0].fromId, 3);
    QCOMPARE(route[0].toId, 2);
    QCOMPARE(route[1].fromId, 2);
    QCOMPARE(route[1].toId, 1);
}

QTEST_MAIN(MapRoutePlannerTest)
#include "test_maprouteplanner.moc"
