#include "graph/RoadMarking.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {
using namespace DirectX;

// 頂点数の上限（1 本の道路の 1 種類）。
constexpr size_t kMaxMarkingVertices = 4u * 1024u * 1024u;

XMVECTOR Load(const XMFLOAT3& v) { return XMLoadFloat3(&v); }

// 路面の格子。行の左右の端（列 0 が左端、最後の列が右端）と道のり。
struct RoadSurface {
    const renderer::MeshData* mesh = nullptr;
    uint32_t stride = 0;
    size_t rows = 0;
    float width = 0.0f;
    std::vector<float> distances;  // 行ごとの道のり（路面の UV の y）

    const renderer::MeshVertex& At(size_t row, uint32_t column) const {
        return mesh->vertices[row * stride + column];
    }
};

// 路面の上の点（道のりと左端からの横位置）。行の間は線形に補う。
struct SurfacePoint {
    XMVECTOR position;
    XMVECTOR normal;
    XMVECTOR across;  // 左端 → 右端（幅ぶんの長さ）
};
SurfacePoint Sample(const RoadSurface& road, float distance, float x) {
    const auto upper = std::upper_bound(road.distances.begin(), road.distances.end(), distance);
    const size_t hi = std::clamp<size_t>(static_cast<size_t>(upper - road.distances.begin()), 1, road.rows - 1);
    const size_t lo = hi - 1;
    const float span = road.distances[hi] - road.distances[lo];
    const float t = span > 1e-6f ? std::clamp((distance - road.distances[lo]) / span, 0.0f, 1.0f) : 0.0f;
    const auto lerp = [&](uint32_t column, bool normal) {
        const renderer::MeshVertex& a = road.At(lo, column);
        const renderer::MeshVertex& b = road.At(hi, column);
        return normal ? XMVectorLerp(Load(a.normal), Load(b.normal), t) : XMVectorLerp(Load(a.position), Load(b.position), t);
    };
    const uint32_t last = road.stride - 1;
    const XMVECTOR left = lerp(0, false), right = lerp(last, false);
    const float s = std::clamp(x / road.width, 0.0f, 1.0f);
    SurfacePoint point;
    point.across = XMVectorSubtract(right, left);
    point.position = XMVectorAdd(left, XMVectorScale(point.across, s));
    point.normal = XMVector3Normalize(XMVectorLerp(lerp(0, true), lerp(last, true), s));
    return point;
}

// 帯を作る。横位置 center を中心に幅 width、道のり [start, end] を、間の路面の行ごとに刻む。
void AddStrip(const RoadSurface& road, const RoadMarkingSettings& settings, float center, float width,
              float start, float end, renderer::MeshData& out) {
    const uint32_t first = static_cast<uint32_t>(out.vertices.size());
    const float repeat = std::max(settings.uvRepeatMeters, 0.01f);
    const auto addRing = [&](float distance) {
        for (int side = 0; side < 2; ++side) {
            const float x = center + (side == 0 ? -width : width) * 0.5f;
            const SurfacePoint point = Sample(road, distance, x);
            renderer::MeshVertex vertex{};
            XMStoreFloat3(&vertex.position, XMVectorAdd(point.position, XMVectorScale(point.normal, settings.liftMeters)));
            XMStoreFloat3(&vertex.normal, point.normal);
            // 右（左端 → 右端）と前（道のりの増える向き）。前 = 法線 × 右（右手系）。
            const XMVECTOR right = XMVector3Normalize(XMVectorSubtract(
                point.across, XMVectorScale(point.normal, XMVectorGetX(XMVector3Dot(point.normal, point.across)))));
            const XMVECTOR forward = XMVector3Cross(point.normal, right);
            // 横は線の幅いっぱいで 1 周（U = 0〜繰り返し長。材質は繰り返し長で割る）、長さの向きは道のり。
            // 接線は +U の向き。従法線 cross(N, T) × w が +V の向きになるよう w を決める。
            if (settings.uvAlongU) {
                vertex.uv = {distance, static_cast<float>(side) * repeat};
                XMStoreFloat4(&vertex.tangent, forward);
                vertex.tangent.w = -1.0f;
            } else {
                vertex.uv = {static_cast<float>(side) * repeat, distance};
                XMStoreFloat4(&vertex.tangent, right);
                vertex.tangent.w = 1.0f;
            }
            out.vertices.push_back(vertex);
        }
    };
    addRing(start);
    for (const float distance : road.distances)
        if (distance > start + 1e-4f && distance < end - 1e-4f) addRing(distance);
    addRing(end);
    const uint32_t rings = (static_cast<uint32_t>(out.vertices.size()) - first) / 2;
    for (uint32_t ring = 1; ring < rings; ++ring) {
        const uint32_t a = first + (ring - 1) * 2;
        out.indices.insert(out.indices.end(), {a, a + 2, a + 1, a + 1, a + 2, a + 3});
    }
}
}  // namespace

RoadLaneLayout ComputeRoadLaneLayout(const RoadMeshSettings& mesh) {
    RoadLaneLayout layout;
    const float width = std::clamp(mesh.widthMeters, kRoadMinWidthMeters, kRoadMaxWidthMeters);
    const uint32_t forward = static_cast<uint32_t>(std::max(mesh.lanesForward, 1));
    const uint32_t backward = static_cast<uint32_t>(std::max(mesh.lanesBackward, 0));
    const uint32_t total = std::min(forward + backward, 16u);
    layout.laneWidthMeters = width / static_cast<float>(total);
    // 車線は右端から並べる（轍のマスクと同じ）。右側の車線の数は、左側通行なら対向、右側通行なら進行方向。
    const uint32_t rightSideCount = mesh.leftHandTraffic ? backward : forward;
    for (uint32_t i = 1; i < total; ++i) {
        const float x = width - layout.laneWidthMeters * static_cast<float>(i);
        if (i == rightSideCount && backward > 0) {
            layout.hasCenter = true;
            layout.centerMeters = x;
        } else {
            layout.dividerMeters.push_back(x);
        }
    }
    return layout;
}

