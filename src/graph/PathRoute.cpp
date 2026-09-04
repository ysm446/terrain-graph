#include "graph/PathRoute.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>

namespace tg::graph {
namespace {

// 近傍。3 セル以内の既約な向き 36 方向（8 方向だけだと 45° 刻みの階段になって、
// 斜面を斜めに登る線形が作れない。(3, 1) なら勾配は直登の 1/√10 になる）。
constexpr int kNeighbors[36][2] = {
    {1, 0},  {-1, 0}, {0, 1},  {0, -1}, {1, 1},  {1, -1}, {-1, 1}, {-1, -1}, {2, 1},
    {2, -1}, {-2, 1}, {-2, -1}, {1, 2}, {1, -2}, {-1, 2}, {-1, -2}, {3, 1},  {3, -1},
    {-3, 1}, {-3, -1}, {1, 3}, {1, -3}, {-1, 3}, {-1, -3}, {3, 2},  {3, -2}, {-3, 2},
    {-3, -2}, {2, 3}, {2, -3}, {-2, 3}, {-2, -3}, {2, 0},  {-2, 0}, {0, 2},  {0, -2}};

// ペナルティの強さ。内部定数で始め、欲しくなったらプロパティに出す。
constexpr float kRoadExcessWeight = 8.0f;  // 道路: 超過^2 に掛ける
constexpr float kFlowUphillWeight = 4.0f;  // 流れ: (上り勾配 / 5%)^2 に掛ける
constexpr float kFlowUphillScale = 0.05f;
constexpr float kFlowValleyWeight = 2.0f;  // 流れ: 相対高さ（箱の中で 0〜1）に掛ける
// 曲がりのペナルティ（m）。(1 − cos θ) に掛ける（直進 0、直角 1 倍、折り返し 2 倍）。
// **これが無いと、勾配を守るために 1 歩ごとに左右へ振る細かいジグザグになり、間引くと
// 直登と同じ線に戻ってしまう。** 曲がるたびに払うので、脚の長いつづら折れになる。
// 前の 1 歩の向きだけを見る近似（向きを状態に持たない）。厳密な最適ではないが、
// セルごとの状態が 36 倍にならずに済む。
constexpr float kRoadTurnMeters = 30.0f;
constexpr float kFlowTurnMeters = 10.0f;
// 探索の箱。両端の外接矩形に、両端の距離の 3/4（最低 40 セル）の余白。
// 全面を見ると無意味な大回りをするし、遅い。
constexpr float kBoxMarginRatio = 0.75f;
constexpr int kBoxMarginMinCells = 40;

// 1 歩のコスト係数（長さに掛ける）。
struct StepCost {
    PathRoute mode = PathRoute::Road;
    float allowedGrade = 0.1f;  // 割合
    float heightMin = 0.0f;     // 箱の中の最低（m）。流れの相対高さに使う
    float heightRange = 1.0f;

