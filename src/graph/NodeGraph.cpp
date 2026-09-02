#include "graph/NodeGraph.h"

#include "compositor/MaterialStack.h"

#include <algorithm>
#include <array>
#include <unordered_set>
#include <utility>

namespace tg::graph {
namespace {

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
constexpr std::array<PinDefinition, 2> kFilterNodePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
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

constexpr std::array<PinDefinition, 1> kOutputNodePins = {{
    {PinKind::Input, ValueType::Material, "Material"},
}};

// ソースノードのピン。**入力を持たない。**
constexpr std::array<PinDefinition, 1> kSourceNodePins = {{
    {PinKind::Output, ValueType::Material, "Result"},
}};

constexpr std::array<NodeDefinition, 12> kNodeDefinitions = {{
    {NodeKind::Heightmap, "heightmap", "Heightmap", kSourceNodePins},
    {NodeKind::Surface, "surface", "Surface", kLayerNodePins},
    {NodeKind::Shape, "shape", "Shape", kLayerNodePins},
    {NodeKind::Liquid, "liquid", "Liquid", kLayerNodePins},
    {NodeKind::Blur, "heightmapBlur", "Heightmap Blur", kFilterNodePins},
    {NodeKind::Sediment, "sediment", "Sediment", kFilterNodePins},
    {NodeKind::MaskImage, "maskImage", "Mask Image", kMaskSourcePins},
    {NodeKind::MaskFluvial, "maskFluvial", "Mask Fluvial", kMaskFromHeightPins},
    {NodeKind::MaskSlope, "maskSlope", "Mask Slope", kMaskFromHeightPins},
    {NodeKind::MaskLevels, "maskLevels", "Mask Levels", kMaskFilterPins},
    {NodeKind::MaskBlend, "maskBlend", "Mask Blend", kMaskBlendPins},
    {NodeKind::Output, "output", "Output", kOutputNodePins},
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
           kind == NodeKind::Sediment;
}

bool IsSourceNodeKind(NodeKind kind) {
    return kind == NodeKind::Heightmap;
}

bool IsMaskNodeKind(NodeKind kind) {
    return kind == NodeKind::MaskImage || kind == NodeKind::MaskFluvial ||
           kind == NodeKind::MaskSlope || kind == NodeKind::MaskLevels ||
           kind == NodeKind::MaskBlend;
}

// 下地の Height を読むマスクか。**チェーンのどこを読むか**を Base 入力で指す。
bool IsHeightMaskNodeKind(NodeKind kind) {
    return kind == NodeKind::MaskFluvial || kind == NodeKind::MaskSlope;
}

bool IsPreviewableNodeKind(NodeKind kind) {
    // マスクは見ながら調整するものなので、どのマスクノードもプレビューできる。
    return IsLayerNodeKind(kind) || IsMaskNodeKind(kind);
}

compositor::LayerKind LayerKindFor(NodeKind kind) {
    switch (kind) {
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
    MarkDirty();
    return true;
}

bool NodeGraph::DeleteLink(GraphId linkId) {
    const size_t oldSize = m_links.size();
    std::erase_if(m_links, [linkId](const Link& link) { return link.id == linkId; });
    if (m_links.size() == oldSize) {
        return false;
    }
    MarkDirty();
    return true;
}

GraphId NodeGraph::CreateNode(NodeKind kind) {
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
    std::erase_if(m_nodes, [nodeId](const Node& candidate) { return candidate.id == nodeId; });
    MarkDirty();
    return true;
}

void NodeGraph::Replace(std::vector<Node> nodes, std::vector<Link> links) {
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
    MarkDirty();
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
    // 川筋ノードは自分ではハイトを作らない。入力に繋いだチェーンを見る。
    if (node != nullptr && node->kind == NodeKind::MaskFluvial) {
        for (const Pin& pin : node->inputs) {
            if (pin.valueType != ValueType::Material) {
                continue;
            }
            if (const Node* source = FindUpstreamNodeForPin(pin.id); source != nullptr) {
                return source;
            }
        }
    }
    return ChainTop();
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

// マスクのノードを op の列へ落とす（入力が先、出力が後）。
// **同じノードは 1 つの op を共有する。** Mask Blend で合流したり、
// 同じマスクを 2 つのレイヤーで使ったりしても、評価は 1 回で済む。
int NodeGraph::EmitMaskOps(const Node& maskNode, int defaultHeightLayer,
                           const std::vector<const Node*>& layerNodes,
                           compositor::MaskProgram& ops,
                           std::vector<std::pair<const Node*, int>>& emitted, int depth) const {
    // 循環は CanCreateLink が弾いているが、読み込んだファイルが壊れている
    // 可能性もあるので深さでも止める。
    constexpr int kMaxDepth = 32;
    if (depth > kMaxDepth || !IsMaskNodeKind(maskNode.kind)) {
        return -1;
    }
    for (const auto& [node, heightLayer] : emitted) {
        if (node == &maskNode && heightLayer == defaultHeightLayer) {
            // 既に焼いてある。同じ op を共有する。
            for (size_t i = 0; i < emitted.size(); ++i) {
                if (emitted[i].first == node && emitted[i].second == heightLayer) {
                    return static_cast<int>(i);
                }
            }
        }
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
        case NodeKind::MaskFluvial:
            op.kind = compositor::MaskOpKind::Fluvial;
            op.fluvial = settings->fluvial;
            break;
        case NodeKind::MaskSlope:
            op.kind = compositor::MaskOpKind::Slope;
            op.slope = settings->slope;
            break;
        case NodeKind::MaskLevels:
            op.kind = compositor::MaskOpKind::Levels;
            op.levels = settings->levels;
            break;
        case NodeKind::MaskBlend:
            op.kind = compositor::MaskOpKind::Blend;
            op.blend = settings->blend;
            break;
        default:
            return -1;
    }

    // 高さ由来は「チェーンのどこまで合成した Height を読むか」を Base 入力で指す。
    // 繋いでいない / このチェーンに居ないノードなら、呼び出し側の既定へ落とす。
    if (IsHeightMaskNodeKind(maskNode.kind)) {
        op.heightSourceLayer = defaultHeightLayer;
        if (const Node* source = UpstreamOf(maskNode, ValueType::Material)) {
            for (size_t i = 0; i < layerNodes.size(); ++i) {
                if (layerNodes[i] == source) {
                    op.heightSourceLayer = static_cast<int>(i);
                    break;
                }
            }
        }
    }

    // 入力のマスクを先に焼く。**op は自分より前だけを指す。**
    if (maskNode.kind == NodeKind::MaskLevels || maskNode.kind == NodeKind::MaskBlend) {
        if (const Node* a = UpstreamOf(maskNode, ValueType::Mask, 0)) {
            op.inputA = EmitMaskOps(*a, defaultHeightLayer, layerNodes, ops, emitted, depth + 1);
        }
    }
    if (maskNode.kind == NodeKind::MaskBlend) {
        if (const Node* b = UpstreamOf(maskNode, ValueType::Mask, 1)) {
            op.inputB = EmitMaskOps(*b, defaultHeightLayer, layerNodes, ops, emitted, depth + 1);
        }
        // 片方だけ繋いでいるときは繋いだほうを通す（未接続は 0 ではなく「中立」）。
        if (op.inputA < 0 && op.inputB < 0) {
            return -1;
        }
    }
    if (maskNode.kind == NodeKind::MaskLevels && op.inputA < 0) {
        // 入力の無いレベル調整は意味を持たない。
        return -1;
    }

    ops.push_back(op);
    emitted.emplace_back(&maskNode, defaultHeightLayer);
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

CompiledGraph NodeGraph::CompileChainFrom(const Node* top) const {
    const std::vector<const Node*> chain = ChainFrom(top);

    // 遡った順（上→下）を、レイヤー列の順（下→上）へ反転する。
    CompiledGraph compiled;
    // layers と 1 対 1 で並ぶ元ノード。マスクがチェーンのどこを読むかの解決に使う。
    std::vector<const Node*> layerNodes;
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

    // Mask 入力に繋がっているマスクのノードを op の列へ落とし、
    // レイヤーからは添字で参照する（同じ意味の値を 2 か所から編集させない）。
    // 効き方（係数 / カーブ / レベル / 反転）はレイヤー側の設定のまま。
    std::vector<std::pair<const Node*, int>> emitted;
    for (size_t i = 0; i < compiled.layers.size(); ++i) {
        const Node* maskNode = UpstreamOf(*layerNodes[i], ValueType::Mask);
        if (maskNode == nullptr) {
            continue;
        }
        // 高さ由来のマスクで Base を繋いでいないときは「このレイヤーの直下」。
        const int defaultHeightLayer = (i > 0) ? (static_cast<int>(i) - 1) : 0;
        const int op =
            EmitMaskOps(*maskNode, defaultHeightLayer, layerNodes, compiled.maskOps, emitted, 0);
        if (op >= 0) {
            compiled.layers[i].mask.source = compositor::MaskSource::Node;
            compiled.layers[i].mask.maskOp = op;
        }
    }

    if (compiled.layers.empty()) {
        // 空のスタックは操作の起点が無いので、下地 1 枚で補う（読み込み時と同じ方針）。
        compiled.layers.push_back(compositor::MaterialStack::MakeBaseLayer());
    }
    return compiled;
}

CompiledGraph NodeGraph::CompileLayers() const {
    return CompileChainFrom(ChainTop());
}

CompiledGraph NodeGraph::CompileLayersTo(GraphId nodeId) const {
    const Node* node = FindNode(nodeId);
    if (node == nullptr) {
        return CompileLayers();
    }
    // マスクのノードを選んでいる間は、**そのマスクを白黒で貼って**見せる。
    // マスクは見ながら調整するものなので、選んだだけで結果が分かるようにする。
    if (IsMaskNodeKind(node->kind)) {
        CompiledGraph compiled = CompileChainFrom(PreviewTop(nodeId));
        // プレビューの塗りレイヤーは列の一番上に積むので、Height の起点は
        // 「いまの一番上」（＝入力に繋いだチェーンの天面）になる。
        std::vector<const Node*> layerNodes;
        std::vector<std::pair<const Node*, int>> emitted;
        const int defaultHeightLayer =
            compiled.layers.empty() ? 0 : (static_cast<int>(compiled.layers.size()) - 1);
        const int op =
            EmitMaskOps(*node, defaultHeightLayer, layerNodes, compiled.maskOps, emitted, 0);

        // 下地を黒で覆ってからマスクを白で塗る。マスクの値がそのまま濃淡になる。
        compositor::MaterialLayer cover = MakeMaskPreviewLayer("Mask 0", {0.02f, 0.02f, 0.02f});
        cover.mask.source = compositor::MaskSource::Constant;
        cover.mask.constant = 1.0f;
        compiled.layers.push_back(cover);

        compositor::MaterialLayer paint = MakeMaskPreviewLayer("Mask 1", {0.85f, 0.88f, 0.92f});
        paint.mask.constant = 1.0f;
        if (op >= 0) {
            paint.mask.source = compositor::MaskSource::Node;
            paint.mask.maskOp = op;
        }
        compiled.layers.push_back(paint);
        return compiled;
    }
    if (!IsLayerNodeKind(node->kind)) {
        return CompileLayers();
    }
    return CompileChainFrom(node);
}

}  // namespace tg::graph
