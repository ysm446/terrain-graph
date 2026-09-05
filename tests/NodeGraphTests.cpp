// ノードグラフから評価用レイヤー列へのコンパイルを確かめる。
// GPU 評価の前段だけを対象にし、入力を外したときに古い結果を残さない規則を固定する。

#include "graph/NodeGraph.h"

#include "TestSupport.h"

#include <array>

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
