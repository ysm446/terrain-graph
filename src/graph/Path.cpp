#include "graph/Path.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

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
    // 曲線の種類と丸めは、割った元のエッジから引き継ぐ（鎖の性質が途切れないように）。
    if (const PathEdge* head = path.FindEdge(edgeId)) {
        tail.curve = head->curve;
        tail.rounding = head->rounding;
        tail.clothoidRatio = head->clothoidRatio;
    }
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

// --- 鎖 --------------------------------------------------------------------

namespace {

PathElementId OtherEndOf(const PathEdge& edge, PathElementId pointId) {
    return (edge.from == pointId) ? edge.to : edge.from;
}

// pointId に付いているエッジのうち、まだ使っていないもの。
const PathEdge* NextUnvisitedEdge(const PathSettings& path, PathElementId pointId,
                                  const std::unordered_set<PathElementId>& visited) {
    for (const PathEdge& edge : path.edges) {
        if ((edge.from == pointId || edge.to == pointId) && !visited.contains(edge.id)) {
            return &edge;
        }
    }
    return nullptr;
}

// start から edge を通って、エッジが 2 本だけの点を辿り続ける。
PathStrand WalkStrand(const PathSettings& path, PathElementId start, const PathEdge* edge,
                      std::unordered_set<PathElementId>& visited) {
    PathStrand strand;
    strand.points.push_back(start);
    PathElementId current = start;
    while (edge != nullptr) {
        visited.insert(edge->id);
        strand.edges.push_back(edge->id);
        current = OtherEndOf(*edge, current);
        strand.points.push_back(current);
        if (current == start) {
            strand.closed = true;
            break;
        }
        if (path.EdgeCount(current) != 2) {
            break;
        }
        edge = NextUnvisitedEdge(path, current, visited);
    }
    return strand;
}

}  // namespace

std::vector<PathStrand> BuildPathStrands(const PathSettings& path) {
    std::vector<PathStrand> strands;
    std::unordered_set<PathElementId> visited;
    // 端と分岐から出る鎖。
    for (const PathPoint& point : path.points) {
        if (path.EdgeCount(point.id) == 2) {
            continue;
        }
        while (const PathEdge* edge = NextUnvisitedEdge(path, point.id, visited)) {
            strands.push_back(WalkStrand(path, point.id, edge, visited));
        }
    }
    // 残りは分岐の無い輪。
    for (const PathEdge& edge : path.edges) {
        if (visited.contains(edge.id)) {
            continue;
        }
        strands.push_back(WalkStrand(path, edge.from, &edge, visited));
    }
    return strands;
}

const PathStrand* FindStrandOfEdge(const std::vector<PathStrand>& strands,
                                   PathElementId edgeId) {
    for (const PathStrand& strand : strands) {
        if (std::find(strand.edges.begin(), strand.edges.end(), edgeId) != strand.edges.end()) {
            return &strand;
        }
    }
    return nullptr;
}

PathCurve StrandCurve(const PathSettings& path, const PathStrand& strand, float* outRounding,
                      bool* outMixed) {
    PathCurve curve = PathCurve::Line;
    float rounding = 1.0f;
    float ratio = 0.5f;
    bool mixed = false;
    bool first = true;
    for (const PathElementId edgeId : strand.edges) {
        const PathEdge* edge = path.FindEdge(edgeId);
        if (edge == nullptr) {
            continue;
        }
        if (first) {
            curve = edge->curve;
            rounding = edge->rounding;
            ratio = edge->clothoidRatio;
            first = false;
        } else if (edge->curve != curve || std::abs(edge->rounding - rounding) > 1e-4f ||
                   std::abs(edge->clothoidRatio - ratio) > 1e-4f) {
            mixed = true;
        }
    }
    if (outRounding != nullptr) {
        *outRounding = rounding;
    }
    if (outMixed != nullptr) {
        *outMixed = mixed;
    }
    return curve;
}

float StrandClothoidRatio(const PathSettings& path, const PathStrand& strand) {
    for (const PathElementId edgeId : strand.edges) {
        if (const PathEdge* edge = path.FindEdge(edgeId)) {
            return std::clamp(edge->clothoidRatio, 0.0f, 1.0f);
        }
    }
    return 0.5f;
}

