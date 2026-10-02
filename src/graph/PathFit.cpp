#include "graph/PathFit.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace tg::graph {
namespace {

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }
float Dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
float Length(Vec2 a) { return std::sqrt(Dot(a, a)); }

// 点から線分への距離と、線分上の最寄りの位置。
float DistanceToSegment(Vec2 p, Vec2 a, Vec2 b, Vec2* outClosest) {
    const Vec2 ab = b - a;
    const float lengthSq = Dot(ab, ab);
    float t = 0.0f;
    if (lengthSq > 1e-12f) t = std::clamp(Dot(p - a, ab) / lengthSq, 0.0f, 1.0f);
    const Vec2 closest = a + ab * t;
    if (outClosest) *outClosest = closest;
    return Length(p - closest);
}

// 折れ線の線分を一様な格子に入れ、点から折れ線への最寄りを近くの升だけ見て求める。
// 元の線（固定）にも、引き直すたびに変わる曲線にも使う（元の点 × 曲線の標本 の総当たりを避ける）。
class SegmentGrid {
public:
    SegmentGrid(const std::vector<Vec2>& line, float cellSize) : m_line(line), m_cell(std::max(cellSize, 0.01f)) {
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            const auto [ax, ay] = CellOf(line[i]);
            const auto [bx, by] = CellOf(line[i + 1]);
            for (int y = std::min(ay, by); y <= std::max(ay, by); ++y)
                for (int x = std::min(ax, bx); x <= std::max(ax, bx); ++x) m_cells[Key(x, y)].push_back(i);
        }
    }
    // 最寄りの位置と、最寄りの頂点の添字。maxRings を超えて見つからなければ偽
    // （距離は maxRings × 升の大きさより大きい）。
    bool Nearest(Vec2 p, int maxRings, float* outDistance, Vec2* outClosest, size_t* outVertex) const {
        if (m_line.size() < 2) return false;
        const auto [cx, cy] = CellOf(p);
        float best = std::numeric_limits<float>::max();
        Vec2 bestPoint;
        size_t bestVertex = 0;
        for (int ring = 0; ring <= maxRings; ++ring) {
            // 見つかった距離より遠い輪は見なくてよい（輪 r の升の最寄りは (r − 1) × 升 以上離れている）。
            if (best < static_cast<float>(ring - 1) * m_cell) break;
            for (int y = cy - ring; y <= cy + ring; ++y) {
                for (int x = cx - ring; x <= cx + ring; ++x) {
                    if (std::abs(x - cx) != ring && std::abs(y - cy) != ring) continue;  // 輪の内側は見た
                    const auto found = m_cells.find(Key(x, y));
                    if (found == m_cells.end()) continue;
                    for (const size_t i : found->second) {
                        Vec2 closest;
                        const float distance = DistanceToSegment(p, m_line[i], m_line[i + 1], &closest);
                        if (distance < best) {
                            best = distance;
                            bestPoint = closest;
                            bestVertex = (Length(closest - m_line[i]) <= Length(closest - m_line[i + 1])) ? i : i + 1;
                        }
                    }
                }
            }
        }
        if (best == std::numeric_limits<float>::max()) return false;
        if (outDistance) *outDistance = best;
        if (outClosest) *outClosest = bestPoint;
        if (outVertex) *outVertex = bestVertex;
        return true;
    }

private:
    std::pair<int, int> CellOf(Vec2 p) const {
        return {static_cast<int>(std::floor(p.x / m_cell)), static_cast<int>(std::floor(p.y / m_cell))};
    }
    static int64_t Key(int x, int y) { return (static_cast<int64_t>(x) << 32) ^ static_cast<uint32_t>(y); }
    const std::vector<Vec2>& m_line;
    float m_cell;
    std::unordered_map<int64_t, std::vector<size_t>> m_cells;
};

