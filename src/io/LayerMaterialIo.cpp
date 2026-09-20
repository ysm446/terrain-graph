#include "io/LayerMaterialIo.h"
#include "graph/SurfacePresetGraph.h"
#include <limits>
#include <cmath>
namespace tg::io {
using nlohmann::json;
namespace {
struct Reader {
    bool valid = true;
    const json& Field(const json& object, const char* key) {
        static const json missing;
        if (!object.is_object() || !object.contains(key)) { valid = false; return missing; }
        return object[key];
    }
    const json& Array(const json& object, const char* key) {
        static const json empty = json::array();
        const auto& value = Field(object, key);
        if (!value.is_array()) { valid = false; return empty; }
        return value;
    }
    uint32_t UInt(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<int64_t>() < 0)) { valid = false; return 0; }
        const auto number = value.get<uint64_t>();
        if (number > std::numeric_limits<uint32_t>::max()) { valid = false; return 0; }
        return static_cast<uint32_t>(number);
    }
    float Float(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_number()) { valid = false; return 0; }
        return value.get<float>();
    }
    bool Bool(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_boolean()) { valid = false; return false; }
        return value.get<bool>();
    }
    std::string String(const json& object, const char* key) {
        const auto& value = Field(object, key);
        if (!value.is_string()) { valid = false; return {}; }
        return value.get<std::string>();
    }
};
json WritePresetMaterial(const graph::PresetMaterial& material) {
    json m = {{"material", material.material}, {"uvRepeat", material.uvRepeatMeters},
        {"worldUv", material.worldUv}, {"baseColor", material.baseColor}, {"roughness", material.roughness},
        {"metallic", material.metallic}, {"ambientOcclusion", material.ambientOcclusion},
        {"blendMode", material.blendMode}, {"heightGate", material.heightGate},
        {"heightGateThreshold", material.heightGateThreshold}, {"heightGateSoftness", material.heightGateSoftness}, {"mask", nullptr}};
    if (material.mask) {
        const auto& mask = *material.mask;
        m["mask"] = {{"shape", static_cast<uint32_t>(mask.shape)}, {"edgeSide", static_cast<uint32_t>(mask.edgeSide)},
            {"laneOffset", mask.laneOffsetMeters}, {"trackSpacing", mask.trackSpacingMeters}, {"trackWidth", mask.trackWidthMeters},
            {"feather", mask.featherMeters}, {"tracksFromLanes", mask.tracksFromLanes}, {"bothLanes", mask.bothLanes},
            {"edgeWidth", mask.edgeWidthMeters}, {"noiseScale", mask.noiseScaleMeters}, {"threshold", mask.threshold},
            {"softness", mask.softness}, {"seed", mask.seed}, {"breakupAmount", mask.breakupAmount},
            {"breakupScale", mask.breakupScaleMeters}, {"strength", mask.strength}, {"invert", mask.invert}};
    }
    if (!material.enabled) m["enabled"] = false;
    return m;
}

graph::PresetMaterial ReadPresetMaterial(Reader& r, const json& m, uint32_t version) {
    graph::PresetMaterial material;
    if (m.contains("enabled")) material.enabled = r.Bool(m, "enabled");
    material.material = r.UInt(m, "material"); material.uvRepeatMeters = r.Float(m, "uvRepeat"); material.worldUv = r.Bool(m, "worldUv");
    material.roughness = r.Float(m, "roughness");
    const auto& color = r.Array(m, "baseColor");
    if (color.size() != 3) r.valid = false;
    for (size_t i = 0; i < color.size() && i < 3; ++i) {
        if (!color[i].is_number()) r.valid = false;
        else material.baseColor[i] = color[i].get<float>();
    }
    if (version >= 2) {
        material.metallic = r.Float(m, "metallic"); material.ambientOcclusion = r.Float(m, "ambientOcclusion");
        material.blendMode = r.UInt(m, "blendMode"); material.heightGate = r.UInt(m, "heightGate");
        material.heightGateThreshold = r.Float(m, "heightGateThreshold"); material.heightGateSoftness = r.Float(m, "heightGateSoftness");
        const auto& mask = r.Field(m, "mask");
        if (!mask.is_null()) {
            graph::RoadMaskNodeSettings settings;
            settings.shape = static_cast<graph::RoadMaskShape>(r.UInt(mask, "shape"));
            settings.edgeSide = static_cast<graph::RoadMaskSide>(r.UInt(mask, "edgeSide"));
            settings.laneOffsetMeters = r.Float(mask, "laneOffset"); settings.trackSpacingMeters = r.Float(mask, "trackSpacing");
            settings.trackWidthMeters = r.Float(mask, "trackWidth"); settings.featherMeters = r.Float(mask, "feather");
            settings.tracksFromLanes = r.Bool(mask, "tracksFromLanes"); settings.bothLanes = r.Bool(mask, "bothLanes");
            settings.edgeWidthMeters = r.Float(mask, "edgeWidth"); settings.noiseScaleMeters = r.Float(mask, "noiseScale");
            settings.threshold = r.Float(mask, "threshold"); settings.softness = r.Float(mask, "softness"); settings.seed = r.UInt(mask, "seed");
            settings.breakupAmount = r.Float(mask, "breakupAmount"); settings.breakupScaleMeters = r.Float(mask, "breakupScale");
            settings.strength = r.Float(mask, "strength"); settings.invert = r.Bool(mask, "invert");
            material.mask = settings;
        }
    }
    return material;
}

}


