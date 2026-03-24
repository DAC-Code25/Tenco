#include "routepathfinder.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

QList<int> RoutePathFinder::findShortestPath(
    int startId,
    int endId,
    const QList<RouteGraphEdge> &edges,
    const RoutePathFinderOptions &options)
{
    QList<int> result;
    if (startId == endId) {
        return result;
    }
    if (edges.isEmpty()) {
        return result;
    }

    QHash<int, QList<int>> adjacency;
    for (int i = 0; i < edges.size(); ++i) {
        const RouteGraphEdge &edge = edges.at(i);
        if (edge.pathId < 0 || edge.fromId < 0 || edge.toId < 0) {
            continue;
        }
        adjacency[edge.fromId].append(i);
    }
    if (!adjacency.contains(startId)) {
        return result;
    }

    struct NodeState {
        double cost = std::numeric_limits<double>::infinity();
        int prevNode = -1;
        int prevPathId = -1;
        bool settled = false;
    };

    QHash<int, NodeState> states;
    states[startId].cost = 0.0;

    using QueueEntry = std::pair<double, int>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
    queue.emplace(0.0, startId);

    while (!queue.empty()) {
        const auto [queuedCost, nodeId] = queue.top();
        queue.pop();

        NodeState &node = states[nodeId];
        if (node.settled) {
            continue;
        }
        if (queuedCost > node.cost + 1e-12) {
            continue;
        }
        node.settled = true;
        if (nodeId == endId) {
            break;
        }

        const QList<int> outgoing = adjacency.value(nodeId);
        for (int edgeIndex : outgoing) {
            if (edgeIndex < 0 || edgeIndex >= edges.size()) {
                continue;
            }
            const RouteGraphEdge &edge = edges.at(edgeIndex);
            const double edgeLength = std::max(options.minEdgeCost, edge.lengthMeters);
            const double edgeCost = edgeLength + options.edgePenalty + (edge.isArc ? options.arcPenalty : 0.0);
            const double nextCost = node.cost + edgeCost;

            NodeState &nextState = states[edge.toId];
            if (nextCost + 1e-12 < nextState.cost) {
                nextState.cost = nextCost;
                nextState.prevNode = nodeId;
                nextState.prevPathId = edge.pathId;
                queue.emplace(nextCost, edge.toId);
            }
        }
    }

    if (!states.contains(endId) || states[endId].prevPathId < 0) {
        return result;
    }

    int current = endId;
    while (current != startId) {
        if (!states.contains(current)) {
            result.clear();
            return result;
        }
        const NodeState &state = states[current];
        if (state.prevPathId < 0 || state.prevNode < 0) {
            result.clear();
            return result;
        }
        result.prepend(state.prevPathId);
        current = state.prevNode;
    }

    return result;
}
