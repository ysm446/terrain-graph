#include "graph/RoadPath.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {
using namespace DirectX;

constexpr float kEps = 1e-5f;
// 曲率半径を求める前後の距離（m）。
constexpr float kBankCurvatureStepMeters = 10.0f;
// 中心線の点数の上限。これを超える長さなら間隔を広げる（50 km を 1 m で割れる数）。
constexpr size_t kMaxCenterlinePoints = 50000;

XMVECTOR Load(const XMFLOAT3& p) { return XMLoadFloat3(&p); }
XMFLOAT3 Store(XMVECTOR v) { XMFLOAT3 p; XMStoreFloat3(&p, v); return p; }

// XZ 平面での円弧近似の曲率半径。ほぼ直線なら 0。
float CurvatureRadiusXZ(XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2) {
    p0.y = p1.y = p2.y = 0.0f;
    const XMVECTOR chord = XMVectorSubtract(Load(p2), Load(p0));
    const float chordLength = XMVectorGetX(XMVector3Length(chord));
    if (chordLength < 1e-6f) return 0.0f;
    const XMVECTOR direction = XMVectorScale(chord, 1.0f / chordLength);
    const XMVECTOR toMid = XMVectorSubtract(Load(p1), Load(p0));
    const XMVECTOR projected = XMVectorAdd(Load(p0), XMVectorScale(direction, XMVectorGetX(XMVector3Dot(toMid, direction))));
    const float h = XMVectorGetX(XMVector3Length(XMVectorSubtract(Load(p1), projected)));
    if (h < 1e-4f) return 0.0f;
    const float radius = (chordLength * chordLength * 0.25f + h * h) / (2.0f * h);
    return radius > 1e-6f ? radius : 0.0f;
}

// 曲がる向き。進行方向に向かって右（右手系 Y-up で (-dz, 0, dx)）へ曲がるなら +1、左なら -1、直線なら 0。
// 右カーブでは外側の Left が上がるので、正のバンク（Left 側上がり）になる。
float TurnSignXZ(XMFLOAT3 p0, XMFLOAT3 p1, XMFLOAT3 p2) {
    p0.y = p1.y = p2.y = 0.0f;
    const XMVECTOR a = XMVector3Normalize(XMVectorSubtract(Load(p1), Load(p0)));
    const XMVECTOR b = XMVector3Normalize(XMVectorSubtract(Load(p2), Load(p1)));
    const XMFLOAT3 right{-XMVectorGetZ(a), 0.0f, XMVectorGetX(a)};
    const float toward = XMVectorGetX(XMVector3Dot(b, Load(right)));
    if (std::abs(toward) <= 1e-5f) return 0.0f;
    return toward > 0.0f ? 1.0f : -1.0f;
}

struct ResolvedPoint {
    float distance = 0.0f;
    float value = 0.0f;
};
float InterpolateLayer(float distance, const std::vector<ResolvedPoint>& points, float fallback) {
    if (points.empty()) return fallback;
    if (distance <= points.front().distance + kEps) return points.front().value;
    if (distance >= points.back().distance - kEps) return points.back().value;
    for (size_t i = 1; i < points.size(); ++i) {
        if (distance > points[i].distance) continue;
        const float span = points[i].distance - points[i - 1].distance;
        if (span <= kEps) return points[i].value;
        const float t = (distance - points[i - 1].distance) / span;
        return points[i - 1].value + (points[i].value - points[i - 1].value) * t;
    }
    return points.back().value;
}

float AutoBankAt(const RoadPathSettings& road, const RoadProfileCurve& curve, float distance,
                 const std::vector<ResolvedPoint>& speeds) {
    const float total = curve.TotalLength();
    const XMFLOAT3 p0 = curve.At(std::max(0.0f, distance - kBankCurvatureStepMeters));
    const XMFLOAT3 p1 = curve.At(distance);
    const XMFLOAT3 p2 = curve.At(std::min(total, distance + kBankCurvatureStepMeters));
    const float radius = CurvatureRadiusXZ(p0, p1, p2);
    const float speed = InterpolateLayer(distance, speeds, std::max(0.0f, road.designSpeedKmh));
    return ComputeAutoBankRadians(radius, speed, std::max(0.0f, road.frictionCoefficient)) *
           TurnSignXZ(p0, p1, p2);
}

