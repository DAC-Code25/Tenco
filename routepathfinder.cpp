#include "routepathfinder.h"

#include <QHash>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

QList<RoutePathTraversal> RoutePathFinder::findShortestRoute(
    int startId,
    int endId,
    const QList<RouteGraphEdge> &edges,
    const RoutePathFinderOptions &options)
{
    QList<RoutePathTraversal> result;
    if (startId == endId) {
        return result;
    }
    if (edges.isEmpty()) {
        return result;
    }

    struct OutgoingEdge {
        int index = -1;
        int toId = -1;
        bool reversed = false;
    };
    QHash<int, QList<OutgoingEdge>> adjacency;
    for (int i = 0; i < edges.size(); ++i) {
        const RouteGraphEdge &edge = edges.at(i);
        if (edge.pathId < 0 || edge.fromId < 0 || edge.toId < 0) {
            continue;
        }
        adjacency[edge.fromId].append({i, edge.toId, false});
        if (edge.bidirectional && edge.fromId != edge.toId) {
            adjacency[edge.toId].append({i, edge.fromId, true});
        }
    }
    if (!adjacency.contains(startId)) {
        return result;
    }

    struct NodeState {
        double cost = std::numeric_limits<double>::infinity();
        int prevNode = -1;
        int prevPathId = -1;
        bool prevReversed = false;
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

        const double nodeCost = node.cost;
        const QList<OutgoingEdge> outgoing = adjacency.value(nodeId);
        for (const OutgoingEdge &traversal : outgoing) {
            const RouteGraphEdge &edge = edges.at(traversal.index);
            const double edgeLength = std::max(options.minEdgeCost, edge.lengthMeters);
            const double edgeCost = edgeLength + options.edgePenalty + (edge.isArc ? options.arcPenalty : 0.0);
            const double nextCost = nodeCost + edgeCost;

            NodeState &nextState = states[traversal.toId];
            if (nextCost + 1e-12 < nextState.cost) {
                nextState.cost = nextCost;
                nextState.prevNode = nodeId;
                nextState.prevPathId = edge.pathId;
                nextState.prevReversed = traversal.reversed;
                queue.emplace(nextCost, traversal.toId);
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
        result.prepend({state.prevPathId, state.prevNode, current, state.prevReversed});
        current = state.prevNode;
    }

    return result;
}

QList<int> RoutePathFinder::findShortestPath(
    int startId,
    int endId,
    const QList<RouteGraphEdge> &edges,
    const RoutePathFinderOptions &options)
{
    QList<int> pathIds;
    const auto traversals = findShortestRoute(startId, endId, edges, options);
    pathIds.reserve(traversals.size());
    for (const RoutePathTraversal &traversal : traversals) pathIds.append(traversal.pathId);
    return pathIds;
}
