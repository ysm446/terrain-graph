#include "graph/Path.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

}  // namespace

const PathPoint* PathSettings::FindPoint(PathElementId id) const {
    for (const PathPoint& point : points) {
        if (point.id == id) {
            return &point;
        }
    }
    return nullptr;
}

PathPoint* PathSettings::FindPoint(PathElementId id) {
    for (PathPoint& point : points) {
        if (point.id == id) {
            return &point;
        }
    }
    return nullptr;
}

const PathEdge* PathSettings::FindEdge(PathElementId id) const {
    for (const PathEdge& edge : edges) {
        if (edge.id == id) {
            return &edge;
        }
    }
    return nullptr;
}

const PathEdge* PathSettings::FindEdgeBetween(PathElementId a, PathElementId b) const {
    for (const PathEdge& edge : edges) {
        if ((edge.from == a && edge.to == b) || (edge.from == b && edge.to == a)) {
            return &edge;
        }
    }
    return nullptr;
}

size_t PathSettings::EdgeCount(PathElementId pointId) const {
    size_t count = 0;
    for (const PathEdge& edge : edges) {
        if (edge.from == pointId || edge.to == pointId) {
            ++count;
        }
    }
    return count;
}

PathElementId AddPathPoint(PathSettings& path, float u, float v, PathElementId connectFrom) {
    PathPoint point;
    point.id = path.nextId++;
    point.u = std::clamp(u, 0.0f, 1.0f);
    point.v = std::clamp(v, 0.0f, 1.0f);
    point.widthMeters = path.defaultWidthMeters;
    point.featherMeters = path.defaultFeatherMeters;
    point.intensity = path.defaultIntensity;
    path.points.push_back(point);
    if (connectFrom != 0 && path.FindPoint(connectFrom) != nullptr) {
        ConnectPathPoints(path, connectFrom, point.id);
    }
    return point.id;
}

bool ConnectPathPoints(PathSettings& path, PathElementId from, PathElementId to) {
    if (from == to || path.FindPoint(from) == nullptr || path.FindPoint(to) == nullptr) {
        return false;
    }
    if (path.FindEdgeBetween(from, to) != nullptr) {
        return true;
    }
    PathEdge edge;
    edge.id = path.nextId++;
    edge.from = from;
    edge.to = to;
    path.edges.push_back(edge);
    return true;
}

PathElementId InsertPathPointOnEdge(PathSettings& path, PathElementId edgeId, float t) {
    const PathEdge* edge = path.FindEdge(edgeId);
    if (edge == nullptr) {
        return 0;
    }
    const PathPoint* a = path.FindPoint(edge->from);
    const PathPoint* b = path.FindPoint(edge->to);
    if (a == nullptr || b == nullptr) {
        return 0;
    }
    const PathElementId from = edge->from;
    const PathElementId to = edge->to;
    t = std::clamp(t, 0.0f, 1.0f);

    PathPoint point;
    point.id = path.nextId++;
    point.u = Lerp(a->u, b->u, t);
    point.v = Lerp(a->v, b->v, t);
    point.widthMeters = Lerp(a->widthMeters, b->widthMeters, t);
    point.featherMeters = Lerp(a->featherMeters, b->featherMeters, t);
    point.intensity = Lerp(a->intensity, b->intensity, t);
    point.heightOffsetMeters = Lerp(a->heightOffsetMeters, b->heightOffsetMeters, t);
    path.points.push_back(point);

    // 元のエッジは from → 新しい点 に縮め、新しい点 → to をもう 1 本張る。
    // 向きは元のまま。
    for (PathEdge& existing : path.edges) {
        if (existing.id == edgeId) {
            existing.to = point.id;
            break;
        }
    }
    PathEdge tail;
    tail.id = path.nextId++;
    tail.from = point.id;
    tail.to = to;
    path.edges.push_back(tail);
    (void)from;
    return point.id;
}

bool DeletePathPoint(PathSettings& path, PathElementId pointId) {
    const size_t before = path.points.size();
    std::erase_if(path.points, [pointId](const PathPoint& point) { return point.id == pointId; });
    if (path.points.size() == before) {
        return false;
    }
    std::erase_if(path.edges, [pointId](const PathEdge& edge) {
        return edge.from == pointId || edge.to == pointId;
    });
    return true;
}

bool DeletePathEdge(PathSettings& path, PathElementId edgeId) {
    const size_t before = path.edges.size();
    std::erase_if(path.edges, [edgeId](const PathEdge& edge) { return edge.id == edgeId; });
    return path.edges.size() != before;
}

bool MergePathPoints(PathSettings& path, PathElementId source, PathElementId target) {
    if (source == target || path.FindPoint(source) == nullptr ||
        path.FindPoint(target) == nullptr) {
        return false;
    }
    // source のエッジを target へ付け替える。付け替えた結果、同じ 2 点を結ぶ
    // エッジが重なったり、自分自身へ戻るエッジができたりしたら捨てる。
    for (PathEdge& edge : path.edges) {
        if (edge.from == source) {
            edge.from = target;
        }
        if (edge.to == source) {
            edge.to = target;
        }
    }
    std::erase_if(path.edges, [](const PathEdge& edge) { return edge.from == edge.to; });
    std::vector<PathEdge> unique;
    for (const PathEdge& edge : path.edges) {
        const bool duplicate = std::any_of(unique.begin(), unique.end(), [&](const PathEdge& u) {
            return (u.from == edge.from && u.to == edge.to) ||
                   (u.from == edge.to && u.to == edge.from);
        });
        if (!duplicate) {
            unique.push_back(edge);
        }
    }
    path.edges = std::move(unique);
    std::erase_if(path.points, [source](const PathPoint& point) { return point.id == source; });
    return true;
}

