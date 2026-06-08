#include "maprouteplanner.h"

#include <QtTest>

class MapRoutePlannerTest : public QObject
{
    Q_OBJECT

private slots:
    void arcPenaltyCanChangeSelectedPath();
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

QTEST_MAIN(MapRoutePlannerTest)
#include "test_maprouteplanner.moc"