// Douglas–Peucker。残す点の添字（昇順。両端を含む）。
std::vector<size_t> DouglasPeucker(const std::vector<Vec2>& line, float tolerance) {
    std::vector<bool> keep(line.size(), false);
    keep.front() = keep.back() = true;
    std::vector<std::pair<size_t, size_t>> stack{{0, line.size() - 1}};
    while (!stack.empty()) {
        const auto [first, last] = stack.back();
        stack.pop_back();
        if (last <= first + 1) continue;
        float farthest = 0.0f;
        size_t index = first;
        for (size_t i = first + 1; i < last; ++i) {
            const float distance = DistanceToSegment(line[i], line[first], line[last], nullptr);
            if (distance > farthest) {
                farthest = distance;
                index = i;
            }
        }
        if (farthest > tolerance) {
            keep[index] = true;
            stack.emplace_back(first, index);
            stack.emplace_back(index, last);
        }
    }
    std::vector<size_t> out;
    for (size_t i = 0; i < keep.size(); ++i)
        if (keep[i]) out.push_back(i);
    return out;
}

// 制御点（m）でクロソイドの鎖を引き、標本（m）を返す。Path の曲線の実装をそのまま使うため、
// 一時の PathSettings に点を置いて SamplePathStrand を呼ぶ。UV は 0〜1 に丸められるので、
// 元の折れ線の範囲を 0.1〜0.9 に収める写像で往復する（曲線は相似なので形は変わらない）。
struct UvMapping {
    Vec2 origin;
    float scale = 1.0f;  // m → UV
    Vec2 ToUv(Vec2 p) const { return {0.1f + (p.x - origin.x) * scale, 0.1f + (p.y - origin.y) * scale}; }
    Vec2 ToMeters(Vec2 uv) const { return {origin.x + (uv.x - 0.1f) / scale, origin.y + (uv.y - 0.1f) / scale}; }
};
UvMapping MappingFor(const std::vector<Vec2>& line) {
    Vec2 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec2 hi{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    for (const Vec2& p : line) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y)};
    }
    // 押し出した制御点が範囲を超えても丸められないように、周りに余裕を取る。
    const float extent = std::max({hi.x - lo.x, hi.y - lo.y, 1.0f});
    UvMapping mapping;
    mapping.origin = {lo.x - extent, lo.y - extent};
    mapping.scale = 0.8f / (extent * 3.0f);
    return mapping;
}

std::vector<Vec2> SampleClothoidChain(const std::vector<Vec2>& control, const UvMapping& mapping, float ratio) {
    PathSettings temp;
    temp.points.reserve(control.size());
    temp.edges.reserve(control.size());
    PathElementId previous = 0;
    for (const Vec2& p : control) {
        // AddPathPoint は点とエッジを線形に探すので、直接積む。
        PathPoint point;
        point.id = temp.nextId++;
        const Vec2 uv = mapping.ToUv(p);
        point.u = std::clamp(uv.x, 0.0f, 1.0f);
        point.v = std::clamp(uv.y, 0.0f, 1.0f);
        temp.points.push_back(point);
        if (previous != 0) {
            PathEdge edge;
            edge.id = temp.nextId++;
            edge.from = previous;
            edge.to = point.id;
            edge.curve = PathCurve::Clothoid;
            edge.rounding = 1.0f;
            edge.clothoidRatio = ratio;
            temp.edges.push_back(edge);
        }
        previous = point.id;
    }
    const std::vector<PathStrand> strands = BuildPathStrands(temp);
    std::vector<Vec2> out;
    if (strands.empty()) return out;
    for (const PathCurveSample& sample : SamplePathStrand(temp, strands.front(), 12))
        out.push_back(mapping.ToMeters({sample.u, sample.v}));
    return out;
}

}  // namespace

