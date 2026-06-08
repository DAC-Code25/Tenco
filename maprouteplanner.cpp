#include "maprouteplanner.h"

#include "routepathfinder.h"

QList<int> MapRoutePlanner::findPathIds(int startId,
                                        int endId,
                                        const QList<Edge> &edges,
                                        const ConfigManager::RoutePlanningConfig &config)
{
    QList<RouteGraphEdge> graphEdges;
    graphEdges.reserve(edges.size());
    for (const Edge &edge : edges) {
        RouteGraphEdge graphEdge;
        graphEdge.pathId = edge.pathId;
        graphEdge.fromId = edge.fromId;
        graphEdge.toId = edge.toId;
        graphEdge.lengthMeters = edge.lengthMeters;
        graphEdge.isArc = edge.isArc;
        graphEdges.append(graphEdge);
    }

    RoutePathFinderOptions options;
    options.minEdgeCost = config.minEdgeCost;
    options.edgePenalty = config.edgePenalty;
    options.arcPenalty = config.arcPenalty;
    return RoutePathFinder::findShortestPath(startId, endId, graphEdges, options);
}
