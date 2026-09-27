// Road Mesh / Mesh Output（道路の路面のメッシュ）。
//
// Mesh Output に繋がった Road Mesh ごとに、Road Path の中心線（地形の CPU 側のハイト +
// 縦断曲線）から路面のメッシュを作り、GeneratedMeshes で描く。形は中心線・バンク・幅が
// 変わったときだけ作り直す（中心線は毎フレーム引いて、値のハッシュで比べる）。
// 材質は毎フレーム定数として差し替える（焼かない。GeneratedMesh.hlsl が画素ごとに評価する）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "graph/RoadMesh.h"
#include "graph/SurfacePresetGraph.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace tg {
namespace {

// FNV-1a。形の見分けにだけ使う。
uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
template <typename T>
uint64_t HashValue(uint64_t hash, const T& value) {
    return HashBytes(hash, &value, sizeof(value));
}

// 形に効く値のハッシュ（中心線、バンク、幅、持ち上げ）。
uint64_t RoadGeometryKey(const graph::RoadProfileCurve& centerline, const graph::RoadPathSettings& road,
                         const graph::RoadMeshSettings& mesh) {
    uint64_t hash = 14695981039346656037ull;
    if (!centerline.points.empty())
        hash = HashBytes(hash, centerline.points.data(), centerline.points.size() * sizeof(centerline.points[0]));
    hash = HashValue(hash, road.bankEnabled);
    if (road.bankEnabled) {
        hash = HashValue(hash, road.designSpeedKmh);
        hash = HashValue(hash, road.frictionCoefficient);
        hash = HashValue(hash, road.smoothBank);
        hash = HashValue(hash, road.bankSmoothMeters);
        for (const graph::RoadBankPoint& point : road.bankPoints) {
            hash = HashValue(hash, point.u);
            hash = HashValue(hash, point.designSpeedKmh);
            hash = HashValue(hash, point.manual);
            hash = HashValue(hash, point.angleDegrees);
        }
    }
    hash = HashValue(hash, mesh.widthMeters);
    hash = HashValue(hash, mesh.surfaceOffsetMeters);
    return hash;
}

// 轍・道路端のマスクが読む道路の文脈（幅・車線数・走行側）。
void SetRoadContext(compositor::LayerMaterialGpu& material, const graph::RoadMeshSettings& mesh) {
    material.road[0] = mesh.widthMeters;
    material.road[1] = static_cast<float>(std::max(mesh.lanesForward, 1));
    material.road[2] = static_cast<float>(std::max(mesh.lanesBackward, 0));
    material.road[3] = mesh.leftHandTraffic ? 1.0f : 0.0f;
}

}  // namespace

bool Application::RoadMaterialGpu(const graph::RoadMeshSettings& mesh, compositor::LayerMaterialGpu& out,
                                  std::string* error) const {
    const compositor::MaterialAsset* asset = m_materialLibrary.Find(mesh.material);
    if (asset == nullptr) return false;
    if (asset->layerMaterial) {
        if (!asset->layerError.empty() || asset->layerGpu.count == 0) {
            if (error) *error = asset->layerError.empty() ? "Layered Material に層がありません" : asset->layerError;
            return false;
        }
        out = asset->layerGpu;
        SetRoadContext(out, mesh);
        return true;
    }
    // 通常の Material は 1 層の Layered Material として塗る（路面の座標で、繰り返し長ごとに）。
    compositor::MaterialAsset single;
    single.layerMaterial.emplace();
    graph::PresetMaterial layer;
    layer.material = mesh.material;
    layer.uvRepeatMeters = mesh.uvRepeatMeters;
    single.layerMaterial->materials.push_back(layer);
    std::string message;
    out = m_materialLibrary.CompileLayerMaterial(single, m_textureLibrary, message);
    if (!message.empty() || out.count == 0) {
        if (error) *error = message.empty() ? "材質を組み立てられません" : message;
        return false;
    }
    SetRoadContext(out, mesh);
    return true;
}

void Application::PrepareRoadMeshes() {
    std::vector<renderer::GeneratedMeshItem> items;
    std::vector<graph::GraphId> alive;
    for (const graph::CompiledRoadMesh& compiled : m_graph.CompileRoadMeshes()) {
        const graph::Node* meshNode = m_graph.FindNode(compiled.roadMesh);
        const graph::Node* pathNode = m_graph.FindNode(compiled.roadPath);
        const auto* mesh = meshNode ? std::get_if<graph::RoadMeshNodeSettings>(&meshNode->settings) : nullptr;
        const auto* path = pathNode ? std::get_if<graph::RoadPathNodeSettings>(&pathNode->settings) : nullptr;
        if (mesh == nullptr || path == nullptr) continue;
        alive.push_back(compiled.roadMesh);
        RoadMeshCache& cache = m_roadMeshCache[compiled.roadMesh];

        graph::RoadProfileCurve base;
        graph::RoadProfileCurve centerline;
        std::string error;
        if (!BuildRoadCenterline(*pathNode, base, centerline, &error)) {
            cache.error = error;
            cache.geometry = {};
            cache.key = 0;
            cache.lengthMeters = 0;
            continue;
        }
        const uint64_t key = RoadGeometryKey(centerline, path->road, mesh->mesh);
        if (key != cache.key) {
            cache.key = key;
            cache.error.clear();
            cache.lengthMeters = centerline.TotalLength();
            if (!graph::BuildRoadMesh(path->road, centerline, mesh->mesh, cache.geometry, &error)) {
                cache.error = error;
                cache.geometry = {};
            }
        }
        if (cache.geometry.indices.empty()) continue;

        renderer::GeneratedMeshItem item;
        item.id = static_cast<uint64_t>(compiled.roadMesh);
        item.geometryKey = cache.key;
        item.geometry = &cache.geometry;
        item.roadWidthMeters = mesh->mesh.widthMeters;
        cache.materialError.clear();
        item.hasMaterial = RoadMaterialGpu(mesh->mesh, item.material, &cache.materialError);
        items.push_back(item);
    }
    for (auto it = m_roadMeshCache.begin(); it != m_roadMeshCache.end();) {
        if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) it = m_roadMeshCache.erase(it);
        else ++it;
    }
    m_generatedMeshes.Update(m_device, items);
}

