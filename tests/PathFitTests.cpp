// 密な点列の鎖をクロソイドの制御点へ置き換える（graph/PathFit）。
//
// 直線 → 円弧 → 直線の道路を 2 m おきの点で作り、少ない制御点で許容誤差の内に収まること、
// 鎖の置き換えで両端と属性が残ることを確かめる。

#include "graph/PathFit.h"

#include "TestSupport.h"

#include <array>
#include <cmath>
#include <numbers>
#include <vector>

namespace {

using namespace tg::graph;
using tg::tests::Check;
using tg::tests::Section;

// 直線 200 m → 半径 radius の 90° の円弧 → 直線 200 m。spacing おきの点。
std::vector<std::array<float, 2>> StraightArcStraight(float radius, float spacing) {
    std::vector<std::array<float, 2>> out;
    for (float x = 0.0f; x < 200.0f; x += spacing) out.push_back({x, 0.0f});
    const float arcLength = radius * std::numbers::pi_v<float> * 0.5f;
    for (float s = 0.0f; s < arcLength; s += spacing) {
        const float angle = s / radius;
        out.push_back({200.0f + radius * std::sin(angle), radius - radius * std::cos(angle)});
    }
    for (float y = 0.0f; y <= 200.0f; y += spacing) out.push_back({200.0f + radius, radius + y});
    return out;
}

}  // namespace

void RunPathFitTests() {
    Section("PathFit: 直線 → 円弧 → 直線");
    {
        const auto polyline = StraightArcStraight(80.0f, 2.0f);
        PathClothoidFitOptions options;
        options.toleranceMeters = 1.0f;
        float maxError = 0.0f;
        const auto fitted = FitClothoidControlPoints(polyline, options, &maxError);
        Check(fitted.size() >= 3 && fitted.size() <= 8, "数百の点が数点の制御点になる");
        Check(maxError <= options.toleranceMeters, "元の折れ線からのずれが許容誤差に収まる");
        Check(fitted.front().x == polyline.front()[0] && fitted.front().y == polyline.front()[1] &&
                  fitted.back().x == polyline.back()[0] && fitted.back().y == polyline.back()[1],
              "両端は動かない");
        // 角の制御点は接線の交点（曲線の外側）へ押し出される。
        bool outside = false;
        for (size_t i = 1; i + 1 < fitted.size(); ++i)
            if (fitted[i].x > 200.0f + 1.0f && fitted[i].y < 80.0f - 1.0f) outside = true;
        Check(outside, "角の制御点は元の線の外（接線の交点の側）に出る");
        // 許容誤差を狭めると点が増える。
        options.toleranceMeters = 0.1f;
        const auto tight = FitClothoidControlPoints(polyline, options, &maxError);
        Check(tight.size() >= fitted.size() && maxError <= 0.1f, "許容誤差を狭めると制御点が増え、誤差は収まる");
    }

    Section("PathFit: 鎖の置き換え");
    {
        PathSettings path;
        const float sizeMeters = 2000.0f;
        PathElementId previous = 0;
        PathElementId first = 0;
        const auto polyline = StraightArcStraight(60.0f, 4.0f);
        for (const auto& p : polyline) {
            const PathElementId id = AddPathPoint(path, p[0] / sizeMeters + 0.5f, p[1] / sizeMeters + 0.5f, previous);
            if (first == 0) first = id;
            previous = id;
        }
        const PathElementId last = previous;
        // 末尾の点に高さのずれと幅を付けておく（内側の点の属性は最寄りの元の点から来る）。
        for (PathPoint& point : path.points) {
            point.widthMeters = 6.0f;
            point.heightOffsetMeters = (point.id == last) ? 3.0f : 0.0f;
        }
        // 末尾を分岐にして別の鎖を 2 本繋いでおく（末尾が鎖の端になり、置き換えで切れないこと）。
        const PathElementId branch = AddPathPoint(path, 0.9f, 0.9f, last);
        AddPathPoint(path, 0.9f, 0.1f, last);
        const size_t pointsBefore = path.points.size();
        std::vector<PathStrand> strands = BuildPathStrands(path);
        const PathStrand* strand = nullptr;
        for (const PathStrand& s : strands)
            if (s.points.front() == first) strand = &s;
        Check(strand != nullptr && strand->points.size() == polyline.size(), "元の鎖は点の数ぶんの長さ");
        PathClothoidFitOptions options;
        options.toleranceMeters = 1.5f;
        PathClothoidFitResult result;
        Check(FitStrandToClothoid(path, *strand, sizeMeters, options, &result), "鎖を置き換えられる");
        Check(result.pointsBefore == polyline.size() && result.pointsAfter < 10 && path.points.size() < pointsBefore,
              "点が減る");
        Check(path.FindPoint(first) != nullptr && path.FindPoint(last) != nullptr &&
                  path.FindEdgeBetween(last, branch) != nullptr,
              "両端の点と、端に繋いだ別の鎖は残る");
        Check(result.edges.size() + 1 == result.interiorPoints.size() + 2, "エッジは点の数 − 1");
        bool clothoid = true, width = true;
        for (const PathElementId edgeId : result.edges) {
            const PathEdge* edge = path.FindEdge(edgeId);
            if (edge == nullptr || edge->curve != PathCurve::Clothoid || edge->rounding != 1.0f) clothoid = false;
        }
        for (const PathElementId pointId : result.interiorPoints) {
            const PathPoint* point = path.FindPoint(pointId);
            if (point == nullptr || point->widthMeters != 6.0f) width = false;
        }
        Check(clothoid, "新しいエッジはクロソイド・丸め 1.0");
        Check(width, "内側の点は元の点の幅を引き継ぐ");
        strands = BuildPathStrands(path);
        size_t strandCount = 0;
        for (const PathStrand& s : strands)
            if (s.points.front() == first && s.points.back() == last) ++strandCount;
        Check(strandCount == 1, "置き換えた鎖は 1 本につながっている");

        // 閉じた輪と 2 点の鎖は置き換えない。
        PathSettings loop;
        const PathElementId a = AddPathPoint(loop, 0.2f, 0.2f, 0);
        const PathElementId b = AddPathPoint(loop, 0.8f, 0.2f, a);
        const PathElementId c = AddPathPoint(loop, 0.5f, 0.8f, b);
        ConnectPathPoints(loop, c, a);
        const std::vector<PathStrand> loops = BuildPathStrands(loop);
        Check(loops.size() == 1 && loops.front().closed &&
                  !FitStrandToClothoid(loop, loops.front(), sizeMeters, options, nullptr),
              "閉じた輪は置き換えない");
    }
}