// 平面の標本（実寸 XZ と高さのずれ）。
struct PlanSample {
    float x = 0.0f;
    float z = 0.0f;
    float offset = 0.0f;
};
}  // namespace

XMFLOAT3 RoadProfileCurve::At(float distance) const {
    if (points.empty()) return {};
    if (points.size() == 1 || distance <= 0.0f) return points.front();
    if (distance >= arcLengths.back()) return points.back();
    const auto it = std::upper_bound(arcLengths.begin(), arcLengths.end(), distance);
    const size_t index = static_cast<size_t>(std::max<std::ptrdiff_t>(1, it - arcLengths.begin()));
    const float span = arcLengths[index] - arcLengths[index - 1];
    const float t = span > kEps ? (distance - arcLengths[index - 1]) / span : 0.0f;
    return Store(XMVectorLerp(Load(points[index - 1]), Load(points[index]), t));
}

RoadProfileCurve BuildRoadProfileCurve(const std::vector<XMFLOAT3>& points) {
    RoadProfileCurve curve;
    curve.points = points;
    curve.arcLengths.assign(points.size(), 0.0f);
    for (size_t i = 1; i < points.size(); ++i) {
        curve.arcLengths[i] = curve.arcLengths[i - 1] +
            XMVectorGetX(XMVector3Length(XMVectorSubtract(Load(points[i]), Load(points[i - 1]))));
    }
    return curve;
}

bool BuildRoadBaseline(const RoadPathSettings& road, float sizeMeters, const RoadHeightSampler& height,
                       RoadProfileCurve& outBaseline, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    const PathSettings& path = road.path;
    if (sizeMeters <= 0.0f) return fail("地形の大きさが決まっていません");
    const auto strands = BuildPathStrands(path);
    if (strands.size() != 1 || strands.front().closed || strands.front().points.size() != path.points.size())
        return fail("分岐・閉ループ・孤立点のない 1 本の線が必要です");

    // 曲線を割った標本を実寸へ。蛇行は掛けない（sizeMeters を渡さない）。
    std::vector<PlanSample> plan;
    for (const PathCurveSample& sample : SamplePathStrand(path, strands.front(), 24)) {
        const PlanSample p{(sample.u - 0.5f) * sizeMeters, (sample.v - 0.5f) * sizeMeters,
                           sample.heightOffsetMeters};
        if (plan.empty() || std::hypot(p.x - plan.back().x, p.z - plan.back().z) > 1e-4f) plan.push_back(p);
    }
    if (plan.size() < 2) return fail("線の点が足りません");

    // 平面の道のりで等間隔に割り直す。曲線の標本は制御点の区間ごとなので、長い区間では
    // 間隔が広く、地形の高さを拾い損ねる。
    std::vector<float> along(plan.size(), 0.0f);
    for (size_t i = 1; i < plan.size(); ++i)
        along[i] = along[i - 1] + std::hypot(plan[i].x - plan[i - 1].x, plan[i].z - plan[i - 1].z);
    const float total = along.back();
    float step = kRoadCenterlineStepMeters;
    if (total / step > static_cast<float>(kMaxCenterlinePoints - 1)) step = total / (kMaxCenterlinePoints - 1);
    const auto count = static_cast<size_t>(std::floor(total / step)) + 1;

    std::vector<XMFLOAT3> centers;
    centers.reserve(count + 1);
    size_t segment = 1;
    const auto emit = [&](float distance) {
        while (segment + 1 < plan.size() && along[segment] < distance) ++segment;
        const float span = along[segment] - along[segment - 1];
        const float t = span > kEps ? std::clamp((distance - along[segment - 1]) / span, 0.0f, 1.0f) : 0.0f;
        const PlanSample& a = plan[segment - 1];
        const PlanSample& b = plan[segment];
        const float x = a.x + (b.x - a.x) * t;
        const float z = a.z + (b.z - a.z) * t;
        const float offset = a.offset + (b.offset - a.offset) * t;
        const float y = height ? height(x / sizeMeters + 0.5f, z / sizeMeters + 0.5f) : 0.0f;
        centers.push_back(XMFLOAT3{x, y + offset, z});
    };
    for (size_t i = 0; i < count; ++i) emit(static_cast<float>(i) * step);
    if (total - static_cast<float>(count - 1) * step > 1e-3f) emit(total);
    if (centers.size() < 2) return fail("線が短すぎます");
    outBaseline = BuildRoadProfileCurve(centers);
    return true;
}

