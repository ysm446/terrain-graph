// Road Mesh / Shoulder / Mesh Output（道路の路面と路肩のメッシュ）。
//
// Mesh Output に繋がった鎖（Road Path → Road Mesh → 路肩…）ごとに、Road Path の中心線（地形の
// CPU 側のハイト + 縦断曲線）から路面のメッシュを作り、路肩をその端から外へ順に張り出して、
// GeneratedMeshes で描く。形は中心線・バンク・幅が
// 変わったときだけ作り直す（中心線は毎フレーム引いて、値のハッシュで比べる）。
// 材質は毎フレーム定数として差し替える（焼かない。GeneratedMesh.hlsl が画素ごとに評価する）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/Log.h"
#include "io/ProjectWorkspace.h"
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
// 路肩の帯の文脈。車線は持たない（幅だけ。道路端のマスクは帯の端で測る）。
void SetShoulderContext(compositor::LayerMaterialGpu& material, float widthMeters) {
    material.road[0] = widthMeters;
    material.road[1] = 1.0f;
    material.road[2] = 0.0f;
    material.road[3] = 0.0f;
}

// 路肩の形に効く値のハッシュ（内側の帯の形、どちらの端か、路肩の設定）。
uint64_t ShoulderGeometryKey(uint64_t sourceKey, int side, const graph::RoadShoulderSettings& shoulder) {
    uint64_t hash = HashValue(14695981039346656037ull, sourceKey);
    hash = HashValue(hash, side);
    hash = HashValue(hash, shoulder.widthMeters);
    hash = HashValue(hash, shoulder.crossSlopePercent);
    hash = HashValue(hash, shoulder.stepHeightMeters);
    hash = HashValue(hash, shoulder.stepWidthMeters);
    return hash;
}

uint64_t MeshItemId(graph::GraphId output, graph::GraphId node, int side) {
    uint64_t hash = HashValue(14695981039346656037ull, output);
    hash = HashValue(hash, node);
    return HashValue(hash, side);
}

// 材質が無いときの色（リニア）。路面はアスファルト、路肩は少し明るい砂利の目安。
constexpr float kRoadFallbackColor[3] = {0.18f, 0.18f, 0.18f};
constexpr float kShoulderFallbackColor[3] = {0.32f, 0.30f, 0.27f};

}  // namespace

bool Application::SurfaceMaterialGpu(compositor::MaterialAssetId material, float uvRepeatMeters,
                                     compositor::LayerMaterialGpu& out, std::string* error) const {
    const compositor::MaterialAsset* asset = m_materialLibrary.Find(material);
    if (asset == nullptr) return false;
    if (asset->layerMaterial) {
        if (!asset->layerError.empty() || asset->layerGpu.count == 0) {
            if (error) *error = asset->layerError.empty() ? "Layered Material に層がありません" : asset->layerError;
            return false;
        }
        out = asset->layerGpu;
        return true;
    }
    // 通常の Material は 1 層の Layered Material として塗る（帯の座標で、繰り返し長ごとに）。
    compositor::MaterialAsset single;
    single.layerMaterial.emplace();
    graph::PresetMaterial layer;
    layer.material = material;
    layer.uvRepeatMeters = uvRepeatMeters;
    single.layerMaterial->materials.push_back(layer);
    std::string message;
    out = m_materialLibrary.CompileLayerMaterial(single, m_textureLibrary, message);
    if (!message.empty() || out.count == 0) {
        if (error) *error = message.empty() ? "材質を組み立てられません" : message;
        return false;
    }
    return true;
}

