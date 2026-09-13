#include "graph/NodeGraph.h"
#include "graph/CloudMapGenerator.h"
#include "graph/CloudShapeGenerator.h"

#include "compositor/MaterialStack.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <utility>

namespace tg::graph {
namespace {

// 楕円体の表面積を球面上の面積要素から数値積分する。球では4πr²に一致する。
// 形状はm単位なので、密度の単位に合わせてkmへ変換して積分する。

// --- 定義テーブル ---------------------------------------------------------
// ノードの種類・保存名・表示名・ピン構成。CreateNode() がここからピンを作る。

// **ノードの名前とピンのラベルは英語で書く。**
// ノードグラフを持つツール（Substance / Houdini / Gaea など）はどれも英語表記で、
// 素材やノードの呼び名もその語彙で流通している。説明文だけ日本語にする。
// 合成レイヤーのピン。**Mask 入力は「どこに乗せるか」**を外から与えるもので、
// 繋がっていなければノード側のマスク設定がそのまま効く。
constexpr std::array<PinDefinition, 3> kLayerNodePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
}};

// マスクを取らない加工（ブラー）のピン。
// ぼかしのピン。**Mask はどこをぼかすか**（明るい所ほどぼける。繋がなければ全体）。
constexpr std::array<PinDefinition, 3> kBlurPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
}};

// マスクのソースのピン。**入力を持たない。**
constexpr std::array<PinDefinition, 1> kMaskSourcePins = {{
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 高さから作るマスクのピン。**どこのハイトから作るか**を Base 入力で指す。
// 繋がなければ、そのマスクを使うレイヤーの直下のハイトを使う。
constexpr std::array<PinDefinition, 2> kMaskFromHeightPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 積雪のピン。ハイトの加工に加えて、**積もった量を Mask として出す**。
// 積もった所へ別のマテリアルを乗せられるようにするため。
// Mask 入力（省略可）は**雪を降らせる場所**（標高で雪線を切るなど）。
// 降った後の滑落はマスクの外へも出る（縁から下へ流れ出るのは自然な形）。
constexpr std::array<PinDefinition, 4> kDepositPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 堆積のピン。積雪と同じ形に加えて、**土砂を供給する場所**（Emission、省略可）を受ける。
// 繋がなければ全面へ一様に供給する。注ぎ口を絞ると「そこから流した液体」になる。
constexpr std::array<PinDefinition, 4> kSedimentPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Emission"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 崩落のピン。発生源のマスクを受け、地形に加えて
// **岩屑の厚み**と**岩片ごとの乱数**を出す。
constexpr std::array<PinDefinition, 6> kCrumblingPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Emission"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Unique"},
    {PinKind::Output, ValueType::Points, "Points"},
}};

// 河川のピン。川の出どころを絞る Seed（省略可）を受け、地形に加えて
// **水面の被覆**・**河原**・**水深**を出す。
constexpr std::array<PinDefinition, 6> kRiverPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Seed"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Water"},
    {PinKind::Output, ValueType::Mask, "Bank"},
    {PinKind::Output, ValueType::Mask, "Depth"},
}};

// 水滴侵食のピン。効かせる範囲を絞る Mask（省略可）を受け、地形に加えて
// **流量**（水の通った量）と**堆積量**を出す。Mask は削り / 積みの差分に
// 掛けるだけで、水滴の落とし方は変えない。
constexpr std::array<PinDefinition, 5> kDropletPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Flow"},
    {PinKind::Output, ValueType::Mask, "Deposit"},
}};

// 散布のピン。散布範囲を絞る Mask（省略可）を受け、地形に加えて
// **分布**と**個体ごとの乱数**を出す。崩落と同じ形。
constexpr std::array<PinDefinition, 6> kScatterPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Unique"},
    {PinKind::Output, ValueType::Points, "Points"},
}};

// マスクを 1 枚受けて 1 枚返す加工のピン。
constexpr std::array<PinDefinition, 2> kMaskFilterPins = {{
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// マスク 2 枚を合成するピン。**ここでグラフが合流する。**
constexpr std::array<PinDefinition, 3> kMaskBlendPins = {{
    {PinKind::Input, ValueType::Mask, "Foreground"},
    {PinKind::Input, ValueType::Mask, "Background"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// パスのピン。Base は「どの時点の地形に沿うか」（表示とプレビューに使う。
// パスの座標は 2D なので評価には効かない）。出力はパスそのもの。
constexpr std::array<PinDefinition, 2> kPathPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Output, ValueType::Path, "Path"},
}};

// パスの足跡をマスクにするピン。
constexpr std::array<PinDefinition, 2> kMaskPathPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 1> kOutputNodePins = {{
    {PinKind::Input, ValueType::Material, "Material"},
}};

// ソースノードのピン。**入力を持たない。**
constexpr std::array<PinDefinition, 1> kSourceNodePins = {{
    {PinKind::Output, ValueType::Material, "Result"},
}};

constexpr std::array<PinDefinition, 4> kFlowlinePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Source"},
    {PinKind::Input, ValueType::Mask, "Outflow"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 6> kLakePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Water Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Lake"},
    {PinKind::Output, ValueType::Mask, "Depth"},
    {PinKind::Output, ValueType::Mask, "Water Level"},
}};

constexpr std::array<PinDefinition, 6> kSnowCoverPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Cover"},
    {PinKind::Output, ValueType::Mask, "Depth"},
    {PinKind::Output, ValueType::Mask, "Flows"},
}};

constexpr std::array<PinDefinition, 7> kFluvialErosionPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Input, ValueType::Mask, "Hardness"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Wear"},
    {PinKind::Output, ValueType::Mask, "Deposit"},
    {PinKind::Output, ValueType::Mask, "Age"},
}};

constexpr std::array<PinDefinition, 1> kCloudPins = {{
    {PinKind::Output, ValueType::Volume, "Volume"},
}};
constexpr std::array<PinDefinition, 1> kCloudOutputPins = {{
    {PinKind::Input, ValueType::Volume, "Volume"},
}};

constexpr std::array<PinDefinition, 3> kCloudWeatherPins = {{
    {PinKind::Input, ValueType::Mask, "Coverage"},
    {PinKind::Input, ValueType::Mask, "Type"},
    {PinKind::Output, ValueType::Volume, "Volume"},
}};
constexpr std::array<PinDefinition, 2> kCloudLayerPins = {{
    {PinKind::Input, ValueType::Mask, "Distribution"},
    {PinKind::Output, ValueType::Volume, "Volume"},
}};

constexpr std::array<PinDefinition, 4> kMeanderingRiversPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "River"},
}};

constexpr std::array<PinDefinition, 1> kCloudShapePins = {{{PinKind::Output, ValueType::CloudShape, "Shape"}}};
constexpr std::array<PinDefinition, 2> kCloudMergePins = {{
    {PinKind::Input, ValueType::CloudShape, "Shape 1"},
    {PinKind::Output, ValueType::CloudShape, "Shape"}}};
constexpr std::array<PinDefinition, 2> kCloudNoisePins = {{
    {PinKind::Input, ValueType::CloudShape, "Shape"}, {PinKind::Output, ValueType::Volume, "Volume"}}};

constexpr std::array<PinDefinition, 2> kCloudTransformPins = {{
    {PinKind::Input, ValueType::CloudShape, "Shape"}, {PinKind::Output, ValueType::CloudShape, "Shape"}}};

constexpr std::array<PinDefinition, 2> kCloudAnimationPins = {{{PinKind::Input, ValueType::Volume, "Volume"}, {PinKind::Output, ValueType::Volume, "Volume"}}};