bool BuildRoadMarkings(const renderer::MeshData& road, uint32_t stride, const RoadMeshSettings& mesh,
                       const RoadMarkingSettings& settings,
                       std::array<renderer::MeshData, kRoadMarkingKindCount>& out, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    for (auto& m : out) m = {};
    if (stride < 2 || road.vertices.size() < static_cast<size_t>(stride) * 2 || road.vertices.size() % stride != 0)
        return fail("路面が作られていません");
    RoadSurface surface;
    surface.mesh = &road;
    surface.stride = stride;
    surface.rows = road.vertices.size() / stride;
    surface.width = std::clamp(mesh.widthMeters, kRoadMinWidthMeters, kRoadMaxWidthMeters);
    surface.distances.resize(surface.rows);
    for (size_t row = 0; row < surface.rows; ++row) surface.distances[row] = surface.At(row, 0).uv.y;
    const float total = surface.distances.back();
    if (!(total > 0.0f)) return fail("路面の延長が 0 です");

    if (!std::isfinite(settings.liftMeters) || settings.liftMeters < 0.0f || settings.liftMeters > 0.1f)
        return fail("浮かせる量は 0〜0.1 m にしてください");
    if (!std::isfinite(settings.uvRepeatMeters) || settings.uvRepeatMeters < 0.1f || settings.uvRepeatMeters > 100.0f)
        return fail("繰り返し長は 0.1〜100 m にしてください");

    // 引く線（左端からの横位置、幅、破線か、種類）。
    struct Strip {
        float center, width;
        bool dashed;
        RoadMarkingKind kind;
    };
    std::vector<Strip> strips;
    const RoadLaneLayout lanes = ComputeRoadLaneLayout(mesh);
    const auto& center = settings.lines[static_cast<size_t>(RoadMarkingKind::Center)];
    const auto& edge = settings.lines[static_cast<size_t>(RoadMarkingKind::Edge)];
    const auto& lane = settings.lines[static_cast<size_t>(RoadMarkingKind::Lane)];
    if (center.enabled && lanes.hasCenter)
        strips.push_back({lanes.centerMeters, center.widthMeters, center.dashed, RoadMarkingKind::Center});
    if (edge.enabled) {
        const float inset = settings.edgeInsetMeters;
        if (!std::isfinite(inset) || inset < edge.widthMeters * 0.5f)
            return fail("外側線が道路の外に出ます。端からの距離を線の幅の半分以上にしてください");
        if (inset + edge.widthMeters * 0.5f > surface.width * 0.5f)
            return fail("外側線が中心を越えています。端からの距離を小さくしてください");
        strips.push_back({inset, edge.widthMeters, edge.dashed, RoadMarkingKind::Edge});
        strips.push_back({surface.width - inset, edge.widthMeters, edge.dashed, RoadMarkingKind::Edge});
    }
    if (lane.enabled)
        for (const float divider : lanes.dividerMeters)
            strips.push_back({divider, lane.widthMeters, lane.dashed, RoadMarkingKind::Lane});

    for (size_t i = 0; i < strips.size(); ++i) {
        const Strip& s = strips[i];
        if (!std::isfinite(s.width) || s.width < 0.05f || s.width > 1.0f) return fail("線の幅は 0.05〜1 m にしてください");
        if (s.kind != RoadMarkingKind::Edge && lanes.laneWidthMeters < s.width * 2.0f)
            return fail("車線の幅に対して線が太すぎます。車線数か線の幅を見直してください");
        if (s.center - s.width * 0.5f < -1e-4f || s.center + s.width * 0.5f > surface.width + 1e-4f)
            return fail("線が道路の外に出ます。線の幅か端からの距離を見直してください");
        for (size_t j = 0; j < i; ++j)
            if (std::abs(s.center - strips[j].center) < (s.width + strips[j].width) * 0.5f)
                return fail("線どうしが重なります。線の幅か端からの距離を見直してください");
    }
    const bool dashed = std::any_of(strips.begin(), strips.end(), [](const Strip& s) { return s.dashed; });
    if (dashed && (!std::isfinite(settings.dashLengthMeters) || settings.dashLengthMeters < 0.1f ||
                   !std::isfinite(settings.dashGapMeters) || settings.dashGapMeters < 0.0f))
        return fail("破線の長さは 0.1 m 以上、間隔は 0 m 以上にしてください");

    for (const Strip& s : strips) {
        renderer::MeshData& target = out[static_cast<size_t>(s.kind)];
        if (!s.dashed) {
            AddStrip(surface, settings, s.center, s.width, 0.0f, total, target);
        } else {
            const float period = settings.dashLengthMeters + settings.dashGapMeters;
            for (float start = 0.0f; start < total; start += period) {
                const float end = std::min(start + settings.dashLengthMeters, total);
                if (end - start < 1e-3f) break;
                AddStrip(surface, settings, s.center, s.width, start, end, target);
                if (target.vertices.size() > kMaxMarkingVertices) return fail("区画線の頂点が多すぎます");
            }
        }
        if (target.vertices.size() > kMaxMarkingVertices) return fail("区画線の頂点が多すぎます");
    }
    return true;
}

}  // namespace tg::graph