void Application::PrepareRoadMeshes() {
    std::vector<renderer::GeneratedMeshItem> items;
    std::vector<graph::GraphId> alive;
    m_roadNodeStatus.clear();
    for (const graph::CompiledRoadMesh& compiled : m_graph.CompileRoadMeshes()) {
        const graph::Node* meshNode = m_graph.FindNode(compiled.roadMesh);
        const graph::Node* pathNode = m_graph.FindNode(compiled.roadPath);
        const auto* mesh = meshNode ? std::get_if<graph::RoadMeshNodeSettings>(&meshNode->settings) : nullptr;
        const auto* path = pathNode ? std::get_if<graph::RoadPathNodeSettings>(&pathNode->settings) : nullptr;
        if (mesh == nullptr || path == nullptr) continue;
        alive.push_back(compiled.output);
        RoadChainCache& cache = m_roadMeshCache[compiled.output];
        RoadNodeStatus& roadStatus = m_roadNodeStatus[compiled.roadMesh];
        roadStatus = {};

        // --- 路面 ---
        graph::RoadProfileCurve base;
        graph::RoadProfileCurve centerline;
        std::string error;
        if (!BuildRoadCenterline(*pathNode, base, centerline, &error)) {
            roadStatus.error = error;
            cache = {};
            continue;
        }
        const uint64_t key = RoadGeometryKey(centerline, path->road, mesh->mesh);
        if (key != cache.road.key) {
            cache.road.key = key;
            cache.road.error.clear();
            cache.road.stride = graph::RoadMeshStride(mesh->mesh);
            cache.lengthMeters = centerline.TotalLength();
            if (!graph::BuildRoadMesh(path->road, centerline, mesh->mesh, cache.road.mesh, &cache.road.error))
                cache.road.mesh = {};
        }
        roadStatus.error = cache.road.error;
        roadStatus.lengthMeters = cache.lengthMeters;
        if (cache.road.mesh.indices.empty()) {
            cache.shoulders.clear();
            continue;
        }
        bool roadItemHasMaterial = false;
        compositor::LayerMaterialGpu roadMaterial;
        roadStatus.vertices = cache.road.mesh.vertices.size();
        roadStatus.triangles = cache.road.mesh.indices.size() / 3;
        {
            renderer::GeneratedMeshItem item;
            item.id = MeshItemId(compiled.output, compiled.roadMesh, 0);
            item.geometryKey = cache.road.key;
            item.geometry = &cache.road.mesh;
            std::copy(std::begin(kRoadFallbackColor), std::end(kRoadFallbackColor), item.fallbackColor);
            item.hasMaterial = SurfaceMaterialGpu(mesh->mesh.material, mesh->mesh.uvRepeatMeters, item.material,
                                                  &roadStatus.materialError);
            if (item.hasMaterial) SetRoadContext(item.material, mesh->mesh);
            roadItemHasMaterial = item.hasMaterial;
            roadMaterial = item.material;
            items.push_back(item);
        }

        // --- 路肩（内側から順に。左右それぞれ、今の外側の端から張り出す） ---
        // 端の帯。左は列 0 が端（隣は列 1）、右は最後の列が端。路肩の帯では最後の列が外側の端。
        struct Edge {
            const RoadStrip* strip = nullptr;
            uint32_t edgeColumn = 0, innerColumn = 0;
            // 内側の帯の材質と、外へ延ばした位置の座標（origin + sign * 外への距離）。
            bool hasMaterial = false;
            compositor::LayerMaterialGpu material;
            float origin = 0, sign = 1;
            const float* fallback = kRoadFallbackColor;
        };
        const float roadWidth = std::clamp(mesh->mesh.widthMeters, graph::kRoadMinWidthMeters, graph::kRoadMaxWidthMeters);
        Edge edges[2] = {{&cache.road, 0, 1, roadItemHasMaterial, roadMaterial, 0.0f, -1.0f, kRoadFallbackColor},
                         {&cache.road, cache.road.stride - 1, cache.road.stride - 2, roadItemHasMaterial, roadMaterial,
                          roadWidth, 1.0f, kRoadFallbackColor}};
        size_t stripIndex = 0;
        for (const graph::GraphId shoulderId : compiled.shoulders) {
            const graph::Node* shoulderNode = m_graph.FindNode(shoulderId);
            const auto* shoulder = shoulderNode ? std::get_if<graph::ShoulderNodeSettings>(&shoulderNode->settings) : nullptr;
            if (shoulder == nullptr) continue;
            RoadNodeStatus& status = m_roadNodeStatus[shoulderId];
            const graph::RoadShoulderSettings& settings = shoulder->shoulder;
            for (int side = 0; side < 2; ++side) {
                const bool wanted = settings.side == graph::RoadShoulderSide::Both ||
                                    (side == 0 && settings.side == graph::RoadShoulderSide::Left) ||
                                    (side == 1 && settings.side == graph::RoadShoulderSide::Right);
                if (!wanted || edges[side].strip == nullptr) continue;
                if (cache.shoulders.size() <= stripIndex) cache.shoulders.emplace_back();
                RoadStrip& strip = cache.shoulders[stripIndex++];
                const uint64_t stripKey = ShoulderGeometryKey(edges[side].strip->key ^ edges[side].edgeColumn, side, settings);
                if (stripKey != strip.key) {
                    strip.key = stripKey;
                    strip.error.clear();
                    if (!graph::BuildRoadShoulder(edges[side].strip->mesh, edges[side].strip->stride,
                                                  edges[side].edgeColumn, edges[side].innerColumn, settings,
                                                  strip.mesh, strip.stride, &strip.error))
                        strip.mesh = {};
                }
                if (!strip.error.empty()) status.error = strip.error;
                if (strip.mesh.indices.empty()) {
                    edges[side].strip = nullptr;  // この側はここで途切れる
                    continue;
                }
                status.vertices += strip.mesh.vertices.size();
                status.triangles += strip.mesh.indices.size() / 3;
                status.lengthMeters = cache.lengthMeters;
                renderer::GeneratedMeshItem item;
                item.id = MeshItemId(compiled.output, shoulderId, side + 1);
                item.geometryKey = strip.key;
                item.geometry = &strip.mesh;
                std::copy(std::begin(kShoulderFallbackColor), std::end(kShoulderFallbackColor), item.fallbackColor);
                item.hasMaterial = SurfaceMaterialGpu(settings.material, settings.uvRepeatMeters, item.material,
                                                      &status.materialError);
                if (item.hasMaterial) SetShoulderContext(item.material, settings.widthMeters);
                // 内側の境界。内側の帯の材質と、境界マテリアルのマスク・ハイト。
                item.hasInner = edges[side].hasMaterial;
                item.innerMaterial = edges[side].material;
                item.innerOrigin = edges[side].origin;
                item.innerSign = edges[side].sign;
                std::copy(edges[side].fallback, edges[side].fallback + 3, item.innerFallbackColor);
                if (!settings.boundaryPath.empty() || !settings.boundaryUid.empty()) {
                    if (const BoundaryAsset* boundary = AcquireBoundary(settings.boundaryPath, settings.boundaryUid)) {
                        if (!boundary->error.empty()) {
                            status.materialError = boundary->error;
                        } else {
                            auto& b = item.boundary;
                            b.maskIndex = m_textureLibrary.SrvIndex(boundary->mask, false);
                            b.heightIndex = m_textureLibrary.SrvIndex(boundary->height, false);
                            // 幅は路肩の幅に収める（反対側の端まで食い込ませない）。
                            b.widthMeters = std::min(boundary->widthMeters, std::max(settings.widthMeters, 0.0f));
                            b.repeatMeters = boundary->repeatMeters;
                            b.depthMeters = boundary->depthMeters;
                            b.heightCenter = boundary->heightCenter;
                            b.alongU = boundary->alongU;
                            b.invertMask = boundary->invertMask;
                        }
                    }
                }
                items.push_back(item);
                const float stripWidth = std::clamp(settings.widthMeters, graph::kShoulderMinWidthMeters,
                                                    graph::kShoulderMaxWidthMeters);
                edges[side] = {&strip, strip.stride - 1, strip.stride - 2, item.hasMaterial, item.material, stripWidth, 1.0f,
                               kShoulderFallbackColor};
            }
        }
        cache.shoulders.resize(stripIndex);
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

    DrawRoadNodeStatus(node.id, "Road Path を繋ぎ、Mesh Output へ繋ぐと描く");
    return changed;
}

void Application::DrawRoadNodeStatus(graph::GraphId nodeId, const char* disconnectedHint) {
    ui::SectionHeader("状態");
    const auto found = m_roadNodeStatus.find(nodeId);
    if (found == m_roadNodeStatus.end()) {
        ui::HintText("%s", disconnectedHint);
        return;
    }
    const RoadNodeStatus& status = found->second;
    const ImVec4 warn = ImGui::ColorConvertU32ToFloat4(ui::WarnColor());
    if (!status.error.empty()) ImGui::TextColored(warn, "%s", status.error.c_str());
    if (!status.materialError.empty()) ImGui::TextColored(warn, "%s", status.materialError.c_str());
    if (ui::BeginPropertyTable("roadNodeStatusRows")) {
        ui::PropertyValue("延長", "%.0f m", status.lengthMeters);
        ui::PropertyValue("メッシュ", "頂点 %zu / 三角形 %zu", status.vertices, status.triangles);
        ui::EndPropertyTable();
    }
}

Application::BoundaryAsset* Application::AcquireBoundary(const std::string& path, const std::string& uid) {
    const std::string key = uid.empty() ? path : uid;
    if (key.empty()) return nullptr;
    if (const auto found = m_boundaries.find(key); found != m_boundaries.end()) return &found->second;
    BoundaryAsset& asset = m_boundaries[key];
    asset.uid = uid;
    asset.path = m_workspace.Resolve(nlohmann::json{{"path", path}, {"uid", uid}});
    nlohmann::json body;
    if (asset.path.empty() || !m_workspace.ReadAsset(asset.path, "boundary-material-asset", body)) {
        asset.error = "境界マテリアルを読めません: " + path;
        TG_LOG_WARN("%s", asset.error.c_str());
        return &asset;
    }
    asset.name = io::ProjectWorkspace::String(body, "name");
    if (asset.name.empty()) {
        const auto stem = asset.path.stem().u8string();
        asset.name.assign(reinterpret_cast<const char*>(stem.c_str()), stem.size());
    }
    const auto number = [&](const char* key, float fallback) {
        return body.contains(key) && body[key].is_number() ? body[key].get<float>() : fallback;
    };
    const auto flag = [&](const char* key) { return body.contains(key) && body[key].is_boolean() && body[key].get<bool>(); };
    asset.widthMeters = std::clamp(number("width", asset.widthMeters), 0.01f, 20.0f);
    asset.repeatMeters = std::clamp(number("repeat", asset.repeatMeters), 0.01f, 1000.0f);
    asset.depthMeters = std::clamp(number("depth", asset.depthMeters), 0.0f, 1.0f);
    asset.heightCenter = std::clamp(number("heightCenter", asset.heightCenter), 0.0f, 1.0f);
    asset.alongU = flag("alongU");
    asset.invertMask = flag("invertMask");
    const auto texture = [&](const char* key) {
        if (!body.contains(key) || !body[key].is_object()) return compositor::kNoTexture;
        const auto file = m_workspace.Resolve(body[key]);
        return file.empty() ? compositor::kNoTexture : m_textureLibrary.Load(m_device, m_pipelineCache, file);
    };
    asset.mask = texture("mask");
    asset.height = texture("height");
    if (asset.mask == compositor::kNoTexture) {
        asset.error = "境界マテリアルのマスク画像を読めません: " + asset.name;
        TG_LOG_WARN("%s", asset.error.c_str());
    }
    return &asset;
}

bool Application::SaveBoundary(BoundaryAsset& asset) {
    const auto reference = [&](compositor::TextureId id) -> nlohmann::json {
        const compositor::LibraryTexture* texture = m_textureLibrary.Find(id);
        return texture ? m_workspace.Reference(texture->path) : nlohmann::json();
    };
    nlohmann::json body = {{"name", asset.name}, {"width", asset.widthMeters}, {"repeat", asset.repeatMeters},
                           {"depth", asset.depthMeters}, {"heightCenter", asset.heightCenter},
                           {"alongU", asset.alongU}, {"invertMask", asset.invertMask},
                           {"mask", reference(asset.mask)}, {"height", reference(asset.height)}};
    if (!asset.uid.empty()) body["uid"] = asset.uid;
    std::filesystem::path path = asset.path;
    if (path.empty() || !m_workspace.SaveAsset(path, "boundary-material-asset", body)) {
        TG_LOG_WARN("境界マテリアルを保存できませんでした: %s", asset.name.c_str());
        return false;
    }
    asset.dirty = false;
    TG_LOG_INFO("境界マテリアルを保存しました: %s", asset.name.c_str());
    return true;
}

bool Application::DrawShoulderSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::ShoulderNodeSettings>(&node.settings);
    if (settings == nullptr) return false;
    graph::RoadShoulderSettings& shoulder = settings->shoulder;
    const graph::RoadShoulderSettings defaults;
    bool changed = false;

    ui::SectionHeader("路肩");
    if (ui::BeginPropertyTable("shoulderShapeRows")) {
        static const char* const kSides[] = {"左右", "左", "右"};
        int side = static_cast<int>(shoulder.side);
        if (ui::PropertyCombo("側", &side, kSides, 3, 0,
                              "どちらの端に張り出すか（進行方向に向かって）。左右で材質や幅を変えるときは、"
                              "左と右の Shoulder を続けて繋ぐ")) {
            shoulder.side = static_cast<graph::RoadShoulderSide>(side);
            changed = true;
        }
        changed |= ui::PropertyFloat("幅", &shoulder.widthMeters, graph::kShoulderMinWidthMeters,
                                     graph::kShoulderMaxWidthMeters, defaults.widthMeters,
                                     "路肩の幅（m）。幅方向は約 1 m ごとに割る", "%.2f m");
        changed |= ui::PropertyFloat("横断勾配", &shoulder.crossSlopePercent, -50.0f, 50.0f, defaults.crossSlopePercent,
                                     "外側へ向かって下がる勾配（%）。負なら上がる", "%.1f %%");
        changed |= ui::PropertyFloat("段差", &shoulder.stepHeightMeters, 0.0f, 0.5f, defaults.stepHeightMeters,
                                     "内側の端で下げる高さ（m）。舗装の端や縁石の段。0 で段差なし", "%.3f m");
        if (shoulder.stepHeightMeters > 0.0f) {
            changed |= ui::PropertyFloat("段差の幅", &shoulder.stepWidthMeters, 0.005f, 1.0f, defaults.stepWidthMeters,
                                         "段差を下りきるまでの水平の幅（m）", "%.3f m");
        }
        ui::EndPropertyTable();
    }

    ui::SectionHeader("材質");
    if (ui::BeginPropertyTable("shoulderMaterialRows")) {
        changed |= DrawMaterialSlotRow("材質", shoulder.material, m_materialLibrary, m_pendingAssetReveal, true, true);
        const compositor::MaterialAsset* asset = m_materialLibrary.Find(shoulder.material);
        if (asset == nullptr || !asset->layerMaterial) {
            changed |= ui::PropertyFloat("繰り返し長", &shoulder.uvRepeatMeters, 0.1f, 100.0f, defaults.uvRepeatMeters,
                                         "模様が 1 周する長さ（m）", "%.2f m", ImGuiSliderFlags_Logarithmic);
        }
        ui::EndPropertyTable();
    }
    ui::HintText("路肩の座標（内側の端からの横位置、始点からの道のり）で貼る。なしなら砂利色の灰色で塗る");

    // --- 内側の境界（境界マテリアル） ---
    ui::SectionHeader("内側の境界");
    if (ui::BeginPropertyTable("shoulderBoundaryRows", "マスクを反転")) {
        const std::vector<std::filesystem::path> files = m_workspace.AssetsWithExtension(L".tgboundary");
        const std::filesystem::path current =
            (shoulder.boundaryPath.empty() && shoulder.boundaryUid.empty())
                ? std::filesystem::path{}
                : m_workspace.Resolve(nlohmann::json{{"path", shoulder.boundaryPath}, {"uid", shoulder.boundaryUid}});
        const auto display = [](const std::filesystem::path& p) {
            const auto text = p.stem().u8string();
            return std::string(reinterpret_cast<const char*>(text.c_str()));
        };
        const std::string preview = current.empty() ? (shoulder.boundaryPath.empty() ? "なし" : "（見つからない）") : display(current);
        ui::PropertyLabel("境界マテリアル", "内側の帯（路面か内側の路肩）との境目の形。ルート内の .tgboundary から選ぶ");
        ImGui::SetNextItemWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##boundary", preview.c_str())) {
            if (ImGui::Selectable("なし", current.empty())) {
                shoulder.boundaryPath.clear();
                shoulder.boundaryUid.clear();
                changed = true;
            }
            for (const auto& file : files) {
                if (ImGui::Selectable(display(file).c_str(), file == current)) {
                    const nlohmann::json reference = m_workspace.Reference(file);
                    shoulder.boundaryPath = io::ProjectWorkspace::String(reference, "path");
                    shoulder.boundaryUid = io::ProjectWorkspace::String(reference, "uid");
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ui::PropertyEnd();
        BoundaryAsset* boundary = current.empty() ? nullptr : AcquireBoundary(shoulder.boundaryPath, shoulder.boundaryUid);
        if (boundary != nullptr && boundary->error.empty()) {
            const BoundaryAsset boundaryDefaults;
            bool edited = false;
            edited |= DrawTextureSlotRow("マスク", boundary->mask, m_textureLibrary, m_pendingAssetReveal);
            edited |= DrawTextureSlotRow("ハイト", boundary->height, m_textureLibrary, m_pendingAssetReveal);
            edited |= ui::PropertyFloat("境界の幅", &boundary->widthMeters, 0.01f, 20.0f, boundaryDefaults.widthMeters,
                                        "内側の端から、境目の模様を置く幅（m）。路肩の幅より広くはしない", "%.2f m");
            edited |= ui::PropertyFloat("繰り返し長", &boundary->repeatMeters, 0.01f, 100.0f, boundaryDefaults.repeatMeters,
                                        "道に沿って模様が 1 周する長さ（m）", "%.2f m", ImGuiSliderFlags_Logarithmic);
            edited |= ui::PropertyFloat("深さ", &boundary->depthMeters, 0.0f, 0.5f, boundaryDefaults.depthMeters,
                                        "ハイトの凹凸の深さ（m）。(ハイト - 基準) × 2 × 深さ。今は陰影だけに効く", "%.3f m");
            edited |= ui::PropertyFloat("基準の高さ", &boundary->heightCenter, 0.0f, 1.0f, boundaryDefaults.heightCenter,
                                        "ハイト画像の平らな所の値", "%.2f");
            static const char* const kAxes[] = {"V（道に沿う向き）", "U（横切る向き）"};
            int axis = boundary->alongU ? 1 : 0;
            if (ui::PropertyCombo("繰り返しの向き", &axis, kAxes, 2, 0, "画像のどちらの向きを道に沿って繰り返すか")) {
                boundary->alongU = axis == 1;
                edited = true;
            }
            edited |= ui::PropertyBool("マスクを反転", &boundary->invertMask, boundaryDefaults.invertMask,
                                       "マスクは白が内側の帯（路面）、黒が路肩。逆の画像のときに入れる");
            if (edited) boundary->dirty = true;
            ui::PropertyLabelEmpty("shoulderBoundarySave");
            ImGui::BeginDisabled(!boundary->dirty);
            if (ui::Button("境界マテリアルを保存", ui::kWideButtonWidth)) SaveBoundary(*boundary);
            ImGui::EndDisabled();
            ui::PropertyEnd();
        }
        ui::EndPropertyTable();
        if (boundary != nullptr && !boundary->error.empty())
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s", boundary->error.c_str());
        else if (boundary != nullptr && boundary->dirty)
            ui::HintText("境界マテリアルの設定は、このファイルを使うすべての路肩に効く。「境界マテリアルを保存」でファイルへ書く");
    }

    DrawRoadNodeStatus(node.id, "Road Mesh（か内側の Shoulder）を繋ぎ、Mesh Output へ繋ぐと描く");
    return changed;
}

}  // namespace tg