namespace {

// エッジの、pointId とは反対側の端。
PathElementId OtherEnd(const PathEdge& edge, PathElementId pointId) {
    return (edge.from == pointId) ? edge.to : edge.from;
}

// pointId から other へ向かう向きに、offsetUv だけ進んだ位置。
void OffsetToward(const PathSettings& path, const PathPoint& origin, PathElementId other,
                  float offsetUv, float& outU, float& outV) {
    outU = origin.u;
    outV = origin.v;
    const PathPoint* target = path.FindPoint(other);
    if (target == nullptr) {
        return;
    }
    const float dx = target->u - origin.u;
    const float dy = target->v - origin.v;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 1e-6f) {
        return;
    }
    // 相手より先へは行かない（短いエッジで追い越さないように）。
    const float step = std::min(offsetUv, length * 0.5f);
    outU = std::clamp(origin.u + dx / length * step, 0.0f, 1.0f);
    outV = std::clamp(origin.v + dy / length * step, 0.0f, 1.0f);
}

}  // namespace

bool SplitPathPoint(PathSettings& path, PathElementId pointId, float offsetUv,
                    std::vector<PathElementId>* outCreated) {
    const PathPoint* origin = path.FindPoint(pointId);
    if (origin == nullptr || path.EdgeCount(pointId) < 2) {
        return false;
    }
    const PathPoint original = *origin;

    // 出ていくエッジ（下流）を 1 本だけ元の点に残す。無ければ最初のエッジ。
    PathElementId keepEdge = 0;
    for (const PathEdge& edge : path.edges) {
        if (edge.from == pointId) {
            keepEdge = edge.id;
            break;
        }
    }
    if (keepEdge == 0) {
        for (const PathEdge& edge : path.edges) {
            if (edge.to == pointId) {
                keepEdge = edge.id;
                break;
            }
        }
    }

    std::vector<PathElementId> edgeIds;
    for (const PathEdge& edge : path.edges) {
        if ((edge.from == pointId || edge.to == pointId) && edge.id != keepEdge) {
            edgeIds.push_back(edge.id);
        }
    }
    for (const PathElementId edgeId : edgeIds) {
        const PathElementId created = DetachPathEdgeEnd(path, edgeId, pointId, offsetUv);
        if (created != 0 && outCreated != nullptr) {
            outCreated->push_back(created);
        }
    }
    (void)original;
    return !edgeIds.empty();
}

PathElementId DetachPathEdgeEnd(PathSettings& path, PathElementId edgeId, PathElementId pointId,
                                float offsetUv) {
    const PathEdge* edge = path.FindEdge(edgeId);
    const PathPoint* origin = path.FindPoint(pointId);
    if (edge == nullptr || origin == nullptr || (edge->from != pointId && edge->to != pointId)) {
        return 0;
    }
    const PathElementId other = OtherEnd(*edge, pointId);
    PathPoint point = *origin;
    point.id = path.nextId++;
    OffsetToward(path, *origin, other, offsetUv, point.u, point.v);
    path.points.push_back(point);
    for (PathEdge& existing : path.edges) {
        if (existing.id != edgeId) {
            continue;
        }
        if (existing.from == pointId) {
            existing.from = point.id;
        } else {
            existing.to = point.id;
        }
        break;
    }
    return point.id;
}

bool ReversePathEdge(PathSettings& path, PathElementId edgeId) {
    for (PathEdge& edge : path.edges) {
        if (edge.id == edgeId) {
            std::swap(edge.from, edge.to);
            return true;
        }
    }
    return false;
}

bool ReversePathEdgesAt(PathSettings& path, PathElementId pointId) {
    bool any = false;
    for (PathEdge& edge : path.edges) {
        if (edge.from == pointId || edge.to == pointId) {
            std::swap(edge.from, edge.to);
            any = true;
        }
    }
    return any;
}

std::vector<compositor::PathSegment> BuildPathSegments(const PathSettings& path) {
    std::vector<compositor::PathSegment> segments;
    segments.reserve(path.edges.size() + path.points.size());
    for (const PathEdge& edge : path.edges) {
        const PathPoint* a = path.FindPoint(edge.from);
        const PathPoint* b = path.FindPoint(edge.to);
        if (a == nullptr || b == nullptr) {
            continue;
        }
        compositor::PathSegment segment;
        segment.ax = a->u;
        segment.ay = a->v;
        segment.bx = b->u;
        segment.by = b->v;
        segment.widthA = a->widthMeters;
        segment.widthB = b->widthMeters;
        segment.featherA = a->featherMeters;
        segment.featherB = b->featherMeters;
        segment.intensityA = a->intensity;
        segment.intensityB = b->intensity;
        segments.push_back(segment);
    }
    // 孤立した点は円として出す（長さ 0 の線分）。
    for (const PathPoint& point : path.points) {
        if (path.EdgeCount(point.id) != 0) {
            continue;
        }
        compositor::PathSegment segment;
        segment.ax = segment.bx = point.u;
        segment.ay = segment.by = point.v;
        segment.widthA = segment.widthB = point.widthMeters;
        segment.featherA = segment.featherB = point.featherMeters;
        segment.intensityA = segment.intensityB = point.intensity;
        segments.push_back(segment);
    }
    return segments;
}

}  // namespace tg::graph
