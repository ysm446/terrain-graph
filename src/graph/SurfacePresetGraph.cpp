#include "graph/SurfacePresetGraph.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_map>

namespace tg::graph {
namespace {
const PresetNode* Find(const PresetGraph& graph, uint32_t id) {
    const auto it = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const auto& n) { return n.id == id; });
    return it == graph.nodes.end() ? nullptr : &*it;
}
PresetMaterial SourceMaterial(const PresetMaterial& settings) {
    auto result = settings;
    result.mask.reset(); result.blendMode = 0; result.heightGate = 0;
    return result;
}
bool Compile(const PresetGraph& graph, uint32_t id, std::vector<PresetMaterial>& result) {
    const auto* node = Find(graph, id);
    if (!node) { result.emplace_back(); return true; }
    if (node->kind == PresetNodeKind::Material) { result.push_back(SourceMaterial(node->settings)); return true; }
    if (!Compile(graph, node->inputs[0], result)) return false;
    if (node->kind == PresetNodeKind::Output) return true;
    const auto* top = Find(graph, node->inputs[1]);
    const auto* mask = Find(graph, node->inputs[2]);
    if (!top || !mask) return true;
    if (result.size() >= 4) return false;
    auto material = SourceMaterial(top->settings);
    material.mask = mask->settings.mask;
    material.blendMode = node->settings.blendMode;
    material.heightGate = node->settings.heightGate;
    material.heightGateThreshold = node->settings.heightGateThreshold;
    material.heightGateSoftness = node->settings.heightGateSoftness;
    result.push_back(material);
    return true;
}
}
bool ValidatePresetMaterial(const PresetMaterial& material, std::string& error) {
    const auto fail = [&](const char* message) { error = message; return false; };
    const auto range = [](float x, float lo, float hi) { return std::isfinite(x) && x >= lo && x <= hi; };
    if (!range(material.uvRepeatMeters, 0.01f, 100) || !range(material.roughness, 0, 1) ||
        !range(material.metallic, 0, 1) || !range(material.ambientOcclusion, 0, 1) ||
        material.blendMode > 1 || material.heightGate > 2 || !range(material.heightGateThreshold, 0, 1) ||
        !range(material.heightGateSoftness, 0.001f, 1)) return fail("マテリアルの寸法・PBR値・合成条件が不正です");
    for (float channel : material.baseColor) if (!range(channel, 0, 1)) return fail("マテリアル色が不正です");
    if (material.mask) {
        const auto& mask = *material.mask;
        if (static_cast<uint32_t>(mask.shape) > 4 || static_cast<uint32_t>(mask.edgeSide) > 2 ||
            !range(mask.laneOffsetMeters, -100, 100) || !range(mask.trackSpacingMeters, 0, 100) ||
            !range(mask.trackWidthMeters, 0, 100) || !range(mask.featherMeters, 0, 100) ||
            !range(mask.edgeWidthMeters, 0, 100) || !range(mask.noiseScaleMeters, 0.05f, 100) ||
            !range(mask.threshold, 0, 1) || !range(mask.softness, 0.0001f, 1) ||
            !range(mask.breakupAmount, 0, 1) || !range(mask.breakupScaleMeters, 0.05f, 100) ||
            !range(mask.strength, 0, 1)) return fail("プリセットの道路マスクが不正です");
    }
    return true;
}