constexpr std::array<PinDefinition, 2> kModelScatterPins = {{
    {PinKind::Input, ValueType::Points, "Points"},
    {PinKind::Output, ValueType::Instances, "Instances"},
}};
constexpr std::array<PinDefinition, 1> kModelOutputPins = {{
    {PinKind::Input, ValueType::Instances, "Instances"},
}};
constexpr std::array<NodeDefinition, 47> kNodeDefinitions = {{
    {NodeKind::Heightmap, "heightmap", "Heightmap", kSourceNodePins},
    {NodeKind::Surface, "surface", "Surface", kLayerNodePins},
    {NodeKind::Shape, "shape", "Shape", kLayerNodePins},
    {NodeKind::Liquid, "liquid", "Liquid", kLayerNodePins},
    {NodeKind::Blur, "heightmapBlur", "Heightmap Blur", kBlurPins},
    {NodeKind::Sediment, "sediment", "Sediment", kSedimentPins},
    {NodeKind::Crumbling, "crumbling", "Crumbling", kCrumblingPins},
    {NodeKind::SnowCover, "snowCover", "Snow Cover", kSnowCoverPins},
    {NodeKind::Snow, "snow", "Snow", kDepositPins},
    {NodeKind::River, "river", "River", kRiverPins},
    {NodeKind::Lake, "lake", "Lake", kLakePins},
    {NodeKind::MeanderingRivers, "meanderingRivers", "Meandering Rivers", kMeanderingRiversPins},
    {NodeKind::Droplet, "droplet", "Droplet Erosion", kDropletPins},
    {NodeKind::FluvialErosion, "fluvialErosion", "Fluvial Erosion", kFluvialErosionPins},
    {NodeKind::FlattenBorders, "flattenBorders", "Flatten Borders", kBlurPins},
    {NodeKind::MultiScaleErosion, "multiScaleErosion", "Multi-Scale Erosion", kBlurPins},
    {NodeKind::Scatter, "scatter", "Scatter", kScatterPins},
    {NodeKind::MaskImage, "maskImage", "Mask Image", kMaskSourcePins},
    {NodeKind::MaskNoise, "maskNoise", "Mask Noise", kMaskSourcePins},
    {NodeKind::MaskFluvial, "maskFluvial", "Mask Fluvial", kMaskFromHeightPins},
    {NodeKind::MaskFlowline, "maskFlowline", "Mask Flowline", kFlowlinePins},
    {NodeKind::MaskHeight, "maskHeight", "Mask Height", kMaskFromHeightPins},
    {NodeKind::MaskSlope, "maskSlope", "Mask Slope", kMaskFromHeightPins},
    {NodeKind::MaskCurvature, "maskCurvature", "Mask Curvature", kMaskFromHeightPins},
    {NodeKind::MaskLevels, "maskLevels", "Mask Levels", kMaskFilterPins},
    {NodeKind::MaskBlur, "maskBlur", "Mask Blur", kMaskFilterPins},
    {NodeKind::MaskBlend, "maskBlend", "Mask Blend", kMaskBlendPins},
    {NodeKind::Path, "path", "Path", kPathPins},
    {NodeKind::MaskPath, "maskPath", "Mask Path", kMaskPathPins},
    {NodeKind::MaskArea, "maskArea", "Mask Area", kMaskPathPins},
    {NodeKind::Cloud, "cloud", "Cloud (Legacy)", kCloudPins},
    {NodeKind::CloudLayer, "cloudLayer", "Cloud Layer (Legacy)", kCloudLayerPins},
    {NodeKind::CloudWeatherLayer, "cloudWeatherLayer", "Cloud Weather Layer", kCloudWeatherPins},
    {NodeKind::CloudMerge, "cloudMerge", "Cloud Merge (Experimental)", kCloudMergePins},
    {NodeKind::CloudAnimation, "cloudAnimation", "Cloud Animation", kCloudAnimationPins},
    {NodeKind::CloudNoise, "cloudNoise", "Cloud Noise (Experimental)", kCloudNoisePins},
    {NodeKind::CloudShapeGenerate, "cloudShapeGenerate", "Cloud Shape Generate (Experimental)", kCloudShapePins},
    {NodeKind::CloudMapGenerate, "cloudMapGenerate", "Cloud Map Generate (Experimental)", kCloudShapePins},
    {NodeKind::CloudTransform, "cloudTransform", "Cloud Transform (Experimental)", kCloudTransformPins},
    {NodeKind::CloudOutput, "cloudOutput", "Cloud Output", kCloudOutputPins},
    {NodeKind::ModelScatter, "modelScatter", "Model Scatter", kModelScatterPins},
    {NodeKind::ModelOutput, "modelOutput", "Model Output", kModelOutputPins},
    {NodeKind::Output, "output", "Output", kOutputNodePins},
    // 追加メニューには出さない。読み込みで定義が見つからなかったノードの受け皿。
    {NodeKind::Missing, "missing", "Missing", {}},
}};

}  // namespace

std::span<const NodeDefinition> NodeDefinitions() {
    return kNodeDefinitions;
}

const NodeDefinition* FindNodeDefinition(NodeKind kind) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.kind == kind) {
            return &definition;
        }
    }
    return nullptr;
}

const NodeDefinition* FindNodeDefinitionByName(std::string_view name) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

bool IsLayerNodeKind(NodeKind kind) {
    return kind == NodeKind::Surface || kind == NodeKind::Shape || kind == NodeKind::Liquid ||
           kind == NodeKind::Heightmap || kind == NodeKind::Blur ||
           kind == NodeKind::Sediment || kind == NodeKind::Crumbling ||
           kind == NodeKind::Snow || kind == NodeKind::SnowCover || kind == NodeKind::Lake || kind == NodeKind::MeanderingRivers || kind == NodeKind::River || kind == NodeKind::Droplet ||
           kind == NodeKind::Scatter || kind == NodeKind::MultiScaleErosion ||
           kind == NodeKind::FluvialErosion || kind == NodeKind::FlattenBorders;
}

bool IsSourceNodeKind(NodeKind kind) {
    return kind == NodeKind::Heightmap;
}

bool IsMaskNodeKind(NodeKind kind) {
    return kind == NodeKind::MaskImage || kind == NodeKind::MaskNoise ||
           kind == NodeKind::MaskFlowline || kind == NodeKind::MaskFluvial || kind == NodeKind::MaskHeight ||
           kind == NodeKind::MaskSlope || kind == NodeKind::MaskCurvature ||
           kind == NodeKind::MaskLevels || kind == NodeKind::MaskBlur ||
           kind == NodeKind::MaskBlend || kind == NodeKind::MaskPath ||
           kind == NodeKind::MaskArea;
}

// 下地の Height を読むマスクか。**チェーンのどこを読むか**を Base 入力で指す。
bool IsHeightMaskNodeKind(NodeKind kind) {
    return kind == NodeKind::MaskFlowline || kind == NodeKind::MaskFluvial || kind == NodeKind::MaskHeight ||
           kind == NodeKind::MaskSlope || kind == NodeKind::MaskCurvature;
}

bool IsLayerMaskSourceKind(NodeKind kind) {
    return kind == NodeKind::Sediment || kind == NodeKind::Crumbling ||
           kind == NodeKind::Snow || kind == NodeKind::SnowCover || kind == NodeKind::Lake || kind == NodeKind::MeanderingRivers || kind == NodeKind::River || kind == NodeKind::FluvialErosion || kind == NodeKind::Droplet ||
           kind == NodeKind::Scatter;
}

bool IsPreviewableNodeKind(NodeKind kind) {
    // マスクは見ながら調整するものなので、どのマスクノードもプレビューできる。
    // パスは Base に繋いだ地形（沿う面）を出す。
    return IsLayerNodeKind(kind) || IsMaskNodeKind(kind) || kind == NodeKind::Path;
}

compositor::LayerKind LayerKindFor(NodeKind kind) {
    switch (kind) {
        case NodeKind::FluvialErosion:
            return compositor::LayerKind::FluvialErosion;
        case NodeKind::FlattenBorders:
            return compositor::LayerKind::FlattenBorders;
        case NodeKind::MultiScaleErosion:
            return compositor::LayerKind::MultiScaleErosion;
        // ハイトマップは合成規則としてはシェイプ（高さへの加算）。
        // 先頭に置く前提なので、加算がそのまま地形になる。
        case NodeKind::Heightmap:
        case NodeKind::Shape:
            return compositor::LayerKind::Shape;
        case NodeKind::Liquid:
            return compositor::LayerKind::Liquid;
        case NodeKind::Blur:
            return compositor::LayerKind::Blur;
        case NodeKind::Sediment:
            return compositor::LayerKind::Sediment;
        case NodeKind::Crumbling:
            return compositor::LayerKind::Crumbling;
        case NodeKind::MeanderingRivers:
            return compositor::LayerKind::MeanderingRivers;
        case NodeKind::Lake:
            return compositor::LayerKind::Lake;
        case NodeKind::SnowCover:
            return compositor::LayerKind::SnowCover;
        case NodeKind::Snow:
            return compositor::LayerKind::Snow;
        case NodeKind::River:
            return compositor::LayerKind::River;
        case NodeKind::Droplet:
            return compositor::LayerKind::Droplet;
        case NodeKind::Scatter:
            return compositor::LayerKind::Scatter;
        default:
            return compositor::LayerKind::Surface;
    }
}

// --- NodeGraph ------------------------------------------------------------

NodeGraph NodeGraph::CreateDefault() {
    NodeGraph graph;
    const GraphId baseId = graph.CreateNode(NodeKind::Surface);
    if (Node* base = graph.FindMutableNode(baseId)) {
        compositor::MaterialLayer layer = compositor::MaterialStack::MakeBaseLayer();
        base->settings = LayerNodeSettings{std::move(layer)};
        base->posX = 60.0f;
        base->posY = 120.0f;
        base->positionValid = true;
    }
    const GraphId outputId = graph.CreateNode(NodeKind::Output);
    if (Node* output = graph.FindMutableNode(outputId)) {
        output->posX = 420.0f;
        output->posY = 120.0f;
        output->positionValid = true;
    }
    const Node* base = graph.FindNode(baseId);
    const Node* output = graph.FindNode(outputId);
    if (base != nullptr && output != nullptr && !base->outputs.empty() &&
        !output->inputs.empty()) {
        graph.CreateLink(base->outputs.front().id, output->inputs.front().id);
    }
    return graph;
}

const Pin* NodeGraph::FindPin(GraphId pinId) const {
    for (const Node& node : m_nodes) {
        for (const Pin& pin : node.inputs) {
            if (pin.id == pinId) {
                return &pin;
            }
        }
        for (const Pin& pin : node.outputs) {
            if (pin.id == pinId) {
                return &pin;
            }
        }
    }
    return nullptr;
}

