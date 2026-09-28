#ifndef NOMINMAX
#define NOMINMAX
#endif
// Road Path の線形（地形に沿う中心線、縦断曲線、バンク角）とノードの登録を確かめる。

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
    }
}