bool ValidatePresetGraph(const PresetGraph& graph, std::string& error) {
    error.clear();
    const auto fail = [&](const char* message) { error = message; return false; };
    if (!graph.nextId || graph.nextId > 1000000 || graph.nodes.empty() || graph.nodes.size() > 32)
        return fail("プリセットグラフのID・ノード数が不正です（最大32ノード）");
    std::unordered_map<uint32_t, int> state;
    size_t outputs = 0;
    for (const auto& node : graph.nodes) {
        if (!node.id || node.id >= graph.nextId || !state.emplace(node.id, 0).second ||
            static_cast<uint32_t>(node.kind) > 3 || !std::isfinite(node.position[0]) || !std::isfinite(node.position[1]))
            return fail("プリセットノードのID・種類・位置が不正です");
        if (!ValidatePresetMaterial(node.settings, error)) return false;
        if (node.kind == PresetNodeKind::Mask && !node.settings.mask) return fail("マスクノードに設定がありません");
        outputs += node.kind == PresetNodeKind::Output;
        for (uint32_t input = 0; input < 3; ++input) {
            if (!node.inputs[input]) continue;
            const auto* source = Find(graph, node.inputs[input]);
            if (!source) return fail("接続元のプリセットノードがありません");
            const bool material = source->kind == PresetNodeKind::Material;
            const bool surface = material || source->kind == PresetNodeKind::Blend;
            const bool valid = (node.kind == PresetNodeKind::Output && input == 0 && surface) ||
                (node.kind == PresetNodeKind::Blend && ((input == 0 && surface) || (input == 1 && material) ||
                    (input == 2 && source->kind == PresetNodeKind::Mask)));
            if (!valid) return fail("ピンの種類が一致しません。合成の上層には素材を接続してください");
        }
    }
    if (outputs != 1) return fail("プリセットグラフには出力が1つ必要です");
    const std::function<bool(uint32_t)> visit = [&](uint32_t id) {
        if (!id || state[id] == 2) return true;
        if (state[id] == 1) return false;
        state[id] = 1;
        for (auto input : Find(graph, id)->inputs) if (!visit(input)) return false;
        state[id] = 2; return true;
    };
    for (const auto& node : graph.nodes) if (!visit(node.id)) return fail("循環する接続は作成できません");
    // 未出力の枝も、接続する前から4層の制限を検査する。
    for (const auto& node : graph.nodes) if (node.kind == PresetNodeKind::Blend || node.kind == PresetNodeKind::Output) {
        std::vector<PresetMaterial> result;
        if (!Compile(graph, node.id, result)) return fail("合成できるマテリアルは下地を含めて最大4層です");
    }
    return true;
}
uint32_t AddPresetNode(PresetGraph& graph, PresetNodeKind kind, std::array<float, 2> position) {
    if (graph.nodes.size() >= 32 || graph.nextId >= 1000000) return 0;
    PresetNode node; node.id = graph.nextId++; node.kind = kind; node.position = position;
    if (kind == PresetNodeKind::Mask) { node.settings.mask.emplace(); node.settings.mask->shape = RoadMaskShape::Constant; }
    graph.nodes.push_back(node); return node.id;
}
PresetGraph MakePresetGraph(const std::vector<PresetMaterial>& materials) {
    PresetGraph graph;
    uint32_t previous = 0;
    for (size_t i = 0; i < materials.size(); ++i) {
        const float x = float(i) * 250;
        const auto material = AddPresetNode(graph, PresetNodeKind::Material, {x, i ? 150.0f : 0.0f});
        graph.nodes.back().settings = SourceMaterial(materials[i]);
        if (!i) { previous = material; continue; }
        uint32_t mask = 0;
        if (materials[i].mask) {
            mask = AddPresetNode(graph, PresetNodeKind::Mask, {x, 340});
            graph.nodes.back().settings.mask = materials[i].mask;
        }
        previous = AddPresetNode(graph, PresetNodeKind::Blend, {x + 240, 0});
        auto& blend = graph.nodes.back();
        blend.inputs = {i == 1 ? graph.nodes.front().id : 0, material, mask};
        // 直前の合成出力をたどる。IDの間隔には依存しない。
        if (i > 1) for (auto it = graph.nodes.rbegin() + 1; it != graph.nodes.rend(); ++it)
            if (it->kind == PresetNodeKind::Blend) { blend.inputs[0] = it->id; break; }
        blend.settings.blendMode = materials[i].blendMode;
        blend.settings.heightGate = materials[i].heightGate;
        blend.settings.heightGateThreshold = materials[i].heightGateThreshold;
        blend.settings.heightGateSoftness = materials[i].heightGateSoftness;
    }
    AddPresetNode(graph, PresetNodeKind::Output, {float(materials.size()) * 250 + 240, 0});
    graph.nodes.back().inputs[0] = previous;
    return graph;
}
bool CompilePresetMaterials(const LayerMaterial& preset, std::vector<PresetMaterial>& materials, std::string& error) {
    error.clear();
    if (!preset.materialGraph) materials = preset.materials;
    else {
        if (!ValidatePresetGraph(*preset.materialGraph, error)) return false;
        materials.clear();
        for (const auto& node : preset.materialGraph->nodes) if (node.kind == PresetNodeKind::Output)
            if (!Compile(*preset.materialGraph, node.id, materials)) return false;
    }
    if (!materials.empty()) materials[0] = SourceMaterial(materials[0]);
    for (size_t i = 0; i < materials.size(); ++i) if (!materials[i].enabled) {
        if (!i) materials[i] = PresetMaterial{};
        else materials[i].mask.reset();
    }
    return !materials.empty();
}
bool ConnectPresetNodes(PresetGraph& graph, uint32_t source, uint32_t target, uint32_t input, std::string& error) {
    auto candidate = graph;
    auto it = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [target](const auto& n) { return n.id == target; });
    if (it == candidate.nodes.end() || input >= 3) { error = "接続先がありません"; return false; }
    it->inputs[input] = source;
    if (!ValidatePresetGraph(candidate, error)) return false;
    graph = std::move(candidate); return true;
}
bool DeletePresetNode(PresetGraph& graph, uint32_t id) {
    const auto* node = Find(graph, id);
    if (!node || node->kind == PresetNodeKind::Output) return false;
    std::erase_if(graph.nodes, [id](const auto& n) { return n.id == id; });
    for (auto& other : graph.nodes) for (auto& input : other.inputs) if (input == id) input = 0;
    return true;
}
bool AppendPresetLayer(PresetGraph& graph, std::string& error) {
    auto candidate = graph;
    if (!ValidatePresetGraph(candidate, error)) return false;
    if (candidate.nodes.size() + 3 > 32 || candidate.nextId + 3 >= 1000000) {
        error = "追加できるノード数の上限です"; return false;
    }
    auto output = std::find_if(candidate.nodes.begin(), candidate.nodes.end(), [](const auto& n) { return n.kind == PresetNodeKind::Output; });
    const auto outputId = output->id, previous = output->inputs[0];
    const auto position = output->position;
    output->position[0] += 260;
    const auto material = AddPresetNode(candidate, PresetNodeKind::Material, {position[0] - 220, position[1] + 170});
    const auto mask = AddPresetNode(candidate, PresetNodeKind::Mask, {position[0] - 220, position[1] + 350});
    const auto blend = AddPresetNode(candidate, PresetNodeKind::Blend, position);
    candidate.nodes.back().inputs = {previous, material, mask};
    for (auto& node : candidate.nodes) if (node.id == outputId) node.inputs[0] = blend;
    if (!ValidatePresetGraph(candidate, error)) return false;
    graph = std::move(candidate); return true;
}
}  // namespace tg::graph