std::vector<FittedControlPoint> FitClothoidControlPoints(const std::vector<std::array<float, 2>>& polylineMeters,
                                                         const PathClothoidFitOptions& options,
                                                         float* outMaxError) {
    std::vector<FittedControlPoint> result;
    if (outMaxError) *outMaxError = 0.0f;
    std::vector<Vec2> line;
    line.reserve(polylineMeters.size());
    for (const auto& p : polylineMeters) {
        const Vec2 v{p[0], p[1]};
        // 重なった点は捨てる（向きが決まらない）。
        if (line.empty() || Length(v - line.back()) > 1e-4f) line.push_back(v);
    }
    if (line.size() < 3) {
        for (size_t i = 0; i < line.size(); ++i) result.push_back({line[i].x, line[i].y, i});
        return result;
    }
    const float tolerance = std::max(options.toleranceMeters, 0.01f);
    const UvMapping mapping = MappingFor(line);
    // 升は許容誤差の 4 倍。ずれの計測は 1 輪（升 1 つぶん）までしか見ず、見つからなければ
    // 「許容誤差を超えている」とだけ分かればよい。押し出しの目標探しは見つかるまで広げる。
    const float cell = tolerance * 4.0f;
    const SegmentGrid lineGrid(line, cell);
    constexpr int kUnbounded = 1 << 20;

    // 制御点: 位置と、元の点の添字（昇順を保つ）。
    struct Control {
        Vec2 position;
        size_t source;
    };
    // 初期値は粗めに間引く（細かく残すと円弧の上に点が並び、角 1 つにまとまらない）。
    // 足りない所は後で足し、余った所は最後の間引きで消す。
    std::vector<Control> control;
    for (const size_t index : DouglasPeucker(line, tolerance * 3.0f)) control.push_back({line[index], index});

    const auto clampIndex = [&](ptrdiff_t i) {
        return static_cast<size_t>(std::clamp<ptrdiff_t>(i, 0, static_cast<ptrdiff_t>(control.size()) - 1));
    };
    // 制御点 [a, b] だけの鎖を引く。クロソイドの角は両隣の点だけで決まるので、端から 1 つ内側の角は
    // 鎖全体で引いたときと同じ形になる。局所の判定では余白を 3 つ取り、端の影響を遠ざける。
    std::vector<Vec2> positions;
    const auto sampleRange = [&](ptrdiff_t a, ptrdiff_t b) {
        positions.clear();
        for (size_t i = clampIndex(a); i <= clampIndex(b); ++i) positions.push_back(control[i].position);
        return SampleClothoidChain(positions, mapping, options.clothoidRatio);
    };
    // 角ごとに、曲線の頂（制御点に一番近い標本）を元の線へ寄せるように制御点を外へ押し出す。
    // 動かすのは [lo, hi]（両端の点は動かさない）。
    const auto relaxRange = [&](ptrdiff_t lo, ptrdiff_t hi) {
        const size_t first = std::max<size_t>(clampIndex(lo), 1);
        const size_t last = std::min(clampIndex(hi), control.size() - 2);
        if (control.size() < 3 || first > last) return;
        for (int iteration = 0; iteration < 8; ++iteration) {
            const std::vector<Vec2> curve = sampleRange(lo - 3, hi + 3);
            if (curve.size() < 2) return;
            const SegmentGrid curveGrid(curve, cell);
            float moved = 0.0f;
            for (size_t i = first; i <= last; ++i) {
                Vec2 apex, target;
                if (!curveGrid.Nearest(control[i].position, kUnbounded, nullptr, &apex, nullptr)) continue;
                if (!lineGrid.Nearest(apex, kUnbounded, nullptr, &target, nullptr)) continue;
                const Vec2 delta = (target - apex) * 0.8f;
                control[i].position = control[i].position + delta;
                moved = std::max(moved, Length(delta));
            }
            if (moved < tolerance * 0.05f) return;
        }
    };
    // 双方向のずれ。元の点ごとに「その点から曲線まで」と「その点を最寄りとする標本から元の線まで」の
    // 大きいほう。升 1 つより遠いものは升の大きさとして数える（許容誤差を超えていることだけ分かればよい）。
    // 見るのは角 [lo, hi] の周り（元の点では制御点 lo − 1 から hi + 1 の間）。
    // outErrors を渡すと元の点ごとのずれ（範囲外は 0）を返す。
    const auto measureRange = [&](ptrdiff_t lo, ptrdiff_t hi, std::vector<float>* outErrors) {
        const std::vector<Vec2> curve = sampleRange(lo - 3, hi + 3);
        if (curve.size() < 2) return std::numeric_limits<float>::max();
        const SegmentGrid curveGrid(curve, cell);
        const size_t lineFirst = lo - 1 <= 0 ? 0 : control[clampIndex(lo - 1)].source;
        const size_t lineLast = hi + 1 >= static_cast<ptrdiff_t>(control.size()) - 1 ? line.size() - 1
                                                                                     : control[clampIndex(hi + 1)].source;
        std::vector<float> error(line.size(), 0.0f);
        for (size_t i = lineFirst; i <= lineLast; ++i) {
            float distance = cell;
            curveGrid.Nearest(line[i], 1, &distance, nullptr, nullptr);
            error[i] = distance;
        }
        for (const Vec2& p : curve) {
            float distance = cell;
            size_t vertex = 0;
            if (!lineGrid.Nearest(p, 1, &distance, nullptr, &vertex) &&
                !lineGrid.Nearest(p, kUnbounded, nullptr, nullptr, &vertex))
                continue;
            if (vertex < lineFirst || vertex > lineLast) continue;
            error[vertex] = std::max(error[vertex], distance);
        }
        float maxError = 0.0f;
        for (size_t i = lineFirst; i <= lineLast; ++i) maxError = std::max(maxError, error[i]);
        if (outErrors) *outErrors = std::move(error);
        return maxError;
    };
    const ptrdiff_t kAll = 1 << 20;

    // 足す。制御点の間ごとに、許容誤差を超えた中で一番ずれた元の点を足す（1 周で何か所も足す）。
    // 元の点を全部使えば必ず収まるので、回数はその数まで。
    float maxError = 0.0f;
    for (size_t round = 0; round < line.size(); ++round) {
        relaxRange(0, kAll);
        std::vector<float> error;
        maxError = measureRange(-kAll, kAll, &error);
        if (maxError <= tolerance) break;
        std::vector<Control> added;
        for (size_t i = 0; i + 1 < control.size(); ++i) {
            size_t worst = line.size();
            for (size_t k = control[i].source + 1; k < control[i + 1].source; ++k)
                if (error[k] > tolerance && (worst == line.size() || error[k] > error[worst])) worst = k;
            if (worst != line.size()) added.push_back({line[worst], worst});
        }
        if (added.empty()) break;
        control.insert(control.end(), added.begin(), added.end());
        std::sort(control.begin(), control.end(), [](const Control& a, const Control& b) { return a.source < b.source; });
    }

    // 減らす。曲がりの小さい角から順に、消しても許容誤差に収まるなら消す。収まらなければ戻す。
    // 消した影響は両隣の角までなので、判定はその周りだけで行う。消えるものが無くなるまで繰り返す。
    for (bool removed = true; removed && control.size() > 2;) {
        removed = false;
        std::vector<std::pair<float, size_t>> candidates;  // (曲がり, 元の点の添字)
        for (size_t i = 1; i + 1 < control.size(); ++i) {
            const Vec2 in = control[i].position - control[i - 1].position;
            const Vec2 out = control[i + 1].position - control[i].position;
            const float lengths = Length(in) * Length(out);
            const float turn = lengths > 1e-12f ? std::acos(std::clamp(Dot(in, out) / lengths, -1.0f, 1.0f)) : 0.0f;
            candidates.emplace_back(turn, control[i].source);
        }
        std::sort(candidates.begin(), candidates.end());
        for (const auto& [turn, source] : candidates) {
            const auto at = std::find_if(control.begin(), control.end(), [source](const Control& c) { return c.source == source; });
            if (at == control.end() || at == control.begin() || at + 1 == control.end()) continue;
            const ptrdiff_t index = at - control.begin();
            const std::vector<Control> backup = control;
            control.erase(at);
            relaxRange(index - 1, index);
            if (measureRange(index - 2, index + 1, nullptr) <= tolerance) {
                removed = true;
            } else {
                control = backup;
            }
        }
    }
    maxError = measureRange(-kAll, kAll, nullptr);

    for (const Control& c : control) result.push_back({c.position.x, c.position.y, c.source});
    if (outMaxError) *outMaxError = maxError;
    return result;
}