    float Factor(float heightFrom, float heightTo, float length) const {
        const float delta = heightTo - heightFrom;
        if (mode == PathRoute::Flow) {
            const float up = std::max(0.0f, delta / length) / kFlowUphillScale;
            const float relative =
                (heightRange > 1e-3f) ? (heightTo - heightMin) / heightRange : 0.0f;
            return 1.0f + kFlowUphillWeight * up * up + kFlowValleyWeight * relative;
        }
        const float grade = std::abs(delta) / length;
        const float excess =
            std::max(0.0f, grade - allowedGrade) / std::max(allowedGrade, 0.005f);
        return 1.0f + kRoadExcessWeight * excess * excess;
    }
};

float DistanceToSegment(const PathRouteWaypoint& p, const PathRouteWaypoint& a,
                        const PathRouteWaypoint& b) {
    const float abx = b.u - a.u;
    const float aby = b.v - a.v;
    const float lengthSq = abx * abx + aby * aby;
    float t = 0.0f;
    if (lengthSq > 1e-12f) {
        t = std::clamp(((p.u - a.u) * abx + (p.v - a.v) * aby) / lengthSq, 0.0f, 1.0f);
    }
    const float dx = p.u - (a.u + abx * t);
    const float dy = p.v - (a.v + aby * t);
    return std::sqrt(dx * dx + dy * dy);
}

// Douglas–Peucker。両端は必ず残す。
std::vector<PathRouteWaypoint> Simplify(const std::vector<PathRouteWaypoint>& points,
                                        float tolerance) {
    std::vector<PathRouteWaypoint> out;
    if (points.size() < 3) {
        out = points;
        return out;
    }
    std::vector<bool> keep(points.size(), false);
    keep.front() = true;
    keep.back() = true;
    std::vector<std::pair<size_t, size_t>> stack;
    stack.emplace_back(0, points.size() - 1);
    while (!stack.empty()) {
        const auto [first, last] = stack.back();
        stack.pop_back();
        if (last <= first + 1) {
            continue;
        }
        float farthest = 0.0f;
        size_t index = first;
        for (size_t i = first + 1; i < last; ++i) {
            const float distance = DistanceToSegment(points[i], points[first], points[last]);
            if (distance > farthest) {
                farthest = distance;
                index = i;
            }
        }
        if (farthest > tolerance && index != first) {
            keep[index] = true;
            stack.emplace_back(first, index);
            stack.emplace_back(index, last);
        }
    }
    for (size_t i = 0; i < points.size(); ++i) {
        if (keep[i]) {
            out.push_back(points[i]);
        }
    }
    return out;
}

}  // namespace

bool FindPathRoute(const PathRouteTerrain& terrain, const PathRouteQuery& query,
                   std::vector<PathRouteWaypoint>& outWaypoints) {
    outWaypoints.clear();
    if (!terrain.IsValid()) {
        return false;
    }
    const int resolution = static_cast<int>(terrain.resolution);
    const auto cellOf = [resolution](float coordinate) {
        return std::clamp(static_cast<int>(coordinate * static_cast<float>(resolution)), 0,
                          resolution - 1);
    };
    const int startX = cellOf(query.fromU);
    const int startY = cellOf(query.fromV);
    const int endX = cellOf(query.toU);
    const int endY = cellOf(query.toV);
    if (startX == endX && startY == endY) {
        return true;
    }

    // 探索の箱。
    const int span = std::max(std::abs(endX - startX), std::abs(endY - startY));
    const int margin = std::max(static_cast<int>(static_cast<float>(span) * kBoxMarginRatio),
                                kBoxMarginMinCells);
    const int minX = std::max(0, std::min(startX, endX) - margin);
    const int minY = std::max(0, std::min(startY, endY) - margin);
    const int maxX = std::min(resolution - 1, std::max(startX, endX) + margin);
    const int maxY = std::min(resolution - 1, std::max(startY, endY) + margin);
    const int width = maxX - minX + 1;
    const int height = maxY - minY + 1;
    const size_t cellCount = static_cast<size_t>(width) * static_cast<size_t>(height);

    const float cellMeters = terrain.sizeMeters / static_cast<float>(resolution);
    const auto heightAt = [&](int x, int y) {
        return terrain.heights[static_cast<size_t>(y) * terrain.resolution +
                               static_cast<size_t>(x)] *
               terrain.heightMeters;
    };
    const auto indexOf = [&](int x, int y) {
        return static_cast<size_t>(y - minY) * static_cast<size_t>(width) +
               static_cast<size_t>(x - minX);
    };

    StepCost cost;
    cost.mode = query.mode;
    cost.allowedGrade = std::max(query.maxGradePercent, 0.1f) * 0.01f;
    if (cost.mode == PathRoute::Flow) {
        float lowest = std::numeric_limits<float>::max();
        float highest = std::numeric_limits<float>::lowest();
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float h = heightAt(x, y);
                lowest = std::min(lowest, h);
                highest = std::max(highest, h);
            }
        }
        cost.heightMin = lowest;
        cost.heightRange = highest - lowest;
    }

    // A*。ヒューリスティックは直線距離（コストは常に長さ以上なので許容的）。
    constexpr float kInfinity = std::numeric_limits<float>::max();
    std::vector<float> best(cellCount, kInfinity);
    std::vector<int32_t> parent(cellCount, -1);
    std::vector<uint8_t> closed(cellCount, 0);
    using Entry = std::pair<float, int32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    const auto heuristic = [&](int x, int y) {
        const float dx = static_cast<float>(x - endX);
        const float dy = static_cast<float>(y - endY);
        return std::sqrt(dx * dx + dy * dy) * cellMeters;
    };
    const size_t startIndex = indexOf(startX, startY);
    const size_t endIndex = indexOf(endX, endY);
    best[startIndex] = 0.0f;
    open.emplace(heuristic(startX, startY), static_cast<int32_t>(startIndex));
    while (!open.empty()) {
        const int32_t current = open.top().second;
        open.pop();
        const auto index = static_cast<size_t>(current);
        if (closed[index]) {
            continue;
        }
        closed[index] = 1;
        if (index == endIndex) {
            break;
        }
        const int x = minX + static_cast<int>(index % static_cast<size_t>(width));
        const int y = minY + static_cast<int>(index / static_cast<size_t>(width));
        const float h0 = heightAt(x, y);
        // 入ってきた向き（曲がりのペナルティ用）。始点は無し。
        float inX = 0.0f;
        float inY = 0.0f;
        if (parent[index] >= 0) {
            const auto previous = static_cast<size_t>(parent[index]);
            const int px = minX + static_cast<int>(previous % static_cast<size_t>(width));
            const int py = minY + static_cast<int>(previous / static_cast<size_t>(width));
            const float dx = static_cast<float>(x - px);
            const float dy = static_cast<float>(y - py);
            const float length = std::sqrt(dx * dx + dy * dy);
            inX = dx / length;
            inY = dy / length;
        }
        const float turnMeters = (cost.mode == PathRoute::Flow) ? kFlowTurnMeters : kRoadTurnMeters;
        for (const auto& offset : kNeighbors) {
            const int nx = x + offset[0];
            const int ny = y + offset[1];
            if (nx < minX || nx > maxX || ny < minY || ny > maxY) {
                continue;
            }
            const size_t next = indexOf(nx, ny);
            if (closed[next]) {
                continue;
            }
            const float cells =
                std::sqrt(static_cast<float>(offset[0] * offset[0] + offset[1] * offset[1]));
            const float length = cells * cellMeters;
            float turn = 0.0f;
            if (inX != 0.0f || inY != 0.0f) {
                const float cosine =
                    (inX * static_cast<float>(offset[0]) + inY * static_cast<float>(offset[1])) /
                    cells;
                turn = turnMeters * (1.0f - cosine);
            }
            const float candidate =
                best[index] + length * cost.Factor(h0, heightAt(nx, ny), length) + turn;
            if (candidate < best[next]) {
                best[next] = candidate;
                parent[next] = current;
                open.emplace(candidate + heuristic(nx, ny), static_cast<int32_t>(next));
            }
        }
    }
    if (parent[endIndex] < 0) {
        return false;
    }

    // 終点から辿って折れ線にする。両端はセルの中心ではなく、問い合わせの位置そのもの。
    std::vector<PathRouteWaypoint> cells;
    for (int32_t index = static_cast<int32_t>(endIndex); index >= 0;
         index = parent[static_cast<size_t>(index)]) {
        const auto cell = static_cast<size_t>(index);
        const int x = minX + static_cast<int>(cell % static_cast<size_t>(width));
        const int y = minY + static_cast<int>(cell / static_cast<size_t>(width));
        cells.push_back({(static_cast<float>(x) + 0.5f) / static_cast<float>(resolution),
                         (static_cast<float>(y) + 0.5f) / static_cast<float>(resolution)});
        if (cell == startIndex) {
            break;
        }
    }
    std::reverse(cells.begin(), cells.end());
    cells.front() = {query.fromU, query.fromV};
    cells.back() = {query.toU, query.toV};

    const float tolerance = (query.simplifyToleranceUv > 0.0f)
                                ? query.simplifyToleranceUv
                                : 1.5f / static_cast<float>(resolution);
    const std::vector<PathRouteWaypoint> simplified = Simplify(cells, tolerance);
    if (simplified.size() > 2) {
        outWaypoints.assign(simplified.begin() + 1, simplified.end() - 1);
    }
    return true;
}