const Node* NodeGraph::FindNode(GraphId nodeId) const {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [nodeId](const Node& node) { return node.id == nodeId; });
    return it == m_nodes.end() ? nullptr : &*it;
}

Node* NodeGraph::FindMutableNode(GraphId nodeId) {
    const auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                                 [nodeId](const Node& node) { return node.id == nodeId; });
    return it == m_nodes.end() ? nullptr : &*it;
}

const Node* NodeGraph::FindUpstreamNodeForPin(GraphId inputPinId) const {
    for (const Link& link : m_links) {
        if (link.endPin != inputPinId) {
            continue;
        }
        if (const Pin* startPin = FindPin(link.startPin)) {
            return FindNode(startPin->nodeId);
        }
    }
    return nullptr;
}

// producer の出力から下流を辿り、target に届くか。循環チェックに使う。
bool NodeGraph::ReachesDownstream(GraphId fromNodeId, GraphId targetNodeId) const {
    std::unordered_set<GraphId> visited;
    std::vector<GraphId> stack{fromNodeId};
    while (!stack.empty()) {
        const GraphId current = stack.back();
        stack.pop_back();
        if (current == targetNodeId) {
            return true;
        }
        if (!visited.insert(current).second) {
            continue;
        }
        const Node* node = FindNode(current);
        if (node == nullptr) {
            continue;
        }
        for (const Pin& output : node->outputs) {
            for (const Link& link : m_links) {
                if (link.startPin != output.id) {
                    continue;
                }
                if (const Pin* endPin = FindPin(link.endPin)) {
                    stack.push_back(endPin->nodeId);
                }
            }
        }
    }
    return false;
}

bool NodeGraph::CanCreateLink(GraphId startPin, GraphId endPin) const {
    if (startPin == 0 || endPin == 0 || startPin == endPin) {
        return false;
    }
    const Pin* start = FindPin(startPin);
    const Pin* end = FindPin(endPin);
    if (start == nullptr || end == nullptr || start->nodeId == end->nodeId ||
        start->valueType != end->valueType) {
        return false;
    }
    if (start->kind == end->kind) {
        return false;
    }
    // 出力側 → 入力側へ揃えてから循環を見る。
    if (start->kind == PinKind::Input) {
        std::swap(start, end);
    }
    // end（消費側）の下流に start（生産側）がいたら、この接続で輪ができる。
    if (ReachesDownstream(end->nodeId, start->nodeId)) {
        return false;
    }
    return true;
}

bool NodeGraph::CreateLink(GraphId startPin, GraphId endPin) {
    if (!CanCreateLink(startPin, endPin)) {
        return false;
    }
    const Pin* start = FindPin(startPin);
    if (start != nullptr && start->kind == PinKind::Input) {
        std::swap(startPin, endPin);
    }
    // 入力ピンは 1 本だけ。既にある接続は置き換える。
    std::erase_if(m_links, [endPin](const Link& link) { return link.endPin == endPin; });
    m_links.push_back({AllocateGraphId(), startPin, endPin});
    NormalizeCloudMergeInputs();
    MarkDirty();
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId) {
    const size_t oldSize = m_links.size();
    std::erase_if(m_links, [linkId](const Link& link) { return link.id == linkId; });
    if (m_links.size() == oldSize) {
        return false;
    }
    NormalizeCloudMergeInputs();
    MarkDirty();
    return true;
}

CompiledCloud NodeGraph::CompileCloudShapes(GraphId shapeId) const {
    std::unordered_map<GraphId,CompiledCloud> cache;
    std::unordered_set<GraphId> active;
    std::function<CompiledCloud(const Node*,int)> evaluate = [&](const Node* shape,int depth) -> CompiledCloud {
        CompiledCloud result;
        if (!shape) return result;
        if (depth>128 || active.contains(shape->id)) { result.shapeOverflow=true; return result; }
        if (const auto found=cache.find(shape->id);found!=cache.end()) return found->second;
        active.insert(shape->id);
        std::unordered_set<uint64_t> identities;
        const auto append = [&](CloudPrimitive primitive) {
            const uint64_t key=(uint64_t(static_cast<uint32_t>(primitive.originId))<<32)|primitive.originIndex;
            if (!identities.insert(key).second) return;
            if ((result.primitives.size()+1)*sizeof(CloudPrimitive)>CloudShapeMemoryBudget) { result.shapeOverflow=true; return; }
            result.primitives.push_back(primitive);
        };
        if (const auto* map = std::get_if<CloudMapSettings>(&shape->settings)) {
            auto found=m_cloudMapCache.find(shape->id);
            if (found==m_cloudMapCache.end() || !(found->second.settings==*map)) {
                CloudMapCache generated{*map,GenerateCloudMap(*map,shape->id)};
                found=m_cloudMapCache.insert_or_assign(shape->id,std::move(generated)).first;
            }
            result=found->second.result;
        } else if (const auto* generate = std::get_if<CloudShapeGenerateSettings>(&shape->settings)) {
            auto found=m_cloudShapeGenerateCache.find(shape->id);
            if (found==m_cloudShapeGenerateCache.end() || !(found->second.settings==*generate)) {
                CloudShapeGenerateCache generated{*generate,GenerateCloudShape(*generate,shape->id)};
                found=m_cloudShapeGenerateCache.insert_or_assign(shape->id,std::move(generated)).first;
            }
            result=found->second.result;
        } else if (const auto* merge=std::get_if<CloudMergeSettings>(&shape->settings)) {
            result.smoothness=std::clamp(merge->smoothness,0.0f,500.0f);
            for (const auto& pin:shape->inputs) {
                const auto input=evaluate(FindUpstreamNodeForPin(pin.id),depth+1);
                result.shapeOverflow|=input.shapeOverflow;
                result.smoothness=std::max(result.smoothness,input.smoothness);
                for (const auto& primitive:input.primitives) append(primitive);
            }
        } else if (const auto* transform=std::get_if<CloudTransformSettings>(&shape->settings)) {
            result=evaluate(UpstreamOf(*shape,ValueType::CloudShape),depth+1);
            uint32_t index=0;
            for (auto& primitive:result.primitives) {
                primitive.centerX+=transform->translateX;
                primitive.centerY+=transform->translateY;
                primitive.centerZ+=transform->translateZ;
                // 変換した枝は元形状と再マージしても重複除外しない。
                primitive.originId=shape->id;
                primitive.originIndex=index++;
            }
        }
        active.erase(shape->id);
        result.connected=!result.primitives.empty() && !result.shapeOverflow;
        cache.emplace(shape->id,result);
        return result;
    };
    return evaluate(FindNode(shapeId),0);
}

std::vector<CompiledModelScatter> NodeGraph::CompileModelScatters() const {
    std::vector<CompiledModelScatter> result;
    for (const auto& output : m_nodes) {
        if (output.kind != NodeKind::ModelOutput || output.inputs.empty()) continue;
        const auto* scatter = FindUpstreamNodeForPin(output.inputs[0].id);
        if (!scatter || scatter->kind != NodeKind::ModelScatter || scatter->inputs.empty()) continue;
        const auto* source = FindUpstreamNodeForPin(scatter->inputs[0].id);
        const auto* settings = std::get_if<ModelScatterSettings>(&scatter->settings);
        if (!source || (source->kind != NodeKind::Crumbling && source->kind != NodeKind::Scatter) || !settings) continue;
        if (std::none_of(result.begin(), result.end(), [&](const auto& x) { return x.node == scatter->id; }))
            result.push_back({scatter->id, source->id, *settings});
    }
    return result;
}

