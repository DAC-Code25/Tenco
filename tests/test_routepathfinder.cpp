#include <QtTest>

#include "../routepathfinder.h"

class RoutePathFinderTest : public QObject
{
    Q_OBJECT

private slots:
    void prefersLowerWeightedDistance();
    void returnsEmptyWhenDisconnected();
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

QTEST_MAIN(RoutePathFinderTest)
#include "test_routepathfinder.moc"