bool RoutePathEdge(PathSettings& path, PathElementId edgeId, const PathRouteTerrain& terrain) {
    PathEdge* edge = nullptr;
    for (PathEdge& candidate : path.edges) {
        if (candidate.id == edgeId) {
            edge = &candidate;
            break;
        }
    }
    if (edge == nullptr || edge->route == PathRoute::None || !terrain.IsValid()) {
        return false;
    }
    const PathPoint* a = path.FindPoint(edge->from);
    const PathPoint* b = path.FindPoint(edge->to);
    if (a == nullptr || b == nullptr) {
        return false;
    }
    PathRouteQuery query;
    query.fromU = a->u;
    query.fromV = a->v;
    query.toU = b->u;
    query.toV = b->v;
    query.mode = edge->route;
    query.maxGradePercent = edge->maxGradePercent;
    // 間引きの許容差は幅の 1/4（最低でもセル 1.5 個）。マスクは幅でぼけるので細かすぎても無駄。
    const float widthMeters = std::min(a->widthMeters, b->widthMeters);
    query.simplifyToleranceUv =
        std::max(1.5f / static_cast<float>(terrain.resolution),
                 0.25f * widthMeters / std::max(terrain.sizeMeters, 1e-3f));
    std::vector<PathRouteWaypoint> waypoints;
    if (!FindPathRoute(terrain, query, waypoints)) {
        return false;
    }
    edge->waypoints = std::move(waypoints);
    edge->routed = true;
    edge->routedFromU = a->u;
    edge->routedFromV = a->v;
    edge->routedToU = b->u;
    edge->routedToV = b->v;
    return true;
}

size_t RoutePathEdges(PathSettings& path, const PathRouteTerrain& terrain, bool force,
                      const std::vector<PathElementId>* only) {
    // 対象の ID を先に集める（計算の途中でエッジの配列を触らないが、参照を握ったまま回さない）。
    std::vector<PathElementId> ids;
    for (PathEdge& edge : path.edges) {
        if (edge.route == PathRoute::None) {
            edge.waypoints.clear();
            edge.routed = false;
            continue;
        }
        if (only != nullptr && std::find(only->begin(), only->end(), edge.id) == only->end()) {
            continue;
        }
        if (!force && IsPathEdgeRouteCurrent(path, edge)) {
            continue;
        }
        ids.push_back(edge.id);
    }
    size_t routed = 0;
    for (const PathElementId id : ids) {
        if (RoutePathEdge(path, id, terrain)) {
            ++routed;
        }
    }
    return routed;
}

}  // namespace tg::graph