bool FitStrandToClothoid(PathSettings& path, const PathStrand& strand, float sizeMeters,
                         const PathClothoidFitOptions& options, PathClothoidFitResult* outResult) {
    if (strand.closed || strand.points.size() < 3 || strand.edges.size() + 1 != strand.points.size()) return false;
    if (sizeMeters <= 0.0f) return false;

    // 元の制御点列（点 + 経路の内部点）を鎖の向きに並べる。エッジは鎖と逆向きのこともある。
    std::vector<PathPoint> source;
    for (size_t i = 0; i < strand.edges.size(); ++i) {
        const PathEdge* edge = path.FindEdge(strand.edges[i]);
        if (edge == nullptr) return false;
        std::vector<PathPoint> control = PathEdgeControlPoints(path, *edge);
        if (control.size() < 2) return false;
        if (edge->from != strand.points[i]) std::reverse(control.begin(), control.end());
        if (!source.empty()) control.erase(control.begin());  // 継ぎ目の点は前のエッジの末尾と同じ
        source.insert(source.end(), control.begin(), control.end());
    }
    if (source.size() < 3) return false;

    std::vector<std::array<float, 2>> polyline;
    polyline.reserve(source.size());
    for (const PathPoint& p : source) polyline.push_back({(p.u - 0.5f) * sizeMeters, (p.v - 0.5f) * sizeMeters});
    float maxError = 0.0f;
    const std::vector<FittedControlPoint> fitted = FitClothoidControlPoints(polyline, options, &maxError);
    if (fitted.size() < 2) return false;

    const PathElementId firstId = strand.points.front();
    const PathElementId lastId = strand.points.back();
    PathEdgeStyle style = GetPathEdgeStyle(*path.FindEdge(strand.edges.front()));
    style.curve = PathCurve::Clothoid;
    style.rounding = 1.0f;
    style.clothoidRatio = options.clothoidRatio;
    style.route = PathRoute::None;

    // 鎖のエッジと内側の点を消し、両端の間に新しい点を並べ直す。
    for (const PathElementId edgeId : strand.edges) DeletePathEdge(path, edgeId);
    for (size_t i = 1; i + 1 < strand.points.size(); ++i) DeletePathPoint(path, strand.points[i]);

    PathClothoidFitResult result;
    result.pointsBefore = source.size();
    result.pointsAfter = fitted.size();
    result.maxErrorMeters = maxError;
    PathElementId previous = firstId;
    for (size_t i = 1; i + 1 < fitted.size(); ++i) {
        const FittedControlPoint& f = fitted[i];
        const PathElementId id = AddPathPoint(path, f.x / sizeMeters + 0.5f, f.y / sizeMeters + 0.5f, previous);
        if (id == 0) return false;
        if (PathPoint* point = path.FindPoint(id)) {
            const PathPoint& from = source[std::min(f.source, source.size() - 1)];
            point->widthMeters = from.widthMeters;
            point->featherMeters = from.featherMeters;
            point->intensity = from.intensity;
            point->heightOffsetMeters = from.heightOffsetMeters;
        }
        result.interiorPoints.push_back(id);
        previous = id;
    }
    ConnectPathPoints(path, previous, lastId);

    PathElementId at = firstId;
    for (const PathElementId next : result.interiorPoints) {
        if (const PathEdge* edge = path.FindEdgeBetween(at, next)) result.edges.push_back(edge->id);
        at = next;
    }
    if (const PathEdge* edge = path.FindEdgeBetween(at, lastId)) result.edges.push_back(edge->id);
    for (PathEdge& edge : path.edges)
        if (std::find(result.edges.begin(), result.edges.end(), edge.id) != result.edges.end())
            ApplyPathEdgeStyle(edge, style);

    if (outResult) *outResult = std::move(result);
    return true;
}

}  // namespace tg::graph
