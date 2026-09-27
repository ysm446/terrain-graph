#ifndef NOMINMAX
#define NOMINMAX
#endif
// Road Path の線形（地形に沿う中心線、縦断曲線、バンク角）とノードの登録を確かめる。

#include "graph/NodeGraph.h"
#include "graph/RoadPath.h"

#include "TestSupport.h"

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
    }
}
