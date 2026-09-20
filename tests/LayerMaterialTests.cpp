#include "graph/SurfacePresetGraph.h"
#include "io/LayerMaterialIo.h"
#include "io/ProjectWorkspace.h"
#include <iostream>
#include <limits>
using nlohmann::json;
int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* label) { if (!value) { ++failures; std::cerr << label << '\n'; } };
    tg::graph::LayerMaterial layer;
    layer.name = "Layer test"; layer.displacementMeters = 0.15f;
    layer.materials.emplace_back(); layer.materials[0].material = 7;
    layer.materialGraph = tg::graph::MakePresetGraph(layer.materials);
    std::string error;
    check(tg::graph::AppendPresetLayer(*layer.materialGraph, error), "append layer");
    std::vector<tg::graph::PresetMaterial> compiled;
    check(tg::graph::CompilePresetMaterials(layer, compiled, error) && compiled.size() == 2, "compile linked layer");
    const auto validGraph = *layer.materialGraph;
    const auto blend = layer.materialGraph->nodes.back().id;
    check(!tg::graph::ConnectPresetNodes(*layer.materialGraph, blend, blend, 0, error), "reject cycle");
    check(layer.materialGraph->nodes.back().inputs == validGraph.nodes.back().inputs, "failed connection is atomic");
    auto body = tg::io::WriteLayerMaterial(layer);
    tg::graph::LayerMaterial restored;
    check(tg::io::ReadLayerMaterial(body, restored, error), "read material");
    check(tg::io::WriteLayerMaterial(restored) == body, "graph and settings roundtrip");
    auto broken = body; broken["materialGraph"]["nodes"][0]["id"] = -1;
    const auto before = tg::io::WriteLayerMaterial(restored);
    check(!tg::io::ReadLayerMaterial(broken, restored, error) && tg::io::WriteLayerMaterial(restored) == before, "bad ID rejected atomically");
    broken = body; broken["displacement"] = "invalid";
    check(!tg::io::ReadLayerMaterial(broken, restored, error), "bad number rejected");
    broken = body; broken["materialGraph"]["nodes"][1]["inputs"] = json::array({999,0,0});
    check(!tg::io::ReadLayerMaterial(broken, restored, error), "dangling link rejected");
    auto withoutMask = layer;
    for (auto& node : withoutMask.materialGraph->nodes) if (node.kind == tg::graph::PresetNodeKind::Blend) node.inputs[2] = 0;
    check(tg::graph::CompilePresetMaterials(withoutMask, compiled, error) && compiled.size() == 1, "unconnected mask adds no coverage");
    check(tg::graph::AppendPresetLayer(*layer.materialGraph, error) && tg::graph::AppendPresetLayer(*layer.materialGraph, error), "four layers");
    check(!tg::graph::AppendPresetLayer(*layer.materialGraph, error), "fifth layer rejected");

    // 編集用変換では、非表示の素材・マスクも再表示できるよう保持する。
    auto hidden = layer;
    for (auto& node : hidden.materialGraph->nodes) if (node.kind == tg::graph::PresetNodeKind::Material) node.settings.enabled = false;
    std::vector<tg::graph::PresetMaterial> editable;
    check(tg::graph::ExtractPresetLayers(hidden, editable, error) && editable.size() == 4, "extract editable layers");
    check(!editable.front().enabled && editable.front().material == 7 && !editable.back().enabled && editable.back().mask.has_value(), "hidden settings retained");
    std::vector<tg::graph::PresetMaterial> beforeConversion, afterConversion;
    check(tg::graph::CompilePresetMaterials(hidden, beforeConversion, error), "compile hidden graph");
    hidden.materialGraph.reset(); hidden.materials = editable;
    check(tg::graph::CompilePresetMaterials(hidden, afterConversion, error), "compile converted stack");
    auto beforeLayer = hidden; beforeLayer.materials = beforeConversion;
    auto afterLayer = hidden; afterLayer.materials = afterConversion;
    check(tg::io::WriteLayerMaterial(beforeLayer) == tg::io::WriteLayerMaterial(afterLayer), "conversion preserves evaluated materials");
    auto stackBody = tg::io::WriteLayerMaterial(hidden);
    check(tg::io::ReadLayerMaterial(stackBody, restored, error) && tg::io::WriteLayerMaterial(restored) == stackBody, "hidden layer stack roundtrip");

    namespace fs = std::filesystem;
    const auto root = fs::current_path() / "layer-material-test-data";
    tg::io::ProjectWorkspace workspace;
    check(workspace.Open(root), "open workspace");
    auto sourcePath = workspace.UniquePath(root, "source", ".tgmat");
    json source = {{"name", "Source"}, {"maps", json::object()}};
    check(workspace.SaveAsset(sourcePath, "material-asset", source), "save source");
    const auto ref = workspace.Reference(sourcePath);
    tg::io::MapLayerMaterials(body, [&](const json& id) -> json { return id == 7 ? ref : json(0); });
    auto path = workspace.UniquePath(root, "layer", ".tglayer");
    check(workspace.SaveAsset(path, "layer-material-asset", body), "save layer asset");
    check(workspace.Scan(), "scan layer UID");
    json document = {{"materials", json::array({{{"id", 1}, {"asset", workspace.Reference(path)}}})}};
    check(workspace.Expand(document), "expand shared layer and dependencies");
    check(document["materials"].size() == 2 && document["materials"][0]["materials"][0]["material"] == 2, "dependency IDs remapped");
    check(document["materials"][0]["materialGraph"]["nodes"][0]["settings"]["material"] == 2, "graph refs remapped");
    const auto moved = workspace.UniquePath(root, "moved", ".tgmat");
    std::error_code ec; fs::rename(sourcePath, moved, ec);
    check(!ec && workspace.Scan(), "rename source");
    document = {{"materials", json::array({{{"id", 1}, {"asset", workspace.Reference(path)}}})}};
    check(workspace.Expand(document), "UID survives source rename");
    // 新しい数値IDから保存し直しても依存が通常素材として残る。
    document["textures"] = json::array(); document["models"] = json::array(); document["skies"] = json::array();
    auto scene = workspace.UniquePath(root, "scene", ".tgscene");
    check(workspace.SaveScene(scene, document), "scene save with layer before source");
    check(workspace.ReadScene(scene, document), "scene reload");
    check(document["materials"][0]["materials"][0]["material"] == 2, "scene reference preserved");
    json invalid = body;
    tg::io::MapLayerMaterials(invalid, [&](const json&) -> json { return workspace.Reference(path); });
    auto nested = workspace.UniquePath(root, "nested", ".tglayer");
    check(workspace.SaveAsset(nested, "layer-material-asset", invalid), "save unsupported nesting fixture");
    document = {{"materials", json::array({{{"id", 1}, {"asset", workspace.Reference(nested)}}})}};
    check(!workspace.Expand(document), "nested layer dependency rejected");
    std::cout << "Layer material failures: " << failures << '\n';
    return failures ? 1 : 0;
}
