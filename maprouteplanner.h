#ifndef MAPROUTEPLANNER_H
#define MAPROUTEPLANNER_H

#include "configmanager.h"

#include <QList>

class MapRoutePlanner
{
public:
    struct Edge {
        int pathId = -1;
        int fromId = -1;
        int toId = -1;
        double lengthMeters = 0.0;
        bool isArc = false;
    };

    static QList<int> findPathIds(int startId,
                                  int endId,
                                  const QList<Edge> &edges,
                                  const ConfigManager::RoutePlanningConfig &config);
};

#endif // MAPROUTEPLANNER_H