CompiledCloud NodeGraph::CompileCloud() const {
    CompiledCloud result;
    for (const Node& node : m_nodes) {
        if (node.kind != NodeKind::CloudOutput) continue;
        result.hasOutput = true;
        const Node* source = UpstreamOf(node, ValueType::Volume);
        const Node* animation=nullptr;
        for (size_t depth=0; source && source->kind==NodeKind::CloudAnimation && depth<m_nodes.size(); ++depth) {
            if (!animation) animation=source;
            source=UpstreamOf(*source,ValueType::Volume);
        }
        if (source != nullptr && (source->kind == NodeKind::Cloud || source->kind == NodeKind::CloudLayer)) {
            if (const auto* cloud = std::get_if<CloudNodeSettings>(&source->settings)) {
                result.cloud = *cloud;
                result.connected = true;
                result.sourceId = source->id;
                result.layer = source->kind == NodeKind::CloudLayer;
                if (result.layer && !source->inputs.empty()) {
                    for (const auto& link : m_links) {
                        if (link.endPin != source->inputs.front().id) continue;
                        if (const auto* pin = FindPin(link.startPin)) {
                            result.maskNode = pin->nodeId;
                            result.maskPin = pin->id;
                        }
                    }
                }
            }
        }
        if (source && source->kind == NodeKind::CloudWeatherLayer) {
            const auto& weather = std::get<CloudWeatherSettings>(source->settings);
            result.connected = true;
            result.layer = true;
            result.weather = true;
            result.sourceId = source->id;
            result.cloudType = std::clamp(weather.cloudType, 0.0f, 1.0f);
            result.anvil = std::clamp(weather.anvil, 0.0f, 1.0f);
            result.wisp = std::clamp(weather.wisp, 0.0f, 1.0f);
            result.streets = std::clamp(weather.streets, 0.0f, 1.0f);
            result.variation = std::clamp(weather.variation, 0.0f, 1.0f);
            result.detailScale = std::clamp(weather.detailScale, 20.0f, 5000.0f);
            result.distanceLod = weather.distanceLod;
            result.farDistance = std::clamp(weather.farDistance, 0.0f, 200000.0f);
            auto& cloud = result.cloud;
            cloud.enabled = true;
            cloud.centerX = weather.centerX; cloud.centerZ = weather.centerZ;
            cloud.width = std::clamp(weather.width, 100.0f, 200000.0f);
            cloud.depth = std::clamp(weather.depth, 100.0f, 200000.0f);
            result.weatherThickness = std::clamp(weather.maxThickness, 100.0f, 20000.0f);
            result.weatherBottom = weather.bottomHeight;
            // 球殻の沈み分（範囲の対角の端で d²/(2R)）だけ包囲箱を下へ広げる。
            const float halfDiagonal = std::sqrt(cloud.width * cloud.width + cloud.depth * cloud.depth) * 0.5f;
            const float drop = halfDiagonal * halfDiagonal / (2.0f * 6360000.0f);
            cloud.thickness = result.weatherThickness + drop;
            cloud.centerY = weather.bottomHeight - drop + cloud.thickness * 0.5f;
            cloud.coverage = std::clamp(weather.coverage, 0.0f, 1.0f);
            cloud.noiseScale = std::clamp(weather.noiseScale, 100.0f, 50000.0f);
            cloud.noiseType = std::clamp(weather.noiseType, 0, 1);
            cloud.detailStrength = std::clamp(weather.detailStrength, 0.0f, 1.0f);
            cloud.edgeSoftness = std::clamp(weather.edgeSoftness, 0.01f, 1.0f);
            cloud.extinction = weather.extinction;
            cloud.indirectLight = weather.indirectLight;
            cloud.ambientLight = weather.ambientLight;
            cloud.seed = weather.seed;
            cloud.flatBottom = false;
            cloud.animate = weather.animate;
            cloud.motionMode = 1;
            cloud.windSpeed = weather.windSpeed;
            cloud.windDirection = weather.windDirection;
            cloud.noiseSpeedRatio = weather.evolveNoise ? std::clamp(weather.noiseSpeedRatio, 0.0f, 1.0f) : 1.0f;
            for (size_t input = 0; input < source->inputs.size() && input < 2; ++input) {
                for (const auto& link : m_links) {
                    if (link.endPin != source->inputs[input].id) continue;
                    const auto* pin = FindPin(link.startPin);
                    if (!pin) continue;
                    if (input == 0) { result.maskNode = pin->nodeId; result.maskPin = pin->id; }
                    else { result.typeMaskNode = pin->nodeId; result.typeMaskPin = pin->id; }
                }
            }
        }
        if (source && source->kind == NodeKind::CloudNoise) {
            const auto& noise = std::get<CloudNoiseSettings>(source->settings);
            const auto* shape=UpstreamOf(*source,ValueType::CloudShape);
            result=CompileCloudShapes(shape ? shape->id : 0);
            result.hasOutput=true;
            result.sourceId=source->id;
            auto& cloud=result.cloud;
            cloud.flatBottom=noise.flattenBottom;
            result.bottomHeight=noise.bottomHeight;
            result.bottomFeather=std::clamp(noise.bottomFeather,0.0f,300.0f);
            cloud.animate=false;
            cloud.noiseScale=noise.scale;
            cloud.noiseType=noise.noiseType==0 ? 2 : std::clamp(noise.noiseType-1,0,1);
            cloud.shapeStrength=noise.displacement;
            cloud.detailStrength=noise.detail;
            cloud.edgeSoftness=noise.feather;
            cloud.extinction=noise.extinction;
            cloud.indirectLight=noise.indirectLight;
            cloud.ambientLight=noise.ambientLight;
            cloud.seed=noise.seed;
            if (result.connected) {
                float loX=1e9f,loY=1e9f,loZ=1e9f,hiX=-1e9f,hiY=-1e9f,hiZ=-1e9f;
                // d >= rMin * (length(offset / radii) - 1)。この下界から各軸の範囲を求める。
                // 細部の削りと境界幅は内向きなので、外側の余白には加えない。
                // smooth unionは最も近い2距離の多項式合成なので、膨張は形状数に関係なく最大 k/4。
                const float expansion=result.smoothness*0.25f+noise.displacement;
                for (const auto& primitive : result.primitives) {
                    const float scale=1+expansion/std::min({primitive.radiusX,primitive.radiusY,primitive.radiusZ});
                    loX=std::min(loX,primitive.centerX-primitive.radiusX*scale); hiX=std::max(hiX,primitive.centerX+primitive.radiusX*scale);
                    loY=std::min(loY,primitive.centerY-primitive.radiusY*scale); hiY=std::max(hiY,primitive.centerY+primitive.radiusY*scale);
                    loZ=std::min(loZ,primitive.centerZ-primitive.radiusZ*scale); hiZ=std::max(hiZ,primitive.centerZ+primitive.radiusZ*scale);
                }
                cloud.centerX=(loX+hiX)*0.5f; cloud.centerY=(loY+hiY)*0.5f; cloud.centerZ=(loZ+hiZ)*0.5f;
                cloud.width=hiX-loX; cloud.thickness=hiY-loY; cloud.depth=hiZ-loZ;
            }
        }
        if (animation && result.connected) {
            result.animation=std::get<CloudAnimationSettings>(animation->settings);
            result.animation.width=std::clamp(result.animation.width,100.0f,100000.0f);
            result.animation.depth=std::clamp(result.animation.depth,100.0f,100000.0f);
            result.cloud.animate=result.animation.playing;
            result.cloud.windSpeed=std::clamp(result.animation.speed,0.0f,1000.0f);
            result.cloud.windDirection=result.animation.direction;
            result.cloud.noiseSpeedRatio=result.animation.evolveNoise ? std::clamp(result.animation.noiseSpeedRatio,0.0f,1.0f) : 1.0f;
            result.cloud.motionMode=3;
            result.sourceId=animation->id;
        }
        break; // 雲出力は一つ。壊れたファイルに複数あっても先頭を採用する。
    }
    return result;
}

GraphId NodeGraph::CreateNode(NodeKind kind) {
    if (kind == NodeKind::CloudOutput && CompileCloud().hasOutput) return 0;
    const NodeDefinition* definition = FindNodeDefinition(kind);
    if (definition == nullptr) {
        return 0;
    }
    Node node;
    node.id = AllocateGraphId();
    node.kind = kind;
    if (IsLayerNodeKind(kind)) {
        LayerNodeSettings settings;
        settings.layer.kind = LayerKindFor(kind);
        node.settings = std::move(settings);
    } else if (IsMaskNodeKind(kind)) {
        node.settings = MaskNodeSettings{};
    } else if (kind == NodeKind::Cloud || kind == NodeKind::CloudLayer) {
        CloudNodeSettings cloud;
        if (kind == NodeKind::CloudLayer) {
            cloud.width = cloud.depth = 12000.0f;
            cloud.thickness = 600.0f;
            cloud.centerY = 500.0f;
            cloud.noiseScale = 1500.0f;
            cloud.motionMode = 1;
        }
        node.settings = cloud;
    } else if (kind == NodeKind::CloudMerge) { node.settings = CloudMergeSettings{};
    } else if (kind == NodeKind::CloudAnimation) { node.settings = CloudAnimationSettings{};
    } else if (kind == NodeKind::CloudNoise) { node.settings = CloudNoiseSettings{};
    } else if (kind == NodeKind::CloudMapGenerate) { node.settings = CloudMapSettings{};
    } else if (kind == NodeKind::CloudShapeGenerate) { node.settings = CloudShapeGenerateSettings{};
    } else if (kind == NodeKind::CloudWeatherLayer) { node.settings = CloudWeatherSettings{};
    } else if (kind == NodeKind::CloudTransform) { node.settings = CloudTransformSettings{};
    } else if (kind == NodeKind::ModelScatter) { node.settings = ModelScatterSettings{};
    } else if (kind == NodeKind::Missing) { node.settings = MissingNodeSettings{};
    } else if (kind == NodeKind::Path) {
        node.settings = PathNodeSettings{};
    } else {
        node.settings = OutputNodeSettings{};
    }
    for (const PinDefinition& pin : definition->pins) {
        Pin created;
        created.id = AllocateGraphId();
        created.nodeId = node.id;
        created.kind = pin.kind;
        created.valueType = pin.valueType;
        created.label = pin.label;
        if (pin.kind == PinKind::Input) {
            node.inputs.push_back(std::move(created));
        } else {
            node.outputs.push_back(std::move(created));
        }
    }
    const GraphId nodeId = node.id;
    m_nodes.push_back(std::move(node));
    MarkDirty();
    return nodeId;
}

