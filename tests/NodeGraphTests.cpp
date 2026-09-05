// ノードグラフから評価用レイヤー列へのコンパイルを確かめる。
// GPU 評価の前段だけを対象にし、入力を外したときに古い結果を残さない規則を固定する。

#include "graph/NodeGraph.h"

#include "TestSupport.h"

#include <array>
#include <cmath>

namespace {

using tg::graph::NodeGraph;
using tg::graph::NodeKind;
using tg::tests::Check;
using tg::tests::Section;

bool IsNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.size() != 1) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

bool StartsWithNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.empty()) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

}  // namespace

void RunNodeGraphTests() {
    Section("ノードグラフ — 入力のないハイト加工");

    constexpr std::array kOperationKinds = {
        NodeKind::Blur,      NodeKind::Sediment, NodeKind::Crumbling,
        NodeKind::Snow,      NodeKind::River,    NodeKind::Droplet,
    };
    for (const NodeKind kind : kOperationKinds) {
        NodeGraph graph;
        const tg::graph::GraphId operationId = graph.CreateNode(kind);
        Check(IsNeutralPlane(graph.CompileLayersTo(operationId)),
              "Base 未接続の加工ノードは変位 0 の平面になる");
    }

    {
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId blurId = graph.CreateNode(NodeKind::Blur);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* blur = graph.FindNode(blurId);
        const bool connected = base != nullptr && blur != nullptr && !base->outputs.empty() &&
                               !blur->inputs.empty() &&
                               graph.CreateLink(base->outputs.front().id, blur->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(blurId);
        Check(connected && compiled.layers.size() == 2 &&
                  compiled.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  compiled.layers.back().kind == tg::compositor::LayerKind::Blur,
              "Base 接続中の加工ノードは入力と加工を保つ");
    }

    Section("ノードグラフ — Sediment の Emission 入力");
    {
        // Emission に繋いだマスクは、堆積レイヤーの Mask 入力（供給元）として op へ落ちる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId noiseId = graph.CreateNode(NodeKind::MaskNoise);
        const tg::graph::GraphId sedimentId = graph.CreateNode(NodeKind::Sediment);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* noise = graph.FindNode(noiseId);
        const tg::graph::Node* sediment = graph.FindNode(sedimentId);
        const bool hasPins = base != nullptr && noise != nullptr && sediment != nullptr &&
                             !base->outputs.empty() && !noise->outputs.empty() &&
                             sediment->inputs.size() == 2 &&
                             sediment->inputs[1].valueType == tg::graph::ValueType::Mask;
        const bool connected =
            hasPins && graph.CreateLink(base->outputs.front().id, sediment->inputs[0].id) &&
            graph.CreateLink(noise->outputs.front().id, sediment->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(sedimentId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().kind == tg::compositor::LayerKind::Sediment &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size();
        Check(connected && wired, "Sediment の Emission 入力は供給元のマスク op になる");

        // 繋がなければ供給元は無し（全面へ一様）。
        NodeGraph plain;
        const tg::graph::GraphId plainBaseId = plain.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId plainSedimentId = plain.CreateNode(NodeKind::Sediment);
        const tg::graph::Node* plainBase = plain.FindNode(plainBaseId);
        const tg::graph::Node* plainSediment = plain.FindNode(plainSedimentId);
        const bool plainConnected =
            plainBase != nullptr && plainSediment != nullptr && !plainBase->outputs.empty() &&
            !plainSediment->inputs.empty() &&
            plain.CreateLink(plainBase->outputs.front().id, plainSediment->inputs[0].id);
        const tg::graph::CompiledGraph plainCompiled = plain.CompileLayersTo(plainSedimentId);
        Check(plainConnected && plainCompiled.layers.size() == 2 &&
                  plainCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "Emission 未接続の Sediment は供給元を持たない");
    }

    Section("ノードグラフ — Snow の Mask 入力");
    {
        // Mask に繋いだマスクは、積雪レイヤーの Mask 入力（降らせる場所）として op へ落ちる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId heightId = graph.CreateNode(NodeKind::MaskHeight);
        const tg::graph::GraphId snowId = graph.CreateNode(NodeKind::Snow);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* height = graph.FindNode(heightId);
        const tg::graph::Node* snow = graph.FindNode(snowId);
        const bool hasPins = base != nullptr && height != nullptr && snow != nullptr &&
                             !base->outputs.empty() && !height->outputs.empty() &&
                             snow->inputs.size() == 2 &&
                             snow->inputs[1].valueType == tg::graph::ValueType::Mask;
        const bool connected =
            hasPins && graph.CreateLink(base->outputs.front().id, snow->inputs[0].id) &&
            graph.CreateLink(height->outputs.front().id, snow->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(snowId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().kind == tg::compositor::LayerKind::Snow &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size();
        Check(connected && wired, "Snow の Mask 入力は降らせる場所のマスク op になる");

        // 繋がなければ全面へ一様。
        NodeGraph plain;
        const tg::graph::GraphId plainBaseId = plain.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId plainSnowId = plain.CreateNode(NodeKind::Snow);
        const tg::graph::Node* plainBase = plain.FindNode(plainBaseId);
        const tg::graph::Node* plainSnow = plain.FindNode(plainSnowId);
        const bool plainConnected =
            plainBase != nullptr && plainSnow != nullptr && !plainBase->outputs.empty() &&
            !plainSnow->inputs.empty() &&
            plain.CreateLink(plainBase->outputs.front().id, plainSnow->inputs[0].id);
        const tg::graph::CompiledGraph plainCompiled = plain.CompileLayersTo(plainSnowId);
        Check(plainConnected && plainCompiled.layers.size() == 2 &&
                  plainCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "Mask 未接続の Snow は降らせる場所を持たない");
    }

    Section("パス — まとめて動かす / コピーと貼り付け");
    {
        using tg::graph::PathClip;
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        PathSettings path;
        const PathElementId a = tg::graph::AddPathPoint(path, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(path, 0.4f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(path, 0.4f, 0.4f, b);
        const PathElementId lone = tg::graph::AddPathPoint(path, 0.9f, 0.9f, 0);
        const tg::graph::PathEdge* ab = path.FindEdgeBetween(a, b);
        const tg::graph::PathEdge* bc = path.FindEdgeBetween(b, c);
        Check(ab != nullptr && bc != nullptr && lone != 0, "3 点の鎖と孤立点を作れる");

        // まとめて動かす。0〜1 へ丸める。
        const bool moved = tg::graph::MovePathPoints(path, {a, b, c}, 0.1f, -0.3f);
        const tg::graph::PathPoint* pa = path.FindPoint(a);
        const tg::graph::PathPoint* pc = path.FindPoint(c);
        Check(moved && pa != nullptr && pc != nullptr && std::abs(pa->u - 0.3f) < 1e-5f &&
                  pa->v == 0.0f && std::abs(pc->u - 0.5f) < 1e-5f && std::abs(pc->v - 0.1f) < 1e-5f,
              "MovePathPoints は指定した点だけを動かし、0〜1 へ丸める");
        float cu = 0.0f;
        float cv = 0.0f;
        Check(tg::graph::PathPointsCentroid(path, {a, b, c}, cu, cv) &&
                  std::abs(cu - (0.3f + 0.5f + 0.5f) / 3.0f) < 1e-5f,
              "PathPointsCentroid は重心を返す");

        // 鎖を切り出す。エッジは両端の点を連れていき、内部点は持ち越さない。
        if (ab != nullptr && bc != nullptr) {
            tg::graph::PathEdge* mutableAb = const_cast<tg::graph::PathEdge*>(ab);
            mutableAb->routed = true;
            mutableAb->waypoints.push_back({0.35f, 0.1f});
            mutableAb->curve = tg::graph::PathCurve::Cubic;
        }
        PathClip clip;
        const bool extracted = tg::graph::ExtractPathClip(path, {}, {ab->id, bc->id}, clip);
        Check(extracted && clip.points.size() == 3 && clip.edges.size() == 2 &&
                  !clip.edges.front().routed && clip.edges.front().waypoints.empty() &&
                  clip.edges.front().curve == tg::graph::PathCurve::Cubic,
              "ExtractPathClip は鎖の点とエッジを切り出し、内部点は捨てて曲線の性質は残す");

        // 点の集合から切り出すと、その間のエッジだけが付いてくる。
        PathClip pointClip;
        Check(tg::graph::ExtractPathClip(path, {a, b, lone}, {}, pointClip) &&
                  pointClip.points.size() == 3 && pointClip.edges.size() == 1,
              "点の集合の ExtractPathClip は点どうしを結ぶエッジだけを拾う");

        // 貼り付け。ID は振り直され、ずらした位置に同じ形で入る。
        const size_t pointsBefore = path.points.size();
        const size_t edgesBefore = path.edges.size();
        std::vector<PathElementId> pastedPoints;
        std::vector<PathElementId> pastedEdges;
        const bool pasted =
            tg::graph::PastePathClip(path, clip, 0.2f, 0.5f, &pastedPoints, &pastedEdges);
        bool idsFresh = true;
        for (const PathElementId id : pastedPoints) {
            idsFresh &= (id != a && id != b && id != c && id != lone);
        }
        const tg::graph::PathPoint* firstPasted =
            pastedPoints.empty() ? nullptr : path.FindPoint(pastedPoints.front());
        Check(pasted && path.points.size() == pointsBefore + 3 &&
                  path.edges.size() == edgesBefore + 2 && pastedEdges.size() == 2 && idsFresh &&
                  firstPasted != nullptr && std::abs(firstPasted->u - 0.5f) < 1e-5f &&
                  std::abs(firstPasted->v - 0.5f) < 1e-5f &&
                  path.FindEdgeBetween(pastedPoints[0], pastedPoints[1]) != nullptr,
              "PastePathClip は新しい ID で同じ形を、ずらした位置に貼る");
        Check(tg::graph::BuildPathStrands(path).size() == 2,
              "貼った鎖は元の鎖と別の鎖になる");
    }

    Section("ノードグラフ — Path の Base");
    {
        NodeGraph graph;
        const tg::graph::GraphId heightmapId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId outputId = graph.CreateNode(NodeKind::Output);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::Node* heightmap = graph.FindNode(heightmapId);
        const tg::graph::Node* output = graph.FindNode(outputId);
        const tg::graph::Node* path = graph.FindNode(pathId);

        const bool outputConnected =
            heightmap != nullptr && output != nullptr && !heightmap->outputs.empty() &&
            !output->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, output->inputs.front().id);
        Check(outputConnected && IsNeutralPlane(graph.CompileLayersTo(pathId)),
              "Base 未接続の Path は Output 側の地形ではなく変位 0 の平面になる");

        const bool pathConnected =
            heightmap != nullptr && path != nullptr && !heightmap->outputs.empty() &&
            !path->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, path->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(pathId);
        Check(pathConnected && compiled.layers.size() == 1 &&
                  compiled.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  compiled.layers.front().heightSource != tg::compositor::ValueSource::Constant,
              "Base 接続中の Path は自身の入力地形を表示する");
    }

    Section("ノードグラフ — ハイト由来マスクの Base");
    constexpr std::array kHeightMaskKinds = {
        NodeKind::MaskFluvial,
        NodeKind::MaskHeight,
        NodeKind::MaskSlope,
        NodeKind::MaskCurvature,
    };
    for (const NodeKind kind : kHeightMaskKinds) {
        NodeGraph graph;
        const tg::graph::GraphId heightmapId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId outputId = graph.CreateNode(NodeKind::Output);
        const tg::graph::GraphId maskId = graph.CreateNode(kind);
        const tg::graph::Node* heightmap = graph.FindNode(heightmapId);
        const tg::graph::Node* output = graph.FindNode(outputId);
        const tg::graph::Node* mask = graph.FindNode(maskId);

        const bool outputConnected =
            heightmap != nullptr && output != nullptr && !heightmap->outputs.empty() &&
            !output->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, output->inputs.front().id);
        const tg::graph::CompiledGraph disconnected = graph.CompileLayersTo(maskId);
        Check(outputConnected && StartsWithNeutralPlane(disconnected),
              "Base 未接続のハイト由来マスクは変位 0 の平面上で表示する");

        const bool maskConnected =
            heightmap != nullptr && mask != nullptr && !heightmap->outputs.empty() &&
            !mask->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, mask->inputs.front().id);
        const tg::graph::CompiledGraph connected = graph.CompileLayersTo(maskId);
        Check(maskConnected && !connected.layers.empty() &&
                  connected.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  connected.layers.front().heightSource != tg::compositor::ValueSource::Constant,
              "Base 接続中のハイト由来マスクは自身の入力地形上で表示する");
    }
}