bool ReversePathStrand(PathSettings& path, const PathStrand& strand) {
    bool any = false;
    for (const PathElementId edgeId : strand.edges) {
        any |= ReversePathEdge(path, edgeId);
    }
    return any;
}

namespace {

// 制御点の並びの「道のり」（点の添字 + 区間内の割合）で値を補間した標本。
PathCurveSample SampleAt(const std::vector<const PathPoint*>& points, float parameter, float u,
                         float v) {
    const int last = static_cast<int>(points.size()) - 1;
    const float clamped = std::clamp(parameter, 0.0f, static_cast<float>(last));
    const int i0 = std::clamp(static_cast<int>(std::floor(clamped)), 0, last);
    const int i1 = std::min(i0 + 1, last);
    const float t = clamped - static_cast<float>(i0);
    const PathPoint& a = *points[static_cast<size_t>(i0)];
    const PathPoint& b = *points[static_cast<size_t>(i1)];
    PathCurveSample sample;
    sample.u = u;
    sample.v = v;
    sample.widthMeters = Lerp(a.widthMeters, b.widthMeters, t);
    sample.featherMeters = Lerp(a.featherMeters, b.featherMeters, t);
    sample.intensity = Lerp(a.intensity, b.intensity, t);
    sample.heightOffsetMeters = Lerp(a.heightOffsetMeters, b.heightOffsetMeters, t);
    return sample;
}

// 折れ線上の位置（道のりで指定）。
void PolylineAt(const std::vector<const PathPoint*>& points, float parameter, float& outU,
                float& outV) {
    const int last = static_cast<int>(points.size()) - 1;
    const float clamped = std::clamp(parameter, 0.0f, static_cast<float>(last));
    const int i0 = std::clamp(static_cast<int>(std::floor(clamped)), 0, last);
    const int i1 = std::min(i0 + 1, last);
    const float t = clamped - static_cast<float>(i0);
    outU = Lerp(points[static_cast<size_t>(i0)]->u, points[static_cast<size_t>(i1)]->u, t);
    outV = Lerp(points[static_cast<size_t>(i0)]->v, points[static_cast<size_t>(i1)]->v, t);
}

// 2 次ベジェの連結。途中の点 Pi ごとに、両隣の線分上の「Pi から丸め × 半分」の位置
// A / B を取り、A → B を Pi を制御点にした 2 次ベジェで結ぶ。A と B の間以外は直線。
// 丸めが 1 なら A / B は線分の中点で直線部分は消え、両端の点だけを通る滑らかな線になる。
void SampleQuadratic(const std::vector<const PathPoint*>& points, float rounding,
                     int samplesPerSpan, std::vector<PathCurveSample>& out) {
    const int n = static_cast<int>(points.size());
    const float half = std::clamp(rounding, 0.0f, 1.0f) * 0.5f;
    out.push_back(SampleAt(points, 0.0f, points[0]->u, points[0]->v));
    for (int i = 1; i + 1 < n; ++i) {
        const PathPoint& prev = *points[static_cast<size_t>(i - 1)];
        const PathPoint& here = *points[static_cast<size_t>(i)];
        const PathPoint& next = *points[static_cast<size_t>(i + 1)];
        const float au = Lerp(here.u, prev.u, half);
        const float av = Lerp(here.v, prev.v, half);
        const float bu = Lerp(here.u, next.u, half);
        const float bv = Lerp(here.v, next.v, half);
        const float pa = static_cast<float>(i) - half;
        const float pb = static_cast<float>(i) + half;
        // A まで直線（A = 前の B ではないときだけ点を打つ）。
        out.push_back(SampleAt(points, pa, au, av));
        // A → B の 2 次ベジェ。
        for (int k = 1; k <= samplesPerSpan; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(samplesPerSpan);
            const float w0 = (1.0f - t) * (1.0f - t);
            const float w1 = 2.0f * (1.0f - t) * t;
            const float w2 = t * t;
            const float u = w0 * au + w1 * here.u + w2 * bu;
            const float v = w0 * av + w1 * here.v + w2 * bv;
            out.push_back(SampleAt(points, Lerp(pa, pb, t), u, v));
        }
    }
    const PathPoint& end = *points[static_cast<size_t>(n - 1)];
    out.push_back(SampleAt(points, static_cast<float>(n - 1), end.u, end.v));
}

// 3 次 B スプライン（両端を 3 重にして端の点を通す）。丸めは折れ線との混ぜ具合。
void SampleCubic(const std::vector<const PathPoint*>& points, float rounding, int samplesPerSpan,
                 std::vector<PathCurveSample>& out) {
    const int n = static_cast<int>(points.size());
    const float mix = std::clamp(rounding, 0.0f, 1.0f);
    // 制御点の添字列。端を 3 重にする。
    std::vector<int> control;
    control.push_back(0);
    control.push_back(0);
    for (int i = 0; i < n; ++i) {
        control.push_back(i);
    }
    control.push_back(n - 1);
    control.push_back(n - 1);
    const auto at = [&](size_t index) { return points[static_cast<size_t>(control[index])]; };
    out.push_back(SampleAt(points, 0.0f, points[0]->u, points[0]->v));
    for (size_t span = 0; span + 3 < control.size(); ++span) {
        const PathPoint& c0 = *at(span);
        const PathPoint& c1 = *at(span + 1);
        const PathPoint& c2 = *at(span + 2);
        const PathPoint& c3 = *at(span + 3);
        // この区間は制御点 c1 と c2 の間に当たる。道のりもその添字で取る。
        const float p1 = static_cast<float>(control[span + 1]);
        const float p2 = static_cast<float>(control[span + 2]);
        for (int k = 1; k <= samplesPerSpan; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(samplesPerSpan);
            const float t2 = t * t;
            const float t3 = t2 * t;
            const float b0 = (1.0f - t) * (1.0f - t) * (1.0f - t) / 6.0f;
            const float b1 = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
            const float b2 = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
            const float b3 = t3 / 6.0f;
            const float su = b0 * c0.u + b1 * c1.u + b2 * c2.u + b3 * c3.u;
            const float sv = b0 * c0.v + b1 * c1.v + b2 * c2.v + b3 * c3.v;
            const float parameter = Lerp(p1, p2, t);
            float lu = 0.0f;
            float lv = 0.0f;
            PolylineAt(points, parameter, lu, lv);
            out.push_back(SampleAt(points, parameter, Lerp(lu, su, mix), Lerp(lv, sv, mix)));
        }
    }
}

// --- クロソイド ----------------------------------------------------------------
//
// 角ごとに「クロソイド → 円弧 → クロソイド」を挟む（technical-notes.com の
// 「制御点からクロソイド曲線」の方式）。交角 I のうち ratio をクロソイド 2 本が
// 受け持ち（片側 τ = I × ratio / 2）、残りを半径一定の円弧が受け持つ。
// 曲率 0 の直線から連続的に曲率が立ち上がるので、道路 / 鉄道の線形になる。
//
// 単位曲線（円弧の半径 1）をローカル座標で組んでから、角の接線長が
// 「丸め × 半分」の位置（2 次ベジェの入口 / 出口と同じ）に来るように拡大する。
// クロソイドはフレネル積分の級数ではなく、曲率を数値積分して点列にする
// （τ が大きくても精度が落ちない）。

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// 曲率 κ(s) が 0 → 1（rising）または 1 → 0 で、長さ L = 2τ のクロソイドを
// origin から heading の向きに積分する。各標本の点と、最後の向きを返す。
void IntegrateClothoid(Vec2 origin, float heading, float tau, float sign, bool rising,
                       int samples, std::vector<Vec2>& out, float& outHeading) {
    const float length = 2.0f * tau;
    constexpr int kSubsteps = 8;
    const int total = samples * kSubsteps;
    const float ds = length / static_cast<float>(std::max(total, 1));
    Vec2 position = origin;
    float angle = heading;
    for (int i = 1; i <= total; ++i) {
        // 中点則。区間の真ん中の向きで進む。
        const float sMid = (static_cast<float>(i) - 0.5f) * ds;
        const float curvature = rising ? (sMid / length) : (1.0f - sMid / length);
        const float angleMid = angle + sign * curvature * ds * 0.5f;
        position.x += std::cos(angleMid) * ds;
        position.y += std::sin(angleMid) * ds;
        angle += sign * curvature * ds;
        if (i % kSubsteps == 0) {
            out.push_back(position);
        }
    }
    outHeading = angle;
}

// 角 1 つぶんの単位曲線をローカル座標（入口が原点、入りの向きが +x）で組む。
// 返り値は接線長（原点から折れ点まで）。曲線は out に、入口を含まない形で積む。
float BuildUnitClothoidCorner(float turn, float sign, float ratio, int samples,
                              std::vector<Vec2>& out) {
    const float tau = turn * ratio * 0.5f;
    const float arc = turn * (1.0f - ratio);
    Vec2 position;
    float heading = 0.0f;
    if (tau > 1e-5f) {
        IntegrateClothoid(position, heading, tau, sign, true, samples, out, heading);
        position = out.back();
    }
    if (arc > 1e-5f) {
        // 向きの左（sign が正）または右（負）に半径 1 の中心を取り、arc だけ回す。
        const Vec2 center{position.x - sign * std::sin(heading),
                          position.y + sign * std::cos(heading)};
        const float start = std::atan2(position.y - center.y, position.x - center.x);
        for (int i = 1; i <= samples; ++i) {
            const float a = start + sign * arc * (static_cast<float>(i) / samples);
            out.push_back({center.x + std::cos(a), center.y + std::sin(a)});
        }
        heading += sign * arc;
        position = out.back();
    }
    if (tau > 1e-5f) {
        IntegrateClothoid(position, heading, tau, sign, false, samples, out, heading);
    }
    if (out.empty()) {
        return 0.0f;
    }
    // 入りの接線（原点、+x）と出の接線（終点、向き heading）の交点までの距離。
    const Vec2 end = out.back();
    const float sinI = std::sin(heading);
    if (std::abs(sinI) < 1e-5f) {
        return 0.0f;
    }
    const float u = -end.y / sinI;
    return end.x + u * std::cos(heading);
}

void SampleClothoid(const std::vector<const PathPoint*>& points, float rounding, float ratio,
                    int samplesPerSpan, std::vector<PathCurveSample>& out) {
    const int n = static_cast<int>(points.size());
    const float half = std::clamp(rounding, 0.0f, 1.0f) * 0.5f;
    const float clampedRatio = std::clamp(ratio, 0.0f, 1.0f);
    out.push_back(SampleAt(points, 0.0f, points[0]->u, points[0]->v));
    for (int i = 1; i + 1 < n; ++i) {
        const PathPoint& prev = *points[static_cast<size_t>(i - 1)];
        const PathPoint& here = *points[static_cast<size_t>(i)];
        const PathPoint& next = *points[static_cast<size_t>(i + 1)];
        const float inX = here.u - prev.u;
        const float inY = here.v - prev.v;
        const float outX = next.u - here.u;
        const float outY = next.v - here.v;
        const float inLength = std::sqrt(inX * inX + inY * inY);
        const float outLength = std::sqrt(outX * outX + outY * outY);
        if (inLength <= 1e-6f || outLength <= 1e-6f) {
            out.push_back(SampleAt(points, static_cast<float>(i), here.u, here.v));
            continue;
        }
        const float dirInX = inX / inLength;
        const float dirInY = inY / inLength;
        const float dirOutX = outX / outLength;
        const float dirOutY = outY / outLength;
        const float cross = dirInX * dirOutY - dirInY * dirOutX;
        const float dot = std::clamp(dirInX * dirOutX + dirInY * dirOutY, -1.0f, 1.0f);
        const float turn = std::acos(dot);
        // 接線長は短いほうの線分に合わせる（両側とも同じ距離で入って出る）。
        const float tangent = std::min(inLength, outLength) * half;
        // ほぼ直進、または折り返しは曲線にならない。折れ点をそのまま通す。
        if (turn < 1e-3f || turn > 3.0f || tangent <= 1e-6f) {
            out.push_back(SampleAt(points, static_cast<float>(i), here.u, here.v));
            continue;
        }
        const float sign = (cross >= 0.0f) ? 1.0f : -1.0f;
        std::vector<Vec2> local;
        const float unitTangent =
            BuildUnitClothoidCorner(turn, sign, clampedRatio, samplesPerSpan, local);
        if (unitTangent <= 1e-6f || local.empty()) {
            out.push_back(SampleAt(points, static_cast<float>(i), here.u, here.v));
            continue;
        }
        const float scale = tangent / unitTangent;
        // 入口 A = 折れ点から入りの向きに tangent だけ戻った所。
        const float ax = here.u - dirInX * tangent;
        const float ay = here.v - dirInY * tangent;
        const float pa = static_cast<float>(i) - tangent / inLength;
        const float pb = static_cast<float>(i) + tangent / outLength;
        out.push_back(SampleAt(points, pa, ax, ay));
        // ローカル（+x = 入りの向き、+y = その左）をワールドへ。
        const float leftX = -dirInY;
        const float leftY = dirInX;
        for (size_t k = 0; k < local.size(); ++k) {
            const Vec2& p = local[k];
            const float u = ax + (dirInX * p.x + leftX * p.y) * scale;
            const float v = ay + (dirInY * p.x + leftY * p.y) * scale;
            const float t = static_cast<float>(k + 1) / static_cast<float>(local.size());
            out.push_back(SampleAt(points, Lerp(pa, pb, t), u, v));
        }
    }
    const PathPoint& end = *points[static_cast<size_t>(n - 1)];
    out.push_back(SampleAt(points, static_cast<float>(n - 1), end.u, end.v));
}

}  // namespace