bool NodeGraph::DeleteNode(GraphId nodeId) {
    const Node* node = FindNode(nodeId);
    if (node == nullptr) {
        return false;
    }
    std::vector<GraphId> pinIds;
    pinIds.reserve(node->inputs.size() + node->outputs.size());
    for (const Pin& pin : node->inputs) {
        pinIds.push_back(pin.id);
    }
    for (const Pin& pin : node->outputs) {
        pinIds.push_back(pin.id);
    }
    std::erase_if(m_links, [&pinIds](const Link& link) {
        return std::find(pinIds.begin(), pinIds.end(), link.startPin) != pinIds.end() ||
               std::find(pinIds.begin(), pinIds.end(), link.endPin) != pinIds.end();
    });
    m_cloudMapCache.erase(nodeId);
    m_cloudShapeGenerateCache.erase(nodeId);
    std::erase_if(m_nodes, [nodeId](const Node& candidate) { return candidate.id == nodeId; });
    NormalizeCloudMergeInputs();
    MarkDirty();
    return true;
}

void NodeGraph::Replace(std::vector<Node> nodes, std::vector<Link> links) {
    m_cloudMapCache.clear();
    m_cloudShapeGenerateCache.clear();
    m_nodes = std::move(nodes);
    m_links = std::move(links);
    // 壊れたリンク（ピンが無い・型が合わない）は捨てる。読み込みの安全網。
    std::erase_if(m_links, [this](const Link& link) {
        const Pin* start = FindPin(link.startPin);
        const Pin* end = FindPin(link.endPin);
        return start == nullptr || end == nullptr || start->kind != PinKind::Output ||
               end->kind != PinKind::Input || start->valueType != end->valueType;
    });
    RebuildNextGraphId();
    NormalizeCloudMergeInputs();
    MarkDirty();
}

void NodeGraph::NormalizeCloudMergeInputs() {
    std::unordered_set<GraphId> connected;
    for (const auto& link:m_links) connected.insert(link.endPin);
    for (auto& node:m_nodes) {
        if (node.kind!=NodeKind::CloudMerge) continue;
        // 接続済みピンのIDと順序を保ち、空きは末尾の1個にまとめる。
        std::vector<Pin> inputs;
        GraphId spare=0;
        for (const auto& pin:node.inputs) {
            if (connected.contains(pin.id)) inputs.push_back(pin);
            else if (!spare) spare=pin.id;
        }
        if (!spare) spare=AllocateGraphId();
        inputs.push_back({spare,node.id,PinKind::Input,ValueType::CloudShape,{}});
        for (size_t i=0;i<inputs.size();++i) inputs[i].label="Shape "+std::to_string(i+1);
        node.inputs=std::move(inputs);
    }
}

void NodeGraph::RebuildNextGraphId() {
    GraphId maxId = 0;
    for (const Node& node : m_nodes) {
        maxId = std::max(maxId, node.id);
        for (const Pin& pin : node.inputs) {
            maxId = std::max(maxId, pin.id);
        }
        for (const Pin& pin : node.outputs) {
            maxId = std::max(maxId, pin.id);
        }
    }
    for (const Link& link : m_links) {
        maxId = std::max(maxId, link.id);
    }
    m_nextGraphId = maxId + 1;
}

// 「下地」チェーンを上から下へ辿る。輪は visited で止める。
std::vector<const Node*> NodeGraph::ChainFrom(const Node* top) const {
    std::vector<const Node*> chain;
    std::unordered_set<GraphId> visited;
    const Node* current = top;
    while (current != nullptr && IsLayerNodeKind(current->kind) &&
           visited.insert(current->id).second) {
        chain.push_back(current);
        current = current->inputs.empty() ? nullptr
                                          : FindUpstreamNodeForPin(current->inputs.front().id);
    }
    return chain;
}

const Node* NodeGraph::ChainTop() const {
    // 出力ノード（最初の 1 つ）に繋がっているノード。
    for (const Node& node : m_nodes) {
        if (node.kind != NodeKind::Output) {
            continue;
        }
        return node.inputs.empty() ? nullptr : FindUpstreamNodeForPin(node.inputs.front().id);
    }
    return nullptr;
}

const Node* NodeGraph::PreviewTop(GraphId nodeId) const {
    const Node* node = FindNode(nodeId);
    if (node != nullptr && IsLayerNodeKind(node->kind)) {
        return node;
    }
    // Path とハイト由来のマスクは、自分ではハイトを作らない。入力に繋いだ
    // チェーンだけを見る。未接続のときに Output 側の別チェーンへ落とすと
    // 無関係な地形が見えるので、nullptr を返して中立平面を作らせる。
    if (node != nullptr &&
        (node->kind == NodeKind::Path || IsHeightMaskNodeKind(node->kind))) {
        for (const Pin& pin : node->inputs) {
            if (pin.valueType != ValueType::Material) {
                continue;
            }
            if (const Node* source = FindUpstreamNodeForPin(pin.id); source != nullptr) {
                return source;
            }
        }
        return nullptr;
    }
    // **出どころ（堆積 / 崩落 / 積雪）が本流に居ないことがある。**
    // その Mask は「そのレイヤーを合成した時点」の作業用テクスチャから焼くので、
    // 本流だけを合成したチェーンでは結果が残らず、繋いでいないのと同じ扱いになる
    // （Mask Blend だと片側だけが素通りして、合成した絵に見えない）。
    // 出どころを含むチェーンがあれば、そちらを起点にする。
    if (node != nullptr && IsMaskNodeKind(node->kind)) {
        std::vector<const Node*> sources;
        CollectLayerMaskSources(*node, sources, 0);
        if (!sources.empty()) {
            const auto covers = [&](const Node* top) {
                if (top == nullptr) {
                    return false;
                }
                const std::vector<const Node*> chain = ChainFrom(top);
                for (const Node* source : sources) {
                    if (std::find(chain.begin(), chain.end(), source) == chain.end()) {
                        return false;
                    }
                }
                return true;
            };
            // **本流で足りるならそのまま。** 地形を最後まで合成した上に貼れる。
            if (const Node* top = ChainTop(); covers(top)) {
                return top;
            }
            // 足りなければ、出どころのうち**他を全部含むもの**を起点にする。
            for (const Node* source : sources) {
                if (covers(source)) {
                    return source;
                }
            }
        }
    }
    return ChainTop();
}

bool NodeGraph::MaskDependsOnHeight(const Node& maskNode, int depth) const {
    constexpr int kMaxDepth = 32;
    if (depth > kMaxDepth) {
        return false;
    }
    switch (maskNode.kind) {
        case NodeKind::MaskFlowline:
        case NodeKind::MaskFluvial:
        case NodeKind::MaskHeight:
        case NodeKind::MaskSlope:
        case NodeKind::MaskCurvature:
        case NodeKind::Sediment:
        case NodeKind::Crumbling:
        case NodeKind::Lake:
        case NodeKind::MeanderingRivers:
        case NodeKind::SnowCover:
        case NodeKind::Snow:
        case NodeKind::River:
        case NodeKind::Droplet:
        case NodeKind::FluvialErosion:
        case NodeKind::Scatter:
            return true;
        // パスは 2D で高さを読まないが、**地形の上に引いたもの**なので、
        // 平らな板ではなく地形の上に貼って見せる（線と地形の対応こそが見たいもの）。
        case NodeKind::MaskPath:
        case NodeKind::MaskArea:
            return true;
        case NodeKind::MaskLevels:
        case NodeKind::MaskBlur:
        case NodeKind::MaskBlend:
            for (size_t which = 0; which < 2; ++which) {
                const MaskSourceRef input = UpstreamMaskOf(maskNode, which);
                if (input.node != nullptr && MaskDependsOnHeight(*input.node, depth + 1)) {
                    return true;
                }
            }
            return false;
        default:
            return false;
    }
}

int NodeGraph::FindMaskSpliceIndex(const Node& source,
                                   const std::vector<const Node*>& layerNodes) const {
    // 出どころの下地チェーンを下へ辿り、**最初に本流と一致した所**を返す。
    // 自分自身は（本流に居ないから差し込むので）飛ばす。
    for (const Node* node : ChainFrom(&source)) {
        if (node == &source) {
            continue;
        }
        for (size_t i = 0; i < layerNodes.size(); ++i) {
            if (layerNodes[i] == node) {
                return static_cast<int>(i);
            }
        }
    }
    return -1;
}

bool NodeGraph::MaskSourceResolves(const Node& consumer) const {
    const MaskSourceRef source = UpstreamMaskOf(consumer);
    if (source.node == nullptr) {
        return true;  // 繋いでいない。マスクはノード側の設定で決まる。
    }
    // レイヤーでもあるマスクの出どころ（堆積 / 崩落 / 積雪）だけが、居場所に依存する。
    // **Mask Levels / Mask Blend の先にいるものも数える。**
    std::vector<const Node*> sources;
    CollectLayerMaskSources(*source.node, sources, 0);
    if (sources.empty()) {
        return true;
    }
    const std::vector<const Node*> chain = ChainFrom(&consumer);
    for (const Node* node : sources) {
        if (std::find(chain.begin(), chain.end(), node) != chain.end()) {
            continue;  // チェーンの中にいる。そのまま走る。
        }
        // チェーンの外でも、下地が本流と合流していれば差し込める
        // （Result は繋がなくてよい）。
        if (FindMaskSpliceIndex(*node, chain) < 0) {
            return false;
        }
    }
    return true;
}

