#include <QtTest>

#include "../routepathfinder.h"

class RoutePathFinderTest : public QObject
{
    Q_OBJECT

private slots:
    void prefersLowerWeightedDistance();
    void returnsEmptyWhenDisconnected();
    void bidirectionalRelationSupportsReverseTraversal();
    void mixedDirectionsKeepTraversalOrder();
};

void RoutePathFinderTest::prefersLowerWeightedDistance()
{
    const QList<RouteGraphEdge> edges = {
        {1, 1, 2, 100.0, false},
        {2, 2, 4, 100.0, false},
        {3, 1, 3, 1.0, false},
        {4, 3, 5, 1.0, false},
        {5, 5, 4, 1.0, false}
    };

    const QList<int> path = RoutePathFinder::findShortestPath(1, 4, edges);
    QCOMPARE(path, QList<int>({3, 4, 5}));
}

void RoutePathFinderTest::returnsEmptyWhenDisconnected()
{
    const QList<RouteGraphEdge> edges = {
        {1, 1, 2, 2.0, false},
        {2, 2, 3, 2.0, false}
    };
    const QList<int> path = RoutePathFinder::findShortestPath(1, 9, edges);
    QVERIFY(path.isEmpty());
}

void RoutePathFinderTest::bidirectionalRelationSupportsReverseTraversal()
{
    const QList<RouteGraphEdge> edges{{7, 1, 2, 2.0, true, true}};
    QCOMPARE(RoutePathFinder::findShortestPath(1, 2, edges), QList<int>({7}));
    QCOMPARE(RoutePathFinder::findShortestPath(2, 1, edges), QList<int>({7}));
    const auto reverse = RoutePathFinder::findShortestRoute(2, 1, edges);
    QCOMPARE(reverse.size(), 1);
    QCOMPARE(reverse.first().pathId, 7);
    QCOMPARE(reverse.first().fromId, 2);
    QCOMPARE(reverse.first().toId, 1);
    QVERIFY(reverse.first().reversed);

    const QList<RouteGraphEdge> directed{{7, 1, 2, 2.0, true}};
    QVERIFY(RoutePathFinder::findShortestPath(2, 1, directed).isEmpty());
}

void RoutePathFinderTest::mixedDirectionsKeepTraversalOrder()
{
    const QList<RouteGraphEdge> edges{
        {7, 2, 1, 2.0, true, true},
        {8, 2, 3, 2.0, false, false},
        {9, 4, 3, 2.0, false, true}
    };
    const auto route = RoutePathFinder::findShortestRoute(1, 4, edges);
    QCOMPARE(route.size(), 3);
    QCOMPARE(route[0].pathId, 7);
    QCOMPARE(route[0].fromId, 1);
    QCOMPARE(route[0].toId, 2);
    QVERIFY(route[0].reversed);
    QCOMPARE(route[1].pathId, 8);
    QCOMPARE(route[1].fromId, 2);
    QCOMPARE(route[1].toId, 3);
    QVERIFY(!route[1].reversed);
    QCOMPARE(route[2].pathId, 9);
    QCOMPARE(route[2].fromId, 3);
    QCOMPARE(route[2].toId, 4);
    QVERIFY(route[2].reversed);
    QVERIFY(RoutePathFinder::findShortestRoute(4, 1, edges).isEmpty());
}

QTEST_MAIN(RoutePathFinderTest)
#include "test_routepathfinder.moc"