std::vector<PathCurveSample> SamplePathStrand(const PathSettings& path, const PathStrand& strand,
                                              int samplesPerSpan) {
    std::vector<PathCurveSample> out;
    std::vector<const PathPoint*> points;
    points.reserve(strand.points.size());
    for (const PathElementId id : strand.points) {
        const PathPoint* point = path.FindPoint(id);
        if (point == nullptr) {
            return out;
        }
        points.push_back(point);
    }
    if (points.size() < 2) {
        return out;
    }
    float rounding = 1.0f;
    const PathCurve curve = StrandCurve(path, strand, &rounding, nullptr);
    if (curve == PathCurve::Line || points.size() < 3 || rounding <= 0.0f) {
        for (size_t i = 0; i < points.size(); ++i) {
            out.push_back(SampleAt(points, static_cast<float>(i), points[i]->u, points[i]->v));
        }
        return out;
    }
    const int samples = std::clamp(samplesPerSpan, 1, 64);
    if (curve == PathCurve::Quadratic) {
        SampleQuadratic(points, rounding, samples, out);
    } else if (curve == PathCurve::Clothoid) {
        SampleClothoid(points, rounding, StrandClothoidRatio(path, strand), samples, out);
    } else {
        SampleCubic(points, rounding, samples, out);
    }
    return out;
}

std::vector<compositor::PathSegment> BuildPathSegments(const PathSettings& path) {
    std::vector<compositor::PathSegment> segments;
    segments.reserve(path.edges.size() + path.points.size());
    // 評価用の分割。マスクは幅の内側が 1 になるので、粗くても縁は幅でぼける。
    constexpr int kSamplesPerSpan = 12;
    for (const PathStrand& strand : BuildPathStrands(path)) {
        const std::vector<PathCurveSample> samples = SamplePathStrand(path, strand, kSamplesPerSpan);
        for (size_t i = 0; i + 1 < samples.size(); ++i) {
            const PathCurveSample& a = samples[i];
            const PathCurveSample& b = samples[i + 1];
            compositor::PathSegment segment;
            segment.ax = a.u;
            segment.ay = a.v;
            segment.bx = b.u;
            segment.by = b.v;
            segment.widthA = a.widthMeters;
            segment.widthB = b.widthMeters;
            segment.featherA = a.featherMeters;
            segment.featherB = b.featherMeters;
            segment.intensityA = a.intensity;
            segment.intensityB = b.intensity;
            segments.push_back(segment);
        }
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