const TerrainScale* NodeGraph::FindChainScale(GraphId nodeId) const {
    // チェーンの根（一番下）にあるソースを探す。地形の実寸はそこが持つ。
    const std::vector<const Node*> chain = ChainFrom(PreviewTop(nodeId));
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!IsSourceNodeKind((*it)->kind)) {
            continue;
        }
        if (const auto* settings = std::get_if<LayerNodeSettings>(&(*it)->settings)) {
            return &settings->scale;
        }
    }
    return nullptr;
}

// Mask 入力の出どころを、上流の**出力ピンの位置**まで含めて返す。
NodeGraph::MaskSourceRef NodeGraph::UpstreamMaskOf(const Node& node, size_t which) const {
    MaskSourceRef result;
    size_t seen = 0;
    for (const Pin& pin : node.inputs) {
        if (pin.valueType != ValueType::Mask) {
            continue;
        }
        if (seen++ != which) {
            continue;
        }
        for (const Link& link : m_links) {
            if (link.endPin != pin.id) {
                continue;
            }
            const Pin* source = FindPin(link.startPin);
            const Node* sourceNode = (source != nullptr) ? FindNode(source->nodeId) : nullptr;
            if (sourceNode == nullptr) {
                return result;
            }
            // 何番目の Mask 出力か。崩落は 2 本持つので、ここで見分ける。
            size_t index = 0;
            for (const Pin& out : sourceNode->outputs) {
                if (out.valueType != ValueType::Mask) {
                    continue;
                }
                if (out.id == source->id) {
                    result.node = sourceNode;
                    result.outputIndex = index;
                    return result;
                }
                ++index;
            }
            return result;
        }
        return result;
    }
    return result;
}

const Node* NodeGraph::UpstreamOf(const Node& node, ValueType type, size_t which) const {
    size_t seen = 0;
    for (const Pin& pin : node.inputs) {
        if (pin.valueType != type) {
            continue;
        }
        if (seen++ != which) {
            continue;
        }
        return FindUpstreamNodeForPin(pin.id);
    }
    return nullptr;
}

// マスクの木を辿って、レイヤーでもある出どころ（堆積 / 崩落 / 積雪）を集める。
// **出どころ自身はレイヤーなので、そこから先はマスクを遡らない。**
void NodeGraph::CollectLayerMaskSources(const Node& maskNode, std::vector<const Node*>& out,
                                        int depth) const {
    constexpr int kMaxDepth = 32;
    if (depth > kMaxDepth) {
        return;
    }
    if (IsLayerMaskSourceKind(maskNode.kind)) {
        if (std::find(out.begin(), out.end(), &maskNode) == out.end()) {
            out.push_back(&maskNode);
        }
        return;
    }
    for (size_t which = 0; which < 2; ++which) {
        const MaskSourceRef input = UpstreamMaskOf(maskNode, which);
        if (input.node != nullptr) {
            CollectLayerMaskSources(*input.node, out, depth + 1);
        }
    }
}