std::vector<float> EvaluateVerticalProfile(const RoadPathSettings& road, const RoadProfileCurve& base) {
    std::vector<float> heights(base.points.size());
    for (size_t i = 0; i < base.points.size(); ++i) heights[i] = base.points[i].y;
    if (road.verticalPoints.empty() || base.points.size() < 2) return heights;
    const float total = base.TotalLength();
    if (total <= kEps) return heights;

    // ガイド点。始点、縦断ポイント（u 順）、終点。
    struct Guide { float x, y, vcl; };
    std::vector<Guide> guides;
    guides.push_back({0.0f, base.points.front().y, 0.0f});
    std::vector<RoadVerticalPoint> sorted = road.verticalPoints;
    std::sort(sorted.begin(), sorted.end(),
              [](const RoadVerticalPoint& a, const RoadVerticalPoint& b) { return a.u < b.u; });
    for (const RoadVerticalPoint& point : sorted) {
        const float x = std::clamp(point.u, 0.0f, 1.0f) * total;
        guides.push_back({x, base.At(x).y + point.offsetMeters, std::max(0.0f, point.vclMeters)});
    }
    guides.push_back({total, base.points.back().y, 0.0f});

    // ポイントごとのセグメント。両端は道路端、途中は隣との中点で区切る。
    struct Segment {
        float startX, endX;
        Guide p0, p1, p2;
        bool curved = false;
        float i1 = 0.0f, i2 = 0.0f, length = 0.0f, curveStartX = 0.0f, curveStartY = 0.0f, curveEndX = 0.0f;
    };
    std::vector<Segment> segments;
    const auto midpoint = [](const Guide& a, const Guide& b) {
        return Guide{(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f, 0.0f};
    };
    for (size_t i = 1; i + 1 < guides.size(); ++i) {
        Segment segment;
        segment.p0 = (i == 1) ? guides.front() : midpoint(guides[i - 1], guides[i]);
        segment.p1 = guides[i];
        segment.p2 = (i + 2 == guides.size()) ? guides.back() : midpoint(guides[i], guides[i + 1]);
        segment.startX = segment.p0.x;
        segment.endX = segment.p2.x;
        const float dx0 = segment.p1.x - segment.p0.x;
        const float dx1 = segment.p2.x - segment.p1.x;
        if (std::abs(dx0) > kEps && std::abs(dx1) > kEps) {
            segment.i1 = (segment.p1.y - segment.p0.y) / dx0;
            segment.i2 = (segment.p2.y - segment.p1.y) / dx1;
            float length = std::max(0.0f, segment.p1.vcl);
            length = std::min(length, std::abs(dx0) * 2.0f);
            length = std::min(length, std::abs(dx1) * 2.0f);
            segment.length = length;
            segment.curveStartX = segment.p1.x - length * 0.5f;
            segment.curveStartY = segment.p0.y + segment.i1 * (segment.curveStartX - segment.p0.x);
            segment.curveEndX = segment.curveStartX + length;
            segment.curved = true;
        }
        segments.push_back(segment);
    }
    const auto sampleHeight = [&](float distance) {
        const Segment* segment = &segments.back();
        for (const Segment& candidate : segments) {
            if (distance <= candidate.endX + kEps) { segment = &candidate; break; }
        }
        const Segment& s = *segment;
        if (!s.curved) {
            const float span = s.endX - s.startX;
            const float t = span > kEps ? std::clamp((distance - s.startX) / span, 0.0f, 1.0f) : 0.0f;
            return s.p0.y + (s.p2.y - s.p0.y) * t;
        }
        if (distance <= s.curveStartX) return s.p0.y + s.i1 * (distance - s.p0.x);
        if (distance >= s.curveEndX) return s.p2.y - s.i2 * (s.p2.x - distance);
        const float x = distance - s.curveStartX;
        if (s.length <= kEps) return s.curveStartY;
        return s.curveStartY + s.i1 * x - ((s.i1 - s.i2) / (2.0f * s.length)) * x * x;
    };
    for (size_t i = 0; i < base.points.size(); ++i) heights[i] = sampleHeight(base.arcLengths[i]);
    return heights;
}

bool BuildRoadCenterline(const RoadPathSettings& road, float sizeMeters, const RoadHeightSampler& height,
                         RoadProfileCurve& outCenterline, std::string* error) {
    RoadProfileCurve base;
    if (!BuildRoadBaseline(road, sizeMeters, height, base, error)) return false;
    const std::vector<float> heights = EvaluateVerticalProfile(road, base);
    std::vector<XMFLOAT3> centers = base.points;
    for (size_t i = 0; i < centers.size(); ++i) centers[i].y = heights[i];
    outCenterline = BuildRoadProfileCurve(centers);
    return true;
}

float ComputeAutoBankRadians(float radius, float designSpeedKmh, float friction) {
    constexpr float g = 9.8f;
    if (radius <= 1e-6f) return 0.0f;
    const float speed = designSpeedKmh * (1000.0f / 3600.0f);
    const float a = speed * speed / radius;
    return std::max(0.0f, std::atan2(a - g * friction, g + a * friction));
}

float EvaluateBankAngleRadiansRaw(const RoadPathSettings& road, const RoadProfileCurve& curve, float distance) {
    if (curve.points.size() < 2) return 0.0f;
    const float total = curve.TotalLength();
    struct Evaluated {
        float distance = 0.0f;
        float autoRadians = 0.0f;
        bool manual = false;
        float manualRadians = 0.0f;
    };
    std::vector<ResolvedPoint> speeds;
    std::vector<Evaluated> evaluated;
    for (const RoadBankPoint& point : road.bankPoints) {
        const float pointDistance = std::clamp(point.u, 0.0f, 1.0f) * total;
        speeds.push_back({pointDistance, std::max(0.0f, point.designSpeedKmh)});
        evaluated.push_back({pointDistance, 0.0f, point.manual,
                             XMConvertToRadians(std::clamp(point.angleDegrees, -90.0f, 90.0f))});
    }
    std::sort(speeds.begin(), speeds.end(), [](const ResolvedPoint& a, const ResolvedPoint& b) { return a.distance < b.distance; });
    std::sort(evaluated.begin(), evaluated.end(), [](const Evaluated& a, const Evaluated& b) { return a.distance < b.distance; });

    const float autoRadians = AutoBankAt(road, curve, distance, speeds);
    if (evaluated.empty()) return autoRadians;
    for (Evaluated& point : evaluated) point.autoRadians = AutoBankAt(road, curve, point.distance, speeds);

    if (evaluated.front().manual && distance <= evaluated.front().distance + kEps)
        return evaluated.front().manualRadians;
    if (evaluated.back().manual && distance >= evaluated.back().distance - kEps)
        return evaluated.back().manualRadians;
    for (size_t i = 1; i < evaluated.size(); ++i) {
        const Evaluated& prev = evaluated[i - 1];
        const Evaluated& next = evaluated[i];
        if (distance < prev.distance - kEps || distance > next.distance + kEps) continue;
        if (!prev.manual && !next.manual) break;
        const float start = prev.manual ? prev.manualRadians : prev.autoRadians;
        const float end = next.manual ? next.manualRadians : next.autoRadians;
        const float span = next.distance - prev.distance;
        if (span <= kEps) return end;
        const float t = std::clamp((distance - prev.distance) / span, 0.0f, 1.0f);
        return start + (end - start) * t;
    }
    return autoRadians;
}

float EvaluateBankAngleRadians(const RoadPathSettings& road, const RoadProfileCurve& curve, float distance) {
    const float raw = EvaluateBankAngleRadiansRaw(road, curve, distance);
    if (!road.smoothBank || road.bankSmoothMeters <= 1e-4f || curve.points.size() < 2) return raw;
    const float total = curve.TotalLength();
    const float clamped = std::clamp(distance, 0.0f, total);
    const int halfCount = std::clamp(static_cast<int>(std::ceil(road.bankSmoothMeters / 5.0f)), 2, 8);
    float weightedSum = 0.0f;
    float weightSum = 0.0f;
    for (int index = -halfCount; index <= halfCount; ++index) {
        const float normalized = static_cast<float>(index) / static_cast<float>(halfCount);
        const float sampleDistance = std::clamp(clamped + normalized * road.bankSmoothMeters, 0.0f, total);
        constexpr float kSigma = 0.45f;
        const float weight = std::exp(-0.5f * normalized * normalized / (kSigma * kSigma));
        weightedSum += EvaluateBankAngleRadiansRaw(road, curve, sampleDistance) * weight;
        weightSum += weight;
    }
    return weightSum > 1e-5f ? weightedSum / weightSum : raw;
}

RoadProfileFrame EvaluateRoadProfileFrame(const RoadPathSettings& road, const RoadProfileCurve& centerline,
                                          float u) {
    RoadProfileFrame frame;
    if (centerline.points.size() < 2) return frame;
    const float total = centerline.TotalLength();
    frame.distance = std::clamp(u, 0.0f, 1.0f) * total;
    frame.position = centerline.At(frame.distance);
    const float step = std::min(1.0f, total * 0.5f);
    const XMVECTOR a = Load(centerline.At(std::max(0.0f, frame.distance - step)));
    const XMVECTOR b = Load(centerline.At(std::min(total, frame.distance + step)));
    XMVECTOR tangent = XMVectorSubtract(b, a);
    if (XMVectorGetX(XMVector3Length(tangent)) < 1e-6f) tangent = XMVectorSet(0, 0, 1, 0);
    tangent = XMVector3Normalize(tangent);
    frame.tangent = Store(tangent);
    // 進行方向に向かって右（右手系 Y-up）。
    XMFLOAT3 right{-frame.tangent.z, 0.0f, frame.tangent.x};
    const float horizontal = std::hypot(right.x, right.z);
    if (horizontal < 1e-6f) right = {1.0f, 0.0f, 0.0f};
    else { right.x /= horizontal; right.z /= horizontal; }
    frame.right = right;
    frame.up = Store(XMVector3Normalize(XMVector3Cross(Load(right), tangent)));
    frame.bankRadians = road.bankEnabled ? EvaluateBankAngleRadians(road, centerline, frame.distance) : 0.0f;
    return frame;
}

PathElementId AddVerticalPoint(RoadPathSettings& road, float u) {
    RoadVerticalPoint point;
    point.id = road.path.nextId++;
    point.u = std::clamp(u, 0.0f, 1.0f);
    road.verticalPoints.push_back(point);
    return point.id;
}

PathElementId AddBankPoint(RoadPathSettings& road, float u) {
    RoadBankPoint point;
    point.id = road.path.nextId++;
    point.u = std::clamp(u, 0.0f, 1.0f);
    point.designSpeedKmh = road.designSpeedKmh;
    road.bankPoints.push_back(point);
    return point.id;
}

bool DeleteRoadProfilePoint(RoadPathSettings& road, PathElementId id) {
    const size_t before = road.verticalPoints.size() + road.bankPoints.size();
    std::erase_if(road.verticalPoints, [id](const RoadVerticalPoint& p) { return p.id == id; });
    std::erase_if(road.bankPoints, [id](const RoadBankPoint& p) { return p.id == id; });
    return road.verticalPoints.size() + road.bankPoints.size() != before;
}

RoadVerticalPoint* FindVerticalPoint(RoadPathSettings& road, PathElementId id) {
    for (auto& point : road.verticalPoints) if (point.id == id) return &point;
    return nullptr;
}

RoadBankPoint* FindBankPoint(RoadPathSettings& road, PathElementId id) {
    for (auto& point : road.bankPoints) if (point.id == id) return &point;
    return nullptr;
}

}  // namespace tg::graph
