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

constexpr std::array<PinDefinition, 2> kLayerNodePins = {{
    {PinKind::Input, ValueType::Material, "下地"},
    {PinKind::Output, ValueType::Material, "結果"},
}};

constexpr std::array<PinDefinition, 1> kOutputNodePins = {{
    {PinKind::Input, ValueType::Material, "マテリアル"},
}};

constexpr std::array<NodeDefinition, 4> kNodeDefinitions = {{
    {NodeKind::Surface, "surface", "サーフェス", kLayerNodePins},
    {NodeKind::Shape, "shape", "シェイプ", kLayerNodePins},
    {NodeKind::Liquid, "liquid", "水面", kLayerNodePins},
    {NodeKind::Output, "output", "出力", kOutputNodePins},
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
    return kind == NodeKind::Surface || kind == NodeKind::Shape || kind == NodeKind::Liquid;
}

compositor::LayerKind LayerKindFor(NodeKind kind) {
    switch (kind) {
        case NodeKind::Shape:
            return compositor::LayerKind::Shape;
        case NodeKind::Liquid:
            return compositor::LayerKind::Liquid;
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

std::vector<compositor::MaterialLayer> NodeGraph::CompileChainFrom(const Node* top) const {
    std::vector<const Node*> chain;
    std::unordered_set<GraphId> visited;
    const Node* current = top;
    while (current != nullptr && IsLayerNodeKind(current->kind) &&
           visited.insert(current->id).second) {
        chain.push_back(current);
        current = current->inputs.empty() ? nullptr
                                          : FindUpstreamNodeForPin(current->inputs.front().id);
    }

    // 遡った順（上→下）を、レイヤー列の順（下→上）へ反転する。
    std::vector<compositor::MaterialLayer> layers;
    layers.reserve(chain.size());
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (const auto* settings = std::get_if<LayerNodeSettings>(&(*it)->settings)) {
            layers.push_back(settings->layer);
        }
    }
    if (layers.empty()) {
        // 空のスタックは操作の起点が無いので、下地 1 枚で補う（読み込み時と同じ方針）。
        layers.push_back(compositor::MaterialStack::MakeBaseLayer());
    }
    return layers;
}

std::vector<compositor::MaterialLayer> NodeGraph::CompileLayers() const {
    // 出力ノード（最初の 1 つ）から「下地」チェーンを遡る。
    const Node* output = nullptr;
    for (const Node& node : m_nodes) {
        if (node.kind == NodeKind::Output) {
            output = &node;
            break;
        }
    }
    const Node* top = (output != nullptr && !output->inputs.empty())
                          ? FindUpstreamNodeForPin(output->inputs.front().id)
                          : nullptr;
    return CompileChainFrom(top);
}

std::vector<compositor::MaterialLayer> NodeGraph::CompileLayersTo(GraphId nodeId) const {
    const Node* node = FindNode(nodeId);
    if (node == nullptr || !IsLayerNodeKind(node->kind)) {
        return CompileLayers();
    }
    return CompileChainFrom(node);
}

}  // namespace tg::graph