void Application::DrawGeneratedMeshes(ID3D12GraphicsCommandList* commandList,
                                      const DirectX::XMFLOAT4X4& viewProjection, bool shadow) {
    renderer::GeneratedMeshFrame frame;
    frame.viewProjection = viewProjection;
    frame.cameraPosition = m_renderer.GetCamera().Position();
    frame.shadow = shadow;
    frame.environment = &m_renderer.GetEnvironment();
    frame.iblIntensity = m_renderer.EnvironmentIntensity();
    const renderer::LightSettings& light = m_renderer.EffectiveLight();
    frame.lightDirection = light.Direction();
    frame.lightColor = light.color;
    frame.lightIlluminance = light.illuminance;
    if (!shadow) {
        frame.shadows = m_renderer.InstanceShadows();
        const auto& clouds = m_renderer.InstanceClouds();
        frame.atmosphere = clouds.atmosphere;
        frame.cloudNoiseIndex = clouds.noiseIndex;
        frame.atmosphericMode = clouds.mode;
        frame.ambient = m_renderer.InstanceAmbient();
    }
    m_renderer.RecordInstanceDrawCalls(m_generatedMeshes.Draw(m_device, m_pipelineCache, commandList, frame));
}

bool Application::DrawRoadMeshSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::RoadMeshNodeSettings>(&node.settings);
    if (settings == nullptr) return false;
    graph::RoadMeshSettings& mesh = settings->mesh;
    const graph::RoadMeshSettings defaults;
    bool changed = false;

    ui::SectionHeader("路面");
    if (ui::BeginPropertyTable("roadMeshShapeRows")) {
        changed |= ui::PropertyFloat("幅", &mesh.widthMeters, graph::kRoadMinWidthMeters, graph::kRoadMaxWidthMeters,
                                     defaults.widthMeters, "路面の全幅（m）。幅方向は約 1 m ごとに割る", "%.1f m");
        changed |= ui::PropertyInt("車線数（進行方向）", &mesh.lanesForward, 1, 8, defaults.lanesForward,
                                   "進行方向（Road Path の向き）の車線の数");
        changed |= ui::PropertyInt("車線数（対向）", &mesh.lanesBackward, 0, 8, defaults.lanesBackward,
                                   "対向の車線の数。0 で一方通行");
        const int lanes = std::max(1, mesh.lanesForward + mesh.lanesBackward);
        ui::PropertyValue("車線幅", "%.2f m", mesh.widthMeters / static_cast<float>(lanes));
        static const char* const kTrafficSides[] = {"左側通行", "右側通行"};
        int side = mesh.leftHandTraffic ? 0 : 1;
        if (ui::PropertyCombo("走行側", &side, kTrafficSides, 2, 0,
                              "進行方向の車線が道路のどちら側に並ぶか。轍のマスクを車線に合わせるときに使う")) {
            mesh.leftHandTraffic = side == 0;
            changed = true;
        }
        changed |= ui::PropertyFloat("路面の持ち上げ", &mesh.surfaceOffsetMeters, 0.0f, 2.0f, defaults.surfaceOffsetMeters,
                                     "路面を中心線からどれだけ上げるか（m）。地形の均し（切土・盛土）が入るまで、"
                                     "地形と重なって路面が欠けるのを抑える",
                                     "%.2f m");
        ui::EndPropertyTable();
    }

    ui::SectionHeader("材質");
    if (ui::BeginPropertyTable("roadMeshMaterialRows")) {
        changed |= DrawMaterialSlotRow("材質", mesh.material, m_materialLibrary, m_pendingAssetReveal, true, true);
        const compositor::MaterialAsset* asset = m_materialLibrary.Find(mesh.material);
        if (asset == nullptr || !asset->layerMaterial) {
            changed |= ui::PropertyFloat("繰り返し長", &mesh.uvRepeatMeters, 0.1f, 100.0f, defaults.uvRepeatMeters,
                                         "模様が 1 周する長さ（m）。幅方向と進行方向で同じ", "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
        }
        ui::EndPropertyTable();
    }
    ui::HintText("Material か Layered Material を選ぶ。路面の座標（左端からの横位置、始点からの道のり）で貼る。"
                 "Layered Material は層ごとの繰り返し長を使う。なしなら灰色で塗る");

    const auto cache = m_roadMeshCache.find(node.id);
    ui::SectionHeader("状態");
    if (cache == m_roadMeshCache.end()) {
        ui::HintText("Road Path を繋ぎ、Mesh Output へ繋ぐと描く");
    } else {
        if (!cache->second.error.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s", cache->second.error.c_str());
        if (!cache->second.materialError.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s", cache->second.materialError.c_str());
        if (ui::BeginPropertyTable("roadMeshStatsRows")) {
            ui::PropertyValue("延長", "%.0f m", cache->second.lengthMeters);
            ui::PropertyValue("メッシュ", "頂点 %zu / 三角形 %zu", cache->second.geometry.vertices.size(),
                              cache->second.geometry.indices.size() / 3);
            ui::EndPropertyTable();
        }
    }
    return changed;
}

}  // namespace tg