json WriteLayerMaterial(const graph::LayerMaterial& m) {
    json body = {{"name", m.name}, {"displacement", m.displacementMeters}, {"layerBlendRange", m.layerBlendRange}, {"materials", json::array()}};
    for (const auto& layer : m.materials) body["materials"].push_back(WritePresetMaterial(layer));
    if (m.materialGraph) {
        json g = {{"nextId", m.materialGraph->nextId}, {"nodes", json::array()}};
        for (const auto& node : m.materialGraph->nodes)
            g["nodes"].push_back({{"id", node.id}, {"kind", static_cast<uint32_t>(node.kind)}, {"inputs", node.inputs}, {"position", node.position}, {"settings", WritePresetMaterial(node.settings)}});
        body["materialGraph"] = std::move(g);
    }
    return body;
}
bool ReadLayerMaterial(const json& body, graph::LayerMaterial& material, std::string& error) {
    Reader r;
    graph::LayerMaterial candidate;
    candidate.name = r.String(body, "name");
    candidate.displacementMeters = r.Float(body, "displacement");
    candidate.layerBlendRange = r.Float(body, "layerBlendRange");
    const auto& layers = r.Array(body, "materials");
    if (layers.empty() || layers.size() > 4) { error = "合成材質は1〜4層です"; return false; }
    for (const auto& layer : layers) {
        candidate.materials.push_back(ReadPresetMaterial(r, layer, 2));
        if (!graph::ValidatePresetMaterial(candidate.materials.back(), error)) return false;
    }
    if (body.contains("materialGraph")) {
        const auto& g = r.Field(body, "materialGraph");
        candidate.materialGraph.emplace();
        candidate.materialGraph->nextId = r.UInt(g, "nextId");
        const auto& nodes = r.Array(g, "nodes");
        if (nodes.size() > 32) { error = "合成グラフは最大32ノードです"; return false; }
        for (const auto& n : nodes) {
            graph::PresetNode node;
            node.id = r.UInt(n, "id"); node.kind = static_cast<graph::PresetNodeKind>(r.UInt(n, "kind"));
            const auto& inputs = r.Array(n, "inputs"); const auto& pos = r.Array(n, "position");
            if (inputs.size() != 3 || pos.size() != 2) r.valid = false;
            for (size_t i = 0; i < inputs.size() && i < 3; ++i) node.inputs[i] = r.UInt(json{{"v", inputs[i]}}, "v");
            for (size_t i = 0; i < pos.size() && i < 2; ++i) node.position[i] = r.Float(json{{"v", pos[i]}}, "v");
            node.settings = ReadPresetMaterial(r, r.Field(n, "settings"), 2);
            candidate.materialGraph->nodes.push_back(node);
        }
        if (!graph::ValidatePresetGraph(*candidate.materialGraph, error)) return false;
    }
    if (!r.valid || !std::isfinite(candidate.displacementMeters) || candidate.displacementMeters < 0 || candidate.displacementMeters > 10 ||
        !std::isfinite(candidate.layerBlendRange) || candidate.layerBlendRange < 0 || candidate.layerBlendRange > 1) {
        error = "レイヤーマテリアルの形式・値が不正です"; return false;
    }
    material = std::move(candidate); return true;
}
void MapLayerMaterials(json& body, const std::function<json(const json&)>& convert) {
    const auto map = [&](json& settings) { if (settings.is_object() && settings.contains("material")) settings["material"] = convert(settings["material"]); };
    if (body.contains("materials") && body["materials"].is_array()) for (auto& layer : body["materials"]) map(layer);
    if (body.contains("materialGraph") && body["materialGraph"].is_object() && body["materialGraph"].contains("nodes") && body["materialGraph"]["nodes"].is_array())
        for (auto& node : body["materialGraph"]["nodes"]) if (node.is_object() && node.contains("settings")) map(node["settings"]);
}
}