// マスクのノードを op の列へ落とす（入力が先、出力が後）。
// **同じノードは 1 つの op を共有する。** Mask Blend で合流したり、
// 同じマスクを 2 つのレイヤーで使ったりしても、評価は 1 回で済む。
int NodeGraph::EmitMaskOps(const MaskSourceRef& source, int defaultHeightLayer,
                           const std::vector<const Node*>& layerNodes,
                           compositor::MaskProgram& ops,
                           std::vector<EmittedMaskOp>& emitted, int depth) const {
    if (source.node == nullptr) {
        return -1;
    }
    const Node& maskNode = *source.node;
    // 循環は CanCreateLink が弾いているが、読み込んだファイルが壊れている
    // 可能性もあるので深さでも止める。
    constexpr int kMaxDepth = 32;
    // 堆積・崩落・積雪は**レイヤーでもありマスクの出どころでもある**
    // （積もった厚み / 積んだ岩屑 / 雪の被覆を出す）。
    const bool isLayerMaskSource = IsLayerMaskSourceKind(maskNode.kind);
    if (depth > kMaxDepth || (!IsMaskNodeKind(maskNode.kind) && !isLayerMaskSource)) {
        return -1;
    }
    for (size_t i = 0; i < emitted.size(); ++i) {
        // 既に焼いてあれば同じ op を共有する。**出力ピンまで一致していること。**
        if (emitted[i].node == &maskNode && emitted[i].heightLayer == defaultHeightLayer &&
            emitted[i].outputIndex == source.outputIndex) {
            return static_cast<int>(i);
        }
    }

    // 堆積 / 崩落 / 積雪はレイヤーの設定を持つ。作業用テクスチャから焼くので、
    // **そのレイヤーがこのチェーンの中にいるときだけ**成立する。
    if (isLayerMaskSource) {
        const auto* layerSettings = std::get_if<LayerNodeSettings>(&maskNode.settings);
        if (layerSettings == nullptr) {
            return -1;
        }
        compositor::MaskOp layerOp;
        if (maskNode.kind == NodeKind::Sediment) {
            layerOp.kind = compositor::MaskOpKind::Sediment;
            layerOp.sedimentMask.contrast = layerSettings->layer.sediment.maskContrast;
            layerOp.sedimentMask.thicknessMeters =
                layerSettings->layer.sediment.maskThicknessMeters;
        } else if (maskNode.kind == NodeKind::MeanderingRivers) {
            layerOp.kind = compositor::MaskOpKind::MeanderingRivers;
        } else if (maskNode.kind == NodeKind::Lake) {
            layerOp.kind = compositor::MaskOpKind::Lake;
            layerOp.dropletMask.channel = static_cast<uint32_t>(std::min<size_t>(source.outputIndex, 2));
        } else if (maskNode.kind == NodeKind::SnowCover) {
            layerOp.kind = compositor::MaskOpKind::SnowCover;
            layerOp.dropletMask.channel = static_cast<uint32_t>(std::min<size_t>(source.outputIndex, 2));
        } else if (maskNode.kind == NodeKind::Snow) {
            layerOp.kind = compositor::MaskOpKind::Snow;
            layerOp.snowMask.thresholdMeters = layerSettings->layer.snow.maskThresholdMeters;
            layerOp.snowMask.featherMeters = layerSettings->layer.snow.maskFeatherMeters;
        } else if (maskNode.kind == NodeKind::River) {
            layerOp.kind = compositor::MaskOpKind::River;
            // 0 番目の Mask 出力が水面、1 番目が河原、2 番目が水深。
            layerOp.riverMask.channel =
                static_cast<uint32_t>(std::min<size_t>(source.outputIndex, 2));
            const auto& river = layerSettings->layer.river;
            layerOp.riverMask.shoreWidthMeters = river.shoreWidthMeters;
            layerOp.riverMask.shoreHeightMeters = river.shoreHeightMeters;
            layerOp.riverMask.shoreFeather = river.shoreFeather;
            layerOp.riverMask.mainWidthMeters = river.mainWidthMeters;
            layerOp.riverMask.minWidthMeters = river.minWidthMeters;
        } else if (maskNode.kind == NodeKind::FluvialErosion) {
            layerOp.kind = compositor::MaskOpKind::FluvialErosion;
            layerOp.dropletMask.channel = static_cast<uint32_t>(std::min<size_t>(source.outputIndex, 2));
        } else if (maskNode.kind == NodeKind::Droplet) {
            layerOp.kind = compositor::MaskOpKind::Droplet;
            // 0 番目の Mask 出力が流量、1 番目が堆積量。
            layerOp.dropletMask.channel = (source.outputIndex == 0) ? 0u : 1u;
        } else if (maskNode.kind == NodeKind::Scatter) {
            layerOp.kind = compositor::MaskOpKind::Scatter;
            // 0 番目の Mask 出力が分布、1 番目が個体ごとの乱数。
            layerOp.scatterMask.channel = (source.outputIndex == 0) ? 0u : 1u;
        } else {
            layerOp.kind = compositor::MaskOpKind::Crumbling;
            // 0 番目の Mask 出力が厚み、1 番目が岩片ごとの乱数。
            layerOp.crumblingMask.channel = (source.outputIndex == 0) ? 0u : 1u;
        }
        layerOp.heightSourceLayer = -1;
        for (size_t i = 0; i < layerNodes.size(); ++i) {
            if (layerNodes[i] == &maskNode) {
                layerOp.heightSourceLayer = static_cast<int>(i);
                break;
            }
        }
        if (layerOp.heightSourceLayer < 0) {
            // このチェーンに居ないノード。結果が残っていないので繋がない。
            return -1;
        }
        ops.push_back(layerOp);
        emitted.push_back({&maskNode, defaultHeightLayer, source.outputIndex});
        return static_cast<int>(ops.size() - 1);
    }

    const auto* settings = std::get_if<MaskNodeSettings>(&maskNode.settings);
    if (settings == nullptr) {
        return -1;
    }

    compositor::MaskOp op;
    switch (maskNode.kind) {
        case NodeKind::MaskImage:
            // 画像が入っていないマスクは「繋がっていない」のと同じ扱い。
            // 白 1 枚として全面に効かせると、繋いだ瞬間に絵が変わって驚く。
            if (settings->map.texture == compositor::kNoTexture) {
                return -1;
            }
            op.kind = compositor::MaskOpKind::Image;
            op.map = settings->map;
            break;
        case NodeKind::MaskNoise:
            op.kind = compositor::MaskOpKind::Noise;
            op.noise = settings->noise;
            break;
        case NodeKind::MaskFlowline:
            op.kind = compositor::MaskOpKind::Flowline;
            op.flowline = settings->flowline;
            break;
        case NodeKind::MaskFluvial:
            op.kind = compositor::MaskOpKind::Fluvial;
            op.fluvial = settings->fluvial;
            break;
        case NodeKind::MaskHeight:
            op.kind = compositor::MaskOpKind::Height;
            op.height = settings->height;
            break;
        case NodeKind::MaskSlope:
            op.kind = compositor::MaskOpKind::Slope;
            op.slope = settings->slope;
            break;
        case NodeKind::MaskCurvature:
            op.kind = compositor::MaskOpKind::Curvature;
            op.curvature = settings->curvature;
            break;
        case NodeKind::MaskLevels:
            op.kind = compositor::MaskOpKind::Levels;
            op.levels = settings->levels;
            break;
        case NodeKind::MaskBlur:
            op.kind = compositor::MaskOpKind::Blur;
            op.blur = settings->blur;
            break;
        case NodeKind::MaskBlend:
            op.kind = compositor::MaskOpKind::Blend;
            op.blend = settings->blend;
            break;
        case NodeKind::MaskPath: {
            // パスが繋がっていなければ「繋がっていない」のと同じ扱い。
            const Node* pathNode = UpstreamOf(maskNode, ValueType::Path);
            const auto* pathSettings = (pathNode != nullptr)
                                           ? std::get_if<PathNodeSettings>(&pathNode->settings)
                                           : nullptr;
            if (pathSettings == nullptr) {
                return -1;
            }
            op.kind = compositor::MaskOpKind::Path;
            op.pathMask = settings->pathMask;
            op.pathSegments = BuildPathSegments(pathSettings->path);
            if (op.pathSegments.empty()) {
                return -1;
            }
            break;
        }
        case NodeKind::MaskArea: {
            // 閉じた鎖が無ければ「繋がっていない」のと同じ扱い（プロパティが注意書きを出す）。
            const Node* pathNode = UpstreamOf(maskNode, ValueType::Path);
            const auto* pathSettings = (pathNode != nullptr)
                                           ? std::get_if<PathNodeSettings>(&pathNode->settings)
                                           : nullptr;
            if (pathSettings == nullptr) {
                return -1;
            }
            op.kind = compositor::MaskOpKind::Area;
            op.areaMask = settings->areaMask;
            op.pathSegments = BuildPathAreaSegments(pathSettings->path);
            if (op.pathSegments.empty()) {
                return -1;
            }
            break;
        }
        default:
            return -1;
    }

    // 高さ由来は「チェーンのどこまで合成した Height を読むか」を Base 入力で指す。
    // 繋いでいない / このチェーンに居ないノードなら、呼び出し側の既定へ落とす。
    if (IsHeightMaskNodeKind(maskNode.kind)) {
        op.heightSourceLayer = defaultHeightLayer;
        if (const Node* heightSource = UpstreamOf(maskNode, ValueType::Material)) {
            for (size_t i = 0; i < layerNodes.size(); ++i) {
                if (layerNodes[i] == heightSource) {
                    op.heightSourceLayer = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    if (maskNode.kind == NodeKind::MaskFlowline) {
        op.inputA = EmitMaskOps(UpstreamMaskOf(maskNode, 0), defaultHeightLayer, layerNodes, ops, emitted, depth + 1);
        op.inputB = EmitMaskOps(UpstreamMaskOf(maskNode, 1), defaultHeightLayer, layerNodes, ops, emitted, depth + 1);
    }
    // 入力のマスクを先に焼く。**op は自分より前だけを指す。**
    if (maskNode.kind == NodeKind::MaskLevels || maskNode.kind == NodeKind::MaskBlur ||
        maskNode.kind == NodeKind::MaskBlend) {
        const MaskSourceRef a = UpstreamMaskOf(maskNode, 0);
        op.inputA = EmitMaskOps(a, defaultHeightLayer, layerNodes, ops, emitted, depth + 1);
    }
    if (maskNode.kind == NodeKind::MaskBlend) {
        const MaskSourceRef b = UpstreamMaskOf(maskNode, 1);
        op.inputB = EmitMaskOps(b, defaultHeightLayer, layerNodes, ops, emitted, depth + 1);
        // 片方だけ繋いでいるときは繋いだほうを通す（未接続は 0 ではなく「中立」）。
        if (op.inputA < 0 && op.inputB < 0) {
            return -1;
        }
    }
    if ((maskNode.kind == NodeKind::MaskLevels || maskNode.kind == NodeKind::MaskBlur) &&
        op.inputA < 0) {
        // 入力の無いレベル調整 / ぼかしは意味を持たない。
        return -1;
    }

    ops.push_back(op);
    emitted.push_back({&maskNode, defaultHeightLayer, source.outputIndex});
    return static_cast<int>(ops.size() - 1);
}

namespace {

// マスクを見せるための塗りレイヤー。
//
// **水面（Liquid）の規則を使う。** 水位を地形より高く取ると
// `weight = mask * 1` になり、重みがマスクの値そのものになる
// （サーフェスのハイトブレンドだと、高さで勝った時点で重みが 1 に張り付く）。
// 色とサーフェスだけを書き、形（Height / Normal）は下地のまま見せる。
compositor::MaterialLayer MakeMaskPreviewLayer(const char* name,
                                               const DirectX::XMFLOAT3& color) {
    compositor::MaterialLayer layer;
    layer.name = name;
    layer.kind = compositor::LayerKind::Liquid;
    layer.channelMask = compositor::ChannelBit(compositor::Channel::BaseColor) |
                        compositor::ChannelBit(compositor::Channel::Surface);
    layer.baseColor = color;
    layer.roughness = 1.0f;
    layer.metallic = 0.0f;
    layer.heightSource = compositor::ValueSource::Constant;
    // 「水位」。地形より高く取ると全面が水中扱いになり、重みはマスクだけで決まる。
    layer.heightBase = 2.0f;
    layer.blendRange = 0.001f;
    return layer;
}

}  // namespace

// 焼いた op の記録を、コンパイル結果の「出どころ」へ写す（ノードのポインタは外へ出さない）。
void NodeGraph::RecordMaskOpSources(const std::vector<EmittedMaskOp>& emitted,
                                    CompiledGraph& compiled) {
    compiled.maskOpSources.clear();
    compiled.maskOpSources.reserve(emitted.size());
    for (const EmittedMaskOp& op : emitted) {
        CompiledGraph::MaskOpSource source;
        source.nodeId = (op.node != nullptr) ? op.node->id : 0;
        source.outputIndex = op.outputIndex;
        compiled.maskOpSources.push_back(source);
    }
}

// レイヤー列の元ノードを、コンパイル結果へ写す。列のほうが長ければ（プレビュー用の
// 塗りレイヤー）残りは 0。
void NodeGraph::RecordLayerSources(const std::vector<const Node*>& layerNodes,
                                   CompiledGraph& compiled) {
    compiled.layerSources.assign(compiled.layers.size(), 0);
    for (size_t i = 0; i < layerNodes.size() && i < compiled.layers.size(); ++i) {
        compiled.layerSources[i] = (layerNodes[i] != nullptr) ? layerNodes[i]->id : 0;
    }
}

CompiledGraph NodeGraph::CompileChainFrom(const Node* top, ChainTrace* trace) const {
    const std::vector<const Node*> chain = ChainFrom(top);

    // 途中経過は呼び出し側が要求したときだけ外へ残す。
    ChainTrace localTrace;
    ChainTrace& state = (trace != nullptr) ? *trace : localTrace;
    state.layerNodes.clear();
    state.emitted.clear();

    // 遡った順（上→下）を、レイヤー列の順（下→上）へ反転する。
    CompiledGraph compiled;
    std::vector<const Node*>& layerNodes = state.layerNodes;
    compiled.layers.reserve(chain.size());
    layerNodes.reserve(chain.size());
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const auto* settings = std::get_if<LayerNodeSettings>(&(*it)->settings)) {
            compositor::MaterialLayer layer = settings->layer;
            // ソース（Heightmap）は画像の 0〜1 がそのままハイトの全幅。
            // 振れ幅は標高差（m）が決めるので、起伏の強さは持たない。
            // 古いファイルが別の値を持っていても、ここで 1.0 に正す。
            if (IsSourceNodeKind((*it)->kind)) {
                layer.heightGain = 1.0f;
            }
            compiled.layers.push_back(layer);
            layerNodes.push_back(*it);
        }
    }

    // Blur / Sediment / Snow などの加工は、入力する下地があって初めて結果を持つ。
    // Base を外して加工だけが残った場合、評価器はどの出力テクスチャにも書かないため、
    // 直前の評価結果がそのまま見えてしまう。加工を走らせず、変位 0 の中立平面へ戻す。
    const bool hasEnabledBase =
        std::any_of(compiled.layers.begin(), compiled.layers.end(), [](const auto& layer) {
            return layer.enabled && !compositor::IsHeightOperationKind(layer.kind);
        });
    if (!hasEnabledBase) {
        compiled.layers.clear();
        compiled.layers.push_back(compositor::MaterialStack::MakeBaseLayer());
        layerNodes.clear();
        return compiled;
    }

    // **Mask だけを繋いだ堆積 / 崩落 / 積雪を、チェーンへ差し込む。**
    //
    // この 3 つの Mask は「そのレイヤーを合成した時点」の作業用テクスチャから焼くので、
    // チェーンの中で走っていないと結果が残らない。Result を繋がずに Mask だけ使いたい
    // ことがあるので、**下地が本流と合流する所の直後**へ差し込み、
    // 代わりに Height へは書き戻さない印（maskOnly）を付ける。
    // 繋いでいない出力は結果に効かせない、という素直な形になる。
    for (bool inserted = true; inserted;) {
        inserted = false;
        for (size_t i = 0; i < layerNodes.size() && !inserted; ++i) {
            std::vector<const Node*> sources;
            const size_t maskCount = layerNodes[i]->kind == NodeKind::FluvialErosion ? 2 : 1;
            for (size_t which = 0; which < maskCount; ++which) {
                const MaskSourceRef mask = UpstreamMaskOf(*layerNodes[i], which);
                if (mask.node != nullptr) CollectLayerMaskSources(*mask.node, sources, 0);
            }
            for (const Node* sourceNode : sources) {
                if (std::find(layerNodes.begin(), layerNodes.end(), sourceNode) !=
                    layerNodes.end()) {
                    continue;  // 既にチェーンの中にいる（Result を繋いでいる）。
                }
                const auto* settings = std::get_if<LayerNodeSettings>(&sourceNode->settings);
                const int join = FindMaskSpliceIndex(*sourceNode, layerNodes);
                if (settings == nullptr || join < 0) {
                    continue;  // 本流と合流しない。差し込めないので、そのまま定数へ落ちる。
                }
                const size_t at = static_cast<size_t>(join) + 1;
                if (at > i) {
                    continue;  // 使う側より上には差し込めない（循環は接続時に弾いている）。
                }
                compositor::MaterialLayer layer = settings->layer;
                layer.maskOnly = true;
                compiled.layers.insert(compiled.layers.begin() + static_cast<ptrdiff_t>(at),
                                       layer);
                layerNodes.insert(layerNodes.begin() + static_cast<ptrdiff_t>(at), sourceNode);
                inserted = true;
                break;
            }
        }
    }

    // Result 経由とマスクだけの分岐の両方で、接続中の Path から標本を作る。
    for (size_t i = 0; i < layerNodes.size(); ++i) {
        if (layerNodes[i]->kind != NodeKind::MeanderingRivers) continue;
        auto& layer = compiled.layers[i];
        layer.meanderPoints.clear();
        const Node* pathNode = UpstreamOf(*layerNodes[i], ValueType::Path);
        const auto* path = pathNode ? std::get_if<PathNodeSettings>(&pathNode->settings) : nullptr;
        const TerrainScale* scale = FindChainScale(layerNodes[i]->id);
        if (path) layer.meanderPoints = BuildMeanderPoints(path->path,
            scale ? scale->sizeMeters : TerrainScale{}.sizeMeters, layer.meanderingRivers.riverWidth *
            layer.meanderingRivers.meanderScale * 2.0f);
    }

    // Mask 入力に繋がっているマスクのノードを op の列へ落とし、
    // レイヤーからは添字で参照する（同じ意味の値を 2 か所から編集させない）。
    // 効き方（係数 / カーブ / レベル / 反転）はレイヤー側の設定のまま。
    std::vector<EmittedMaskOp>& emitted = state.emitted;
    for (size_t i = 0; i < compiled.layers.size(); ++i) {
        if (layerNodes[i]->kind == NodeKind::FluvialErosion) {
            const MaskSourceRef hardnessSource = UpstreamMaskOf(*layerNodes[i], 1);
            if (hardnessSource.node != nullptr)
                compiled.layers[i].hardnessMaskOp = EmitMaskOps(hardnessSource,
                    i > 0 ? static_cast<int>(i) - 1 : 0, layerNodes, compiled.maskOps, emitted, 0);
        }
        const MaskSourceRef maskSource = UpstreamMaskOf(*layerNodes[i]);
        if (maskSource.node == nullptr) {
            continue;
        }
        // 高さ由来のマスクで Base を繋いでいないときは「このレイヤーの直下」。
        const int defaultHeightLayer = (i > 0) ? (static_cast<int>(i) - 1) : 0;
        const int op =
            EmitMaskOps(maskSource, defaultHeightLayer, layerNodes, compiled.maskOps, emitted, 0);
        if (op >= 0) {
            compiled.layers[i].mask.source = compositor::MaskSource::Node;
            compiled.layers[i].mask.maskOp = op;
        }
    }

    RecordMaskOpSources(emitted, compiled);
    RecordLayerSources(layerNodes, compiled);
    return compiled;
}

CompiledGraph NodeGraph::CompileLayers() const {
    return CompileChainFrom(ChainTop());
}

CompiledGraph NodeGraph::CompileLayersTo(GraphId nodeId, GraphId outputPin) const {
    const Node* node = FindNode(nodeId);
    if (node == nullptr) {
        return CompileLayers();
    }

    // **どの出力を見ているか。** 指定が無ければ最初の出力（レイヤーなら Result）。
    // 堆積は Result と Mask の 2 つを出すので、ノードの種類だけでは決まらない。
    ValueType previewType =
        node->outputs.empty() ? ValueType::Material : node->outputs.front().valueType;
    for (const Pin& pin : node->outputs) {
        if (pin.id == outputPin) {
            previewType = pin.valueType;
            break;
        }
    }

    // マスクの出力を見ている間は、**そのマスクを白黒で貼って**見せる。
    // マスクは見ながら調整するものなので、選んだだけで結果が分かるようにする。
    if (previewType == ValueType::Mask) {
        // **下地のハイトを見ないマスクは、平らな板に貼る。**
        // 画像やノイズは地形と無関係なので、地形の上に貼ると起伏の陰影に紛れて
        // 濃淡が読めない。川筋や傾斜のように下地を見るマスクは地形の上に貼る
        // （そちらは地形との対応こそが見たいもの）。
        const bool onTerrain = MaskDependsOnHeight(*node);
        ChainTrace trace;
        CompiledGraph compiled;
        if (onTerrain) {
            // 堆積のようにレイヤーでもあるノードは、PreviewTop が自分を返す
            // （＝そのレイヤーまで合成した状態の上にマスクを貼る）。
            compiled = CompileChainFrom(PreviewTop(nodeId), &trace);
        } else {
            compiled.layers.push_back(compositor::MaterialStack::MakeBaseLayer());
        }
        // プレビューの塗りレイヤーは列の一番上に積むので、Height の起点は
        // 「いまの一番上」（＝入力に繋いだチェーンの天面）になる。
        const int defaultHeightLayer =
            compiled.layers.empty() ? 0 : (static_cast<int>(compiled.layers.size()) - 1);
        // プレビューで見ている出力ピンが、その何番目の Mask 出力か。
        size_t previewOutputIndex = 0;
        {
            size_t index = 0;
            for (const Pin& pin : node->outputs) {
                if (pin.valueType != ValueType::Mask) {
                    continue;
                }
                if (pin.id == outputPin) {
                    previewOutputIndex = index;
                    break;
                }
                ++index;
            }
        }
        const int op = EmitMaskOps({node, previewOutputIndex}, defaultHeightLayer,
                                   trace.layerNodes, compiled.maskOps, trace.emitted, 0);

        // 下地を黒で覆ってからマスクを白で塗る。マスクの値がそのまま濃淡になる。
        compositor::MaterialLayer cover =
            MakeMaskPreviewLayer("Mask 0", {compositor::kMaskPreviewLow,
                                           compositor::kMaskPreviewLow,
                                           compositor::kMaskPreviewLow});
        cover.mask.source = compositor::MaskSource::Constant;
        cover.mask.constant = 1.0f;
        compiled.layers.push_back(cover);

        compositor::MaterialLayer paint = MakeMaskPreviewLayer(
            "Mask 1", {compositor::kMaskPreviewHigh, compositor::kMaskPreviewHigh,
                       compositor::kMaskPreviewHigh});
        paint.mask.constant = 1.0f;
        if (op >= 0) {
            paint.mask.source = compositor::MaskSource::Node;
            paint.mask.maskOp = op;
        }
        compiled.layers.push_back(paint);
        RecordMaskOpSources(trace.emitted, compiled);
        RecordLayerSources(trace.layerNodes, compiled);
        return compiled;
    }
    if (node->kind == NodeKind::Path) {
        // パスは Base に繋いだ地形（沿う面）を見せる。
        return CompileChainFrom(PreviewTop(nodeId));
    }
    if (!IsLayerNodeKind(node->kind)) {
        return CompileLayers();
    }
    return CompileChainFrom(node);
}

}  // namespace tg::graph
