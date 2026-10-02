#ifndef NOMINMAX
#define NOMINMAX
#endif
// Road Path の線形（地形に沿う中心線、縦断曲線、バンク角）とノードの登録を確かめる。

#include "compositor/MeshFootprint.h"
#include "graph/NodeGraph.h"
#include "graph/RoadMarking.h"
#include "graph/RoadMesh.h"
#include "graph/RoadPath.h"

#include "TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <variant>

namespace {

using namespace tg::graph;
using tg::tests::Check;
using tg::tests::Section;

constexpr float kSize = 1000.0f;  // 地形の一辺（m）

bool Near(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }

// (0.1, 0.5) → (0.9, 0.5) の直線 1 本（X 方向に 800 m）。
RoadPathSettings StraightRoad() {
    RoadPathSettings road;
    const PathElementId a = AddPathPoint(road.path, 0.1f, 0.5f, 0);
    AddPathPoint(road.path, 0.9f, 0.5f, a);
    return road;
}

}  // namespace

void RunRoadPathTests() {
    Section("Road Path: 地形に沿う中心線");
    {
        const RoadPathSettings road = StraightRoad();
        // 地形は X 方向に 5% の上り（U が 0.1 増えるごとに 5 m）。
        const RoadHeightSampler slope = [](float u, float) { return (u - 0.5f) * kSize * 0.05f; };
        RoadProfileCurve base;
        std::string error;
        Check(BuildRoadBaseline(road, kSize, slope, base, &error), "1 本の線から中心線を作る");
        Check(Near(base.TotalLength(), 800.0f, 1.0f), "中心線の長さは平面の長さ（800 m）");
        Check(base.points.size() >= 800, "中心線は 1 m ごとに割る");
        Check(Near(base.points.front().y, -20.0f, 0.01f) && Near(base.points.back().y, 20.0f, 0.01f),
              "縦断ポイントが無ければ高さは地形に沿う");
        const std::vector<float> heights = EvaluateVerticalProfile(road, base);
        bool same = heights.size() == base.points.size();
        for (size_t i = 0; same && i < heights.size(); ++i) same = Near(heights[i], base.points[i].y, 1e-4f);
        Check(same, "縦断ポイントが無ければ縦断で高さを変えない");

        RoadPathSettings offset = road;
        offset.path.points.front().heightOffsetMeters = 3.0f;
        offset.path.points.back().heightOffsetMeters = 3.0f;
        RoadProfileCurve raised;
        BuildRoadBaseline(offset, kSize, slope, raised, nullptr);
        Check(Near(raised.At(400.0f).y, base.At(400.0f).y + 3.0f, 0.01f), "点の高さのずれは地形へ足す");
    }

    Section("Road Path: 縦断ポイントの自動作成");
    {
        // 3 km の道。0〜1 km は平ら、1〜2 km で 150 m 登る山、2〜3 km は平ら。そこに 1 m ごとの細かい凹凸。
        std::vector<DirectX::XMFLOAT3> points;
        for (int i = 0; i <= 3000; ++i) {
            const float d = static_cast<float>(i);
            float y = 0.0f;
            if (d > 1000.0f && d < 2000.0f) y = 150.0f * std::sin((d - 1000.0f) / 1000.0f * 3.14159265f);
            y += std::sin(d * 0.9f) * 1.5f;
            points.push_back({d, y, 0.0f});
        }
        const RoadProfileCurve base = BuildRoadProfileCurve(points);
        RoadVerticalAutoParams params;
        const std::vector<RoadVerticalPoint> generated = GenerateVerticalPoints(base, params);
        Check(generated.size() >= 2 && generated.size() <= 20, "山の登り下りに数個の特徴点を取る（細かい凹凸は拾わない）");
        bool ordered = true;
        bool spaced = true;
        const float length = base.TotalLength();
        for (size_t i = 0; i < generated.size(); ++i) {
            const float x = generated[i].u * length;
            if (i > 0) {
                const float gap = x - generated[i - 1].u * length;
                ordered &= gap > 0.0f;
                spaced &= gap >= params.minSpacingMeters - 0.5f;
            }
            spaced &= x >= params.minSpacingMeters * 0.5f - 0.5f && length - x >= params.minSpacingMeters * 0.5f - 0.5f;
        }
        Check(ordered && spaced, "点は道のりの順で、間隔（両端とは半分）を空ける");
        Check(!generated.empty() && Near(generated.front().vclMeters, params.vclMeters, 1e-3f), "縦断曲線長は指定の値");

        // 交点を結ぶ勾配は最大勾配以下（両端の差が収まる範囲なので、全区間で収まる）。
        // 位置 u は道のり（凹凸を含む 3D の長さ）の割合。勾配は水平距離（ここでは x）で測る。
        const float total = base.TotalLength();
        const auto hinge = [&](size_t k) {
            const DirectX::XMFLOAT3 at = base.At(generated[k].u * total);
            return std::pair<float, float>{at.x, at.y + generated[k].offsetMeters};
        };
        std::vector<std::pair<float, float>> guides{{0.0f, base.points.front().y}};
        for (size_t k = 0; k < generated.size(); ++k) guides.push_back(hinge(k));
        guides.push_back({3000.0f, base.points.back().y});
        float steepest = 0.0f;
        for (size_t k = 1; k < guides.size(); ++k) {
            steepest = std::max(steepest, std::abs(guides[k].second - guides[k - 1].second) /
                                              (guides[k].first - guides[k - 1].first));
        }
        Check(steepest <= params.maxGradePercent / 100.0f + 1e-3f, "交点を結ぶ勾配は最大勾配以下");
        // 山は 500 m で 150 m 登る（30 %）ので、10 % では頂上に届かない。一番高い交点が山の真ん中の
        // あたりにあり、上限の勾配で登った高さ（約 50 m）になっていれば形を捉えている。
        std::pair<float, float> highest{0.0f, -1e9f};
        for (size_t k = 0; k < generated.size(); ++k) {
            if (hinge(k).second > highest.second) highest = hinge(k);
        }
        Check(highest.first > 1200.0f && highest.first < 1800.0f && highest.second > 40.0f,
              "一番高い交点は山の真ん中で、勾配の上限で登れる高さ");

        // 勾配の上限を緩めると交点は地形（ならした高さ）に近い。
        RoadVerticalAutoParams loose = params;
        loose.maxGradePercent = 100.0f;
        bool close = true;
        for (const RoadVerticalPoint& point : GenerateVerticalPoints(base, loose)) {
            close &= std::abs(point.offsetMeters) < 5.0f;
        }
        Check(close, "勾配の上限が効かなければ、交点はならした地形の上（ずれは凹凸の範囲）");

        RoadPathSettings road = StraightRoad();
        AddVerticalPoint(road, 0.5f);
        const PathElementId next = road.path.nextId;
        ReplaceVerticalPoints(road, generated);
        Check(road.verticalPoints.size() == generated.size() && road.verticalPoints.front().id == next,
              "置き換えると今のポイントは消え、新しい ID を振る");
        Check(GenerateVerticalPoints(RoadProfileCurve{}, params).empty(), "空の中心線では何も作らない");
    }

    Section("Road Path: 縦断曲線");
    {
        RoadPathSettings road = StraightRoad();
        // でこぼこの地形。縦断ポイントを置けば、その間は放物線でつながる（地形を無視する）。
        const RoadHeightSampler bumpy = [](float u, float) { return std::sin(u * 80.0f) * 5.0f; };
        const PathElementId id = AddVerticalPoint(road, 0.5f);
        FindVerticalPoint(road, id)->offsetMeters = 10.0f;
        FindVerticalPoint(road, id)->vclMeters = 0.0f;
        RoadProfileCurve base;
        RoadProfileCurve centerline;
        const RoadHeightSampler height = bumpy;
        Check(BuildRoadBaseline(road, kSize, height, base, nullptr), "でこぼこの地形でも中心線を作る");
        const std::vector<float> heights = EvaluateVerticalProfile(road, base);
        const size_t mid = base.points.size() / 2;
        Check(Near(heights.front(), base.points.front().y, 1e-3f) && Near(heights.back(), base.points.back().y, 1e-3f),
              "両端は地形の高さのまま");
        Check(Near(heights[mid], base.At(base.arcLengths[mid]).y + 10.0f, 0.5f) ||
                  Near(heights[mid], base.At(400.0f).y + 10.0f, 0.5f),
              "縦断曲線長 0 ならポイントの高さ（地形 + 10 m）を通る");
        // 始点からポイントまでは直線（一定勾配）。
        const float grade1 = (heights[100] - heights[0]) / base.arcLengths[100];
        const float grade2 = (heights[300] - heights[200]) / (base.arcLengths[300] - base.arcLengths[200]);
        Check(Near(grade1, grade2, 1e-3f), "ポイントの間は一定の勾配");

        FindVerticalPoint(road, id)->vclMeters = 200.0f;
        const std::vector<float> curved = EvaluateVerticalProfile(road, base);
        Check(std::abs(curved[mid] - heights[mid]) > 0.5f, "縦断曲線長を付けると頂点が丸まる");
        Check(Near(curved[50], heights[50], 1e-3f), "縦断曲線の外は同じ勾配のまま");

        Check(BuildRoadCenterline(road, kSize, height, centerline, nullptr) &&
                  Near(centerline.At(400.0f).y, curved[mid], 0.5f),
              "中心線に縦断が反映される");
        Check(DeleteRoadProfilePoint(road, id) && road.verticalPoints.empty(), "縦断ポイントを消せる");
    }

    Section("Road Path: バンク角");
    {
        Check(ComputeAutoBankRadians(0.0f, 60.0f, 0.15f) == 0.0f, "直線（半径 0）は傾けない");
        Check(ComputeAutoBankRadians(50.0f, 60.0f, 0.15f) > ComputeAutoBankRadians(200.0f, 60.0f, 0.15f),
              "急なカーブほど大きく傾ける");
        // +X へ進んでから +Z へ曲がる（進行方向の右 = +Z 側へ曲がる右カーブ）。
        RoadPathSettings road;
        const PathElementId a = AddPathPoint(road.path, 0.1f, 0.5f, 0);
        const PathElementId b = AddPathPoint(road.path, 0.5f, 0.5f, a);
        AddPathPoint(road.path, 0.5f, 0.9f, b);
        for (PathEdge& edge : road.path.edges) edge.curve = PathCurve::Quadratic;
        road.bankEnabled = true;
        road.designSpeedKmh = 80.0f;
        const RoadHeightSampler flat = [](float, float) { return 0.0f; };
        RoadProfileCurve centerline;
        Check(BuildRoadCenterline(road, kSize, flat, centerline, nullptr), "曲線の道路の中心線を作る");
        const float mid = centerline.TotalLength() * 0.5f;
        Check(EvaluateBankAngleRadians(road, centerline, mid) > 0.0f, "右カーブは正（左側が上がる）");
        const PathElementId manual = AddBankPoint(road, 0.5f);
        FindBankPoint(road, manual)->manual = true;
        FindBankPoint(road, manual)->angleDegrees = -4.0f;
        Check(Near(EvaluateBankAngleRadians(road, centerline, mid), -4.0f * 3.14159265f / 180.0f, 1e-3f),
              "手動のポイントの位置では指定した角度");
        Check(manual < road.path.nextId && road.path.FindPoint(manual) == nullptr,
              "ポイントの ID は path の番号の空間から振る");
    }

    Section("Road Path: 1 本の線でないとき");
    {
        RoadPathSettings road;
        const PathElementId a = AddPathPoint(road.path, 0.2f, 0.5f, 0);
        const PathElementId b = AddPathPoint(road.path, 0.5f, 0.5f, a);
        AddPathPoint(road.path, 0.8f, 0.5f, b);
        AddPathPoint(road.path, 0.5f, 0.8f, b);  // 分岐
        RoadProfileCurve base;
        std::string error;
        Check(!BuildRoadBaseline(road, kSize, nullptr, base, &error) && !error.empty(), "分岐した線は断る");
    }

    Section("Road Mesh: 路面のメッシュ");
    {
        const RoadPathSettings road = StraightRoad();
        const RoadHeightSampler flat = [](float, float) { return 10.0f; };
        RoadProfileCurve centerline;
        BuildRoadCenterline(road, kSize, flat, centerline, nullptr);
        RoadMeshSettings settings;
        settings.widthMeters = 7.0f;
        settings.surfaceOffsetMeters = 0.05f;
        tg::renderer::MeshData mesh;
        std::string error;
        Check(BuildRoadMesh(road, centerline, settings, mesh, &error), "直線の道路から路面を作る");
        const size_t stride = 8;  // 幅 7 m は 7 列（8 頂点）
        Check(mesh.vertices.size() == centerline.points.size() * stride, "幅方向は約 1 m ごと（7 m で 8 頂点）");
        Check(mesh.indices.size() == (centerline.points.size() - 1) * 7 * 6, "四角ごとに三角形 2 つ");
        const auto& first = mesh.vertices.front();
        const auto& last = mesh.vertices[stride - 1];
        Check(Near(std::abs(first.position.z - last.position.z), 7.0f, 1e-3f) && Near(first.position.x, last.position.x, 1e-3f),
              "断面は進行方向に直角で、幅の分だけ広がる");
        Check(Near(first.position.y, 10.05f, 1e-4f), "路面は中心線から持ち上げる");
        Check(Near(first.uv.x, 0.0f, 1e-4f) && Near(last.uv.x, 7.0f, 1e-4f), "UV の x は左端からの横位置（m）");
        Check(Near(mesh.vertices[stride * 100].uv.y, centerline.arcLengths[100], 1e-3f), "UV の y は道のり（m）");
        bool up = true;
        for (const auto& v : mesh.vertices) up = up && v.normal.y > 0.99f;
        Check(up, "平らな道路の法線は上向き");
        // 進行方向の右は (-dz, 0, dx)（Road Path のバンクと同じ）。+X へ進む道路の右は +Z 側で、
        // 左端（列 0）は -Z 側。
        Check(first.position.z < last.position.z, "列 0 が左端、最後の列が右端");

        RoadPathSettings banked;
        const PathElementId a = AddPathPoint(banked.path, 0.1f, 0.5f, 0);
        const PathElementId b = AddPathPoint(banked.path, 0.5f, 0.5f, a);
        AddPathPoint(banked.path, 0.5f, 0.9f, b);
        for (PathEdge& edge : banked.path.edges) edge.curve = PathCurve::Quadratic;
        banked.bankEnabled = true;
        banked.designSpeedKmh = 80.0f;
        RoadProfileCurve curve;
        BuildRoadCenterline(banked, kSize, flat, curve, nullptr);
        Check(BuildRoadMesh(banked, curve, settings, mesh, &error), "バンクの付いた曲線の道路から路面を作る");
        const size_t mid = curve.points.size() / 2;
        const float leftY = mesh.vertices[mid * stride].position.y;
        const float rightY = mesh.vertices[mid * stride + stride - 1].position.y;
        Check(leftY > rightY + 0.05f, "右カーブでは左端が上がる");

        RoadPathSettings sharp;
        const PathElementId p = AddPathPoint(sharp.path, 0.2f, 0.5f, 0);
        const PathElementId q = AddPathPoint(sharp.path, 0.5f, 0.5f, p);
        AddPathPoint(sharp.path, 0.3f, 0.52f, q);  // ほぼ折り返す
        RoadProfileCurve bent;
        BuildRoadCenterline(sharp, kSize, flat, bent, nullptr);
        Check(!BuildRoadMesh(sharp, bent, settings, mesh, &error) && !error.empty(), "折り返すような角は断る");
    }

    Section("Shoulder: 路肩の帯");
    {
        const RoadPathSettings road = StraightRoad();
        const RoadHeightSampler flat = [](float, float) { return 10.0f; };
        RoadProfileCurve centerline;
        BuildRoadCenterline(road, kSize, flat, centerline, nullptr);
        RoadMeshSettings settings;
        settings.widthMeters = 7.0f;
        settings.surfaceOffsetMeters = 0.0f;
        tg::renderer::MeshData surface;
        BuildRoadMesh(road, centerline, settings, surface, nullptr);
        const uint32_t stride = RoadMeshStride(settings);
        Check(stride == 8, "路面の 1 行の頂点数（幅 7 m で 8）");

        RoadShoulderSettings shoulder;
        shoulder.widthMeters = 2.0f;
        shoulder.crossSlopePercent = 5.0f;
        tg::renderer::MeshData left;
        uint32_t leftStride = 0;
        std::string error;
        Check(BuildRoadShoulder(surface, stride, 0, 1, shoulder, left, leftStride, &error), "左の端から路肩を作る");
        Check(leftStride == 3, "路肩は約 1 m ごと（2 m で 3 頂点）");
        const auto& edge = surface.vertices[0];
        const auto& inner = left.vertices[0];
        const auto& outer = left.vertices[leftStride - 1];
        Check(Near(inner.position.x, edge.position.x, 1e-5f) && Near(inner.position.y, edge.position.y, 1e-5f) &&
                  Near(inner.position.z, edge.position.z, 1e-5f),
              "路肩の内側の端は路面の端と同じ頂点（隙間が無い）");
        Check(Near(std::abs(outer.position.z - edge.position.z), 2.0f, 1e-3f) && outer.position.z < edge.position.z,
              "左の路肩は外（左）へ幅の分だけ張り出す");
        Check(Near(outer.position.y, edge.position.y - 0.1f, 1e-4f), "横断勾配 5% で 2 m 先は 10 cm 下がる");
        Check(Near(outer.uv.x, std::hypot(2.0f, 0.1f), 1e-4f) && Near(left.vertices[leftStride * 50].uv.y, surface.vertices[stride * 50].uv.y, 1e-4f),
              "UV の x は内側の端から断面に沿った長さ、y は路面と同じ道のり");
        bool up = true;
        for (const auto& v : left.vertices) up = up && v.normal.y > 0.99f;
        Check(up, "路肩の法線は上向き");
        // 接線（+U）と w から作る従法線は道のりの増える向き（+X）を向く。
        const auto& v = left.vertices[leftStride * 10 + 1];
        const float bx = (v.normal.y * v.tangent.z - v.normal.z * v.tangent.y) * v.tangent.w;
        Check(bx > 0.9f, "左の路肩でも従法線は進行方向");

        tg::renderer::MeshData right;
        uint32_t rightStride = 0;
        Check(BuildRoadShoulder(surface, stride, stride - 1, stride - 2, shoulder, right, rightStride, &error) &&
                  right.vertices[rightStride - 1].position.z > surface.vertices[stride - 1].position.z,
              "右の端からは右へ張り出す");

        RoadShoulderSettings step = shoulder;
        step.stepHeightMeters = 0.15f;
        tg::renderer::MeshData curb;
        uint32_t curbStride = 0;
        Check(BuildRoadShoulder(surface, stride, 0, 1, step, curb, curbStride, &error) && curbStride == 5 &&
                  Near(curb.vertices[1].position.y, edge.position.y - 0.15f - 0.05f * 0.05f, 1e-4f) &&
                  Near(curb.vertices[2].position.y, curb.vertices[1].position.y, 1e-6f),
              "段差は面取りの列を足し、そこで段差の分だけ下げる（角は列を重ねる）");

        // 断面の点: 縁石 15 cm で上がり、2 m の歩道。
        RoadShoulderSettings walk;
        walk.shape = RoadShoulderShape::Section;
        walk.section = ShoulderSectionTemplate(RoadSectionTemplate::Sidewalk);
        Check(ValidateShoulderSection(walk.section, &error), "歩道のひな形は正しい断面");
        tg::renderer::MeshData sidewalk;
        uint32_t walkStride = 0;
        Check(BuildRoadShoulder(surface, stride, 0, 1, walk, sidewalk, walkStride, &error) && walkStride == 5,
              "縁石の立ち上がり 1 列 + 角の重ね 1 列 + 歩道 2 m を 1 m ごと");
        const auto& kerbTop = sidewalk.vertices[1];
        Check(Near(kerbTop.position.x, edge.position.x, 1e-5f) && Near(kerbTop.position.z, edge.position.z, 1e-5f) &&
                  Near(kerbTop.position.y, edge.position.y + 0.15f, 1e-5f),
              "縁石の上端は端の真上 15 cm");
        Check(sidewalk.vertices[stride * 0].normal.z > 0.99f, "縁石の立ち上がりの面は道路側（左の路肩では +Z）を向く");
        Check(sidewalk.vertices[2].normal.y > 0.99f, "角の外の列は歩道の面の法線（上向き）");
        Check(Near(sidewalk.vertices[walkStride - 1].uv.x, 0.15f + std::hypot(2.0f, 0.04f), 1e-4f),
              "UV の x は断面に沿った長さ");
        tg::renderer::MeshData beyond;
        uint32_t beyondStride = 0;
        Check(BuildRoadShoulder(sidewalk, walkStride, walkStride - 1, walkStride - 2, shoulder, beyond, beyondStride,
                                &error),
              "歩道の外側の端からさらに路肩を重ねられる");

        RoadShoulderSettings gutter = walk;
        gutter.section = ShoulderSectionTemplate(RoadSectionTemplate::Gutter);
        tg::renderer::MeshData ditch;
        uint32_t ditchStride = 0;
        Check(BuildRoadShoulder(surface, stride, 0, 1, gutter, ditch, ditchStride, &error),
              "側溝のひな形（縦の面が下りと上りの両方）も作れる");
        Check(ValidateShoulderSection(ShoulderSectionTemplate(RoadSectionTemplate::SoilShoulder), &error),
              "土の路肩のひな形は正しい断面");
        Check(!ValidateShoulderSection({{1.0f, 0.0f}, {0.5f, 0.0f}}, &error), "外への距離が戻る断面は断る");
        Check(!ValidateShoulderSection({{0.0f, 0.2f}, {0.0f, 0.0f}, {1.0f, 0.0f}}, &error), "縦に折り返す断面は断る");
        Check(Near(ShoulderSectionLength(shoulder), std::hypot(2.0f, 0.1f), 1e-4f), "勾配の形の断面の長さ");

        tg::renderer::MeshData outer2;
        uint32_t outerStride = 0;
        Check(BuildRoadShoulder(left, leftStride, leftStride - 1, leftStride - 2, shoulder, outer2, outerStride, &error) &&
                  Near(outer2.vertices[outerStride - 1].position.z, edge.position.z - 4.0f, 1e-3f),
              "路肩の外側の端からさらに路肩を重ねられる");
    }

    Section("Shoulder: 区間の切り替え");
    {
        RoadShoulderSettings shoulder;
        shoulder.material = 1;
        std::vector<RoadShoulderSpan> spans = ShoulderSpans(shoulder, 100.0f);
        Check(spans.size() == 1 && spans[0].startMeters == 0.0f && spans[0].material == 1, "切り替えが無ければ区間は 1 つ");

        RoadShoulderSwitch late{60.0f, 4.0f, 3, 2.0f, "", ""};
        RoadShoulderSwitch early{20.0f, 6.0f, 2, 2.0f, "b.tgboundary", "uid-b"};
        shoulder.switches = {late, early};
        spans = ShoulderSpans(shoulder, 100.0f);
        Check(spans.size() == 3 && spans[1].startMeters == 20.0f && spans[1].material == 2 && spans[2].material == 3,
              "切り替えは道のりの順に並べる");
        Check(spans[1].boundaryUid == "uid-b" && spans[2].boundaryUid.empty(), "区間ごとに境界を持つ（なしへも替わる）");
        Check(spans[0].transitionMeters == 0.0f && spans[1].transitionMeters == 6.0f && spans[2].transitionMeters == 4.0f,
              "移行距離は区間の長さに収まれば設定のまま");

        shoulder.switches = {{5.0f, 30.0f, 2, 2.0f, "", ""}, {12.0f, 30.0f, 3, 2.0f, "", ""}};
        spans = ShoulderSpans(shoulder, 100.0f);
        Check(spans.size() == 3 && spans[1].transitionMeters == 5.0f && spans[2].transitionMeters == 7.0f,
              "移行距離は前後の区間の長さまで（隣の移行と重ならない）");

        shoulder.switches = {{150.0f, 2.0f, 2, 2.0f, "", ""}};
        spans = ShoulderSpans(shoulder, 100.0f);
        Check(spans.size() == 2 && spans[1].startMeters == 100.0f && spans[1].transitionMeters == 0.0f,
              "道路より先の切り替えは終点に寄せ、移行は持たない");

        shoulder.switches = {{30.0f, 2.0f, 2, 2.0f, "", ""}, {30.0f, 2.0f, 3, 2.0f, "", ""}};
        spans = ShoulderSpans(shoulder, 100.0f);
        Check(spans.size() == 2 && spans[1].material == 3, "同じ位置の切り替えは後のものを使う");

        shoulder.switches = {{0.0f, 2.0f, 4, 2.0f, "", ""}};
        spans = ShoulderSpans(shoulder, 100.0f);
        Check(spans.size() == 1 && spans[0].material == 4 && spans[0].transitionMeters == 0.0f,
              "始点の切り替えは最初の区間を置き換える");
    }

    Section("Lane Marking: 区画線");
    {
        const RoadPathSettings road = StraightRoad();
        const RoadHeightSampler flat = [](float, float) { return 10.0f; };
        RoadProfileCurve centerline;
        BuildRoadCenterline(road, kSize, flat, centerline, nullptr);
        RoadMeshSettings mesh;
        mesh.surfaceOffsetMeters = 0.0f;
        tg::renderer::MeshData surface;
        BuildRoadMesh(road, centerline, mesh, surface, nullptr);
        const uint32_t stride = RoadMeshStride(mesh);
        const float leftZ = surface.vertices[0].position.z;  // 左端（+X へ向かう道路では -Z 側）

        const RoadLaneLayout twoWay = ComputeRoadLaneLayout(mesh);
        Check(twoWay.hasCenter && Near(twoWay.centerMeters, 3.5f, 1e-5f) && twoWay.dividerMeters.empty(),
              "片側 1 車線の対面通行: 中央線が真ん中、車線境界線は無い");

        RoadMarkingSettings settings;
        std::array<tg::renderer::MeshData, kRoadMarkingKindCount> lines;
        std::string error;
        Check(BuildRoadMarkings(surface, stride, mesh, settings, lines, &error), "既定の設定で区画線を作る");
        const auto& centerMesh = lines[static_cast<size_t>(RoadMarkingKind::Center)];
        const auto& edgeMesh = lines[static_cast<size_t>(RoadMarkingKind::Edge)];
        Check(!centerMesh.indices.empty() && !edgeMesh.indices.empty() &&
                  lines[static_cast<size_t>(RoadMarkingKind::Lane)].indices.empty(),
              "中央線と外側線はあり、車線境界線は無い");
        float minX = 1e9f, maxX = -1e9f;
        bool lifted = true, acrossUv = true;
        for (const auto& v : centerMesh.vertices) {
            minX = std::min(minX, v.position.z - leftZ);
            maxX = std::max(maxX, v.position.z - leftZ);
            lifted = lifted && Near(v.position.y, 10.0f + settings.liftMeters, 1e-4f);
            acrossUv = acrossUv && (Near(v.uv.x, 0.0f, 1e-6f) || Near(v.uv.x, settings.uvRepeatMeters, 1e-6f));
        }
        Check(Near(minX, 3.5f - 0.075f, 1e-3f) && Near(maxX, 3.5f + 0.075f, 1e-3f), "中央線は幅 0.15 m で道路の真ん中");
        Check(lifted, "区画線は路面から浮かせる量だけ上");
        Check(acrossUv, "横の UV は線の幅いっぱいで 1 周（0 と繰り返し長）");
        float edgeMin = 1e9f;
        for (const auto& v : edgeMesh.vertices) edgeMin = std::min(edgeMin, v.position.z - leftZ);
        Check(Near(edgeMin, 0.5f - 0.075f, 1e-3f), "外側線は端から 0.5 m の所が中心");

        RoadMeshSettings oneWay = mesh;
        oneWay.lanesForward = 2;
        oneWay.lanesBackward = 0;
        const RoadLaneLayout lanes = ComputeRoadLaneLayout(oneWay);
        Check(!lanes.hasCenter && lanes.dividerMeters.size() == 1, "一方通行 2 車線: 中央線は無く、車線境界線が 1 本");
        Check(BuildRoadMarkings(surface, stride, oneWay, settings, lines, &error), "一方通行の区画線を作る");
        const auto& laneMesh = lines[static_cast<size_t>(RoadMarkingKind::Lane)];
        bool inDash = !laneMesh.vertices.empty();
        for (const auto& v : laneMesh.vertices) inDash = inDash && std::fmod(v.uv.y, 10.0f) <= 5.0f + 1e-3f;
        Check(inDash && lines[static_cast<size_t>(RoadMarkingKind::Center)].indices.empty(),
              "車線境界線は破線（5 m の線と 5 m の間隔）");

        RoadMarkingSettings wide = settings;
        wide.edgeInsetMeters = 3.5f;
        Check(!BuildRoadMarkings(surface, stride, mesh, wide, lines, &error) && !error.empty(),
              "外側線が中心を越える設定は断る");
        RoadMarkingSettings thick = settings;
        thick.lines[static_cast<size_t>(RoadMarkingKind::Center)].widthMeters = 1.0f;
        thick.edgeInsetMeters = 3.0f;
        Check(!BuildRoadMarkings(surface, stride, mesh, thick, lines, &error), "線どうしが重なる設定は断る");
    }

    Section("Road Path: ノード");
    {
        NodeGraph graph;
        const GraphId roadNode = graph.CreateNode(NodeKind::RoadPath);
        const Node* node = graph.FindNode(roadNode);
        Check(node != nullptr && std::holds_alternative<RoadPathNodeSettings>(node->settings), "Road Path を作れる");
        Check(node != nullptr && node->outputs.size() == 1 && node->outputs[0].valueType == ValueType::RoadPath,
              "出力は Road Path の型");
        Check(node != nullptr && EditablePathSettings(*node) != nullptr, "平面の点とエッジを Path と同じに編集できる");
        Check(node != nullptr && EditablePathSettings(*node)->defaultWidthMeters == kRoadDefaultWidthMeters,
              "新しい点の幅は道路の既定");
        Check(IsPathLikeNodeKind(NodeKind::RoadPath) && IsPreviewableNodeKind(NodeKind::RoadPath),
              "Path と同じく編集とプレビューの対象");
        // ノードを足すと並びが作り直されうるので、ピンは足した後に引き直す。
        const GraphId maskPath = graph.CreateNode(NodeKind::MaskPath);
        Check(!graph.CreateLink(graph.FindNode(roadNode)->outputs[0].id, graph.FindNode(maskPath)->inputs[0].id),
              "Road Path は Mask Path（Path の型）へ繋がらない");
        const GraphId meshNode = graph.CreateNode(NodeKind::RoadMesh);
        const GraphId outputNode = graph.CreateNode(NodeKind::MeshOutput);
        Check(graph.CompileRoadMeshes().empty(), "繋いでいなければ描く道路は無い");
        Check(graph.CreateLink(graph.FindNode(roadNode)->outputs[0].id, graph.FindNode(meshNode)->inputs[0].id),
              "Road Path を Road Mesh へ繋げる");
        Check(graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(outputNode)->inputs[0].id),
              "Road Mesh を Mesh Output へ繋げる");
        const auto compiled = graph.CompileRoadMeshes();
        Check(compiled.size() == 1 && compiled[0].roadMesh == meshNode && compiled[0].roadPath == roadNode,
              "Mesh Output から Road Mesh と Road Path を辿る");
        const GraphId modelOutput = graph.CreateNode(NodeKind::ModelOutput);
        Check(!graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(modelOutput)->inputs[0].id),
              "Mesh は Model Output（Instances）へ繋がらない");
        const GraphId shoulderA = graph.CreateNode(NodeKind::Shoulder);
        const GraphId shoulderB = graph.CreateNode(NodeKind::Shoulder);
        graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(shoulderA)->inputs[0].id);
        graph.CreateLink(graph.FindNode(shoulderA)->outputs[0].id, graph.FindNode(shoulderB)->inputs[0].id);
        Check(graph.CreateLink(graph.FindNode(shoulderB)->outputs[0].id, graph.FindNode(outputNode)->inputs[0].id),
              "路肩を 2 つ挟んで Mesh Output へ繋ぐ");
        const auto chained = graph.CompileRoadMeshes();
        Check(chained.size() == 1 && chained[0].roadMesh == meshNode && chained[0].shoulders.size() == 2 &&
                  chained[0].shoulders[0] == shoulderA && chained[0].shoulders[1] == shoulderB,
              "路肩を Road Mesh に近い順に辿る");
        const GraphId marking = graph.CreateNode(NodeKind::LaneMarking);
        graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(marking)->inputs[0].id);
        Check(graph.CreateLink(graph.FindNode(marking)->outputs[0].id, graph.FindNode(shoulderA)->inputs[0].id),
              "Lane Marking を Road Mesh と路肩の間に挟める");
        const auto marked = graph.CompileRoadMeshes();
        Check(marked.size() == 1 && marked[0].roadMesh == meshNode && marked[0].markings.size() == 1 &&
                  marked[0].markings[0] == marking && marked[0].shoulders.size() == 2,
              "区画線を挟んでも Road Mesh まで辿り、区画線と路肩を分けて返す");
        Check(marked[0].drawn && marked[0].maskNodes.empty(), "Mesh Output の鎖は描く。足跡の読み手は無い");
    }

    Section("Mask Mesh: メッシュの足跡");
    {
        // Road Path → Road Mesh → Shoulder → Mask Mesh → Surface の Mask。Mesh Output は繋がない。
        NodeGraph graph;
        const GraphId baseNode = graph.CreateNode(NodeKind::Heightmap);
        const GraphId roadNode = graph.CreateNode(NodeKind::RoadPath);
        const GraphId meshNode = graph.CreateNode(NodeKind::RoadMesh);
        const GraphId shoulderNode = graph.CreateNode(NodeKind::Shoulder);
        const GraphId maskNode = graph.CreateNode(NodeKind::MaskMesh);
        const GraphId surfaceNode = graph.CreateNode(NodeKind::Surface);
        Check(graph.FindNode(maskNode) != nullptr &&
                  std::holds_alternative<MaskNodeSettings>(graph.FindNode(maskNode)->settings) &&
                  graph.FindNode(maskNode)->inputs.size() == 1 &&
                  graph.FindNode(maskNode)->inputs[0].valueType == ValueType::Mesh &&
                  graph.FindNode(maskNode)->outputs.size() == 1 &&
                  graph.FindNode(maskNode)->outputs[0].valueType == ValueType::Mask,
              "Mask Mesh はマスクのノードで、Mesh を受けて Mask を出す");
        const GraphId maskPath = graph.CreateNode(NodeKind::MaskPath);
        Check(!graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(maskPath)->inputs[0].id),
              "Mesh は Mask Path（Path の型）へ繋がらない");
        graph.CreateLink(graph.FindNode(baseNode)->outputs[0].id, graph.FindNode(roadNode)->inputs[0].id);
        graph.CreateLink(graph.FindNode(roadNode)->outputs[0].id, graph.FindNode(meshNode)->inputs[0].id);
        graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(shoulderNode)->inputs[0].id);
        Check(graph.CreateLink(graph.FindNode(shoulderNode)->outputs[0].id, graph.FindNode(maskNode)->inputs[0].id),
              "Shoulder の Mesh を Mask Mesh へ繋げる");
        graph.CreateLink(graph.FindNode(baseNode)->outputs[0].id, graph.FindNode(surfaceNode)->inputs[0].id);
        Check(graph.CreateLink(graph.FindNode(maskNode)->outputs[0].id, graph.FindNode(surfaceNode)->inputs[1].id),
              "Mask Mesh の Mask を Surface へ繋げる");

        // Mesh Output が無くても、Mask Mesh が終端の鎖として形を作る（描かない）。
        const auto chains = graph.CompileRoadMeshes();
        Check(chains.size() == 1 && chains[0].roadMesh == meshNode && chains[0].roadPath == roadNode &&
                  chains[0].shoulders.size() == 1 && chains[0].shoulders[0] == shoulderNode && !chains[0].drawn &&
                  chains[0].output == maskNode && chains[0].maskNodes.size() == 1 && chains[0].maskNodes[0] == maskNode,
              "Mask Mesh だけの鎖は描かずに形を作り、Mask Mesh を足跡の読み手として返す");

        // Mask Mesh の op。足跡のキーは Mask Mesh 自身の ID。
        const CompiledGraph compiled = graph.CompileLayersTo(surfaceNode);
        const auto& layerMask = compiled.layers.back().mask;
        Check(compiled.layers.size() == 2 && layerMask.source == tg::compositor::MaskSource::Node &&
                  layerMask.maskOp >= 0 &&
                  compiled.maskOps[static_cast<size_t>(layerMask.maskOp)].kind == tg::compositor::MaskOpKind::Mesh &&
                  compiled.maskOps[static_cast<size_t>(layerMask.maskOp)].meshSource ==
                      static_cast<uint32_t>(maskNode),
              "Mask Mesh は Mesh の op になり、足跡のキーは自分の ID");

        // 同じ Shoulder を Mesh Output へも繋ぐと、描く鎖 1 本にまとまり、Mask Mesh はその読み手になる。
        const GraphId outputNode = graph.CreateNode(NodeKind::MeshOutput);
        graph.CreateLink(graph.FindNode(shoulderNode)->outputs[0].id, graph.FindNode(outputNode)->inputs[0].id);
        const auto merged = graph.CompileRoadMeshes();
        Check(merged.size() == 1 && merged[0].drawn && merged[0].output == outputNode &&
                  merged[0].maskNodes.size() == 1 && merged[0].maskNodes[0] == maskNode,
              "Mesh Output と同じメッシュに繋いだ Mask Mesh は、描く鎖の足跡を読む");

        // 鎖の途中（Road Mesh）に繋いだ Mask Mesh は別の鎖（路肩なし）として形を作る。
        const GraphId innerMask = graph.CreateNode(NodeKind::MaskMesh);
        graph.CreateLink(graph.FindNode(meshNode)->outputs[0].id, graph.FindNode(innerMask)->inputs[0].id);
        const auto split = graph.CompileRoadMeshes();
        size_t innerChains = 0;
        for (const auto& chain : split) {
            if (chain.output == innerMask) {
                ++innerChains;
                Check(!chain.drawn && chain.shoulders.empty() && chain.roadMesh == meshNode,
                      "Road Mesh に直接繋いだ Mask Mesh は路肩を含まない足跡になる");
            }
        }
        Check(split.size() == 2 && innerChains == 1, "鎖の途中に繋いだ Mask Mesh は別の（描かない）鎖になる");

        // Mesh 入力が無い Mask Mesh は未接続（レイヤーは自分の定数マスク）。
        NodeGraph empty;
        const GraphId emptyMask = empty.CreateNode(NodeKind::MaskMesh);
        const GraphId emptyBase = empty.CreateNode(NodeKind::Heightmap);
        const GraphId emptySurface = empty.CreateNode(NodeKind::Surface);
        empty.CreateLink(empty.FindNode(emptyBase)->outputs[0].id, empty.FindNode(emptySurface)->inputs[0].id);
        empty.CreateLink(empty.FindNode(emptyMask)->outputs[0].id, empty.FindNode(emptySurface)->inputs[1].id);
        const CompiledGraph emptyCompiled = empty.CompileLayersTo(emptySurface);
        Check(emptyCompiled.layers.size() == 2 && emptyCompiled.layers.back().mask.maskOp < 0,
              "Mesh 入力の無い Mask Mesh は未接続と同じ扱い");
        Check(empty.CompileRoadMeshes().empty(), "Mesh 入力の無い Mask Mesh は鎖を作らない");
    }

    Section("Mask Mesh: 足跡の置き場");
    {
        tg::compositor::MeshFootprintStore store;
        const uint64_t initial = store.Revision();
        tg::compositor::MeshFootprint footprint;
        footprint.vertices = {{0.1f, 0.1f, 0.5f}, {0.2f, 0.1f, 0.5f}, {0.1f, 0.2f, 0.6f}};
        footprint.indices = {0, 1, 2};
        Check(store.Set(7, footprint) && store.Revision() == initial + 1 && store.Find(7) != nullptr &&
                  store.Find(7)->TriangleCount() == 1 && store.Find(7)->hash != 0,
              "足跡を置くと世代が進み、ハッシュが付く");
        Check(!store.Set(7, footprint) && store.Revision() == initial + 1, "同じ中身を置き直しても世代は進まない");
        tg::compositor::MeshFootprint jitter = footprint;
        jitter.vertices[0].u += 1e-7f;  // 浮動小数の揺れ（1/65536 より小さい）
        Check(!store.Set(7, jitter) && store.Revision() == initial + 1, "座標の微かな揺れでは世代は進まない");
        tg::compositor::MeshFootprint moved = footprint;
        moved.vertices[0].u += 0.01f;
        Check(store.Set(7, moved) && store.Revision() == initial + 2, "形が変われば世代が進む");
        Check(store.Find(8) == nullptr && !store.Remove(8) && store.Revision() == initial + 2,
              "無いキーの取り除きは何もしない");
        Check(store.Remove(7) && store.Find(7) == nullptr && store.Revision() == initial + 3 && store.Count() == 0,
              "取り除くと世代が進む");
    }
}
