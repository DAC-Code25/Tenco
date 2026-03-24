#ifndef ROUTEPATHFINDER_H
#define ROUTEPATHFINDER_H

#include <QList>

struct RouteGraphEdge
{
    int pathId = -1;
    int fromId = -1;
    int toId = -1;
    double lengthMeters = 0.0;
    bool isArc = false;
};

struct RoutePathFinderOptions
{
    double minEdgeCost = 1e-3;
    double edgePenalty = 0.01;
    double arcPenalty = 0.05;
};

class RoutePathFinder
{
public:
    static QList<int> findShortestPath(
        int startId,
        int endId,
        const QList<RouteGraphEdge> &edges,
        const RoutePathFinderOptions &options = RoutePathFinderOptions{});
};

#endif // ROUTEPATHFINDER_H
