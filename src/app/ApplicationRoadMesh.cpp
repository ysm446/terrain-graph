// Road Mesh / Shoulder / Mesh Output（道路の路面と路肩のメッシュ）。
//
// Mesh Output に繋がった鎖（Road Path → Road Mesh → 路肩…）ごとに、Road Path の中心線（地形の
// CPU 側のハイト + 縦断曲線）から路面のメッシュを作り、路肩をその端から外へ順に張り出して、
// GeneratedMeshes で描く。形は中心線・バンク・幅が
// 変わったときだけ作り直す（中心線は毎フレーム引いて、値のハッシュで比べる）。
// 材質は毎フレーム定数として差し替える（焼かない。GeneratedMesh.hlsl が画素ごとに評価する）。
//
// **鎖の途中のノードを選んでいる間は、そのノードより下流の帯を作らず、描かない。** Road Path を選べば
// 中心線のカーブだけ、Road Mesh を選べば路面だけ、Shoulder を選べば路面とその路肩まで。点をドラッグ
// している間に路面・路肩・区画線（と Mask Mesh の足跡 → 本体スタックの改版）が毎フレーム作り直される
// のを避けるためで、下流のキャッシュと足跡はそのまま残し、選択を外したときに 1 回だけ作り直す。
// 出口の表示フラグとは別の仕組み（フラグは触らない。選択はプロパティに出すノード = m_selectedGraphNode）。

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
    hash = HashValue(hash, static_cast<uint32_t>(shoulder.shape));
    for (const graph::RoadSectionPoint& point : shoulder.section) {
        hash = HashValue(hash, point.acrossMeters);
        hash = HashValue(hash, point.heightMeters);
    }
    return hash;
}

uint64_t MeshItemId(graph::GraphId output, graph::GraphId node, int side) {
    uint64_t hash = HashValue(14695981039346656037ull, output);
    hash = HashValue(hash, node);
    return HashValue(hash, side);
}

// 区画線の形に効く値のハッシュ（路面の形、車線の並び、区画線の設定。材質は含めない）。
uint64_t MarkingGeometryKey(uint64_t roadKey, const graph::RoadMeshSettings& mesh,
                            const graph::RoadMarkingSettings& marking) {
    uint64_t hash = HashValue(14695981039346656037ull, roadKey);
    hash = HashValue(hash, mesh.lanesForward);
    hash = HashValue(hash, mesh.lanesBackward);
    hash = HashValue(hash, mesh.leftHandTraffic);
    for (const graph::RoadMarkingLine& line : marking.lines) {
        hash = HashValue(hash, line.enabled);
        hash = HashValue(hash, line.dashed);
        hash = HashValue(hash, line.widthMeters);
    }
    hash = HashValue(hash, marking.edgeInsetMeters);
    hash = HashValue(hash, marking.dashLengthMeters);
    hash = HashValue(hash, marking.dashGapMeters);
    hash = HashValue(hash, marking.liftMeters);
    hash = HashValue(hash, marking.uvRepeatMeters);
    return HashValue(hash, marking.uvAlongU);
}

// 材質が無いときの色（リニア）。路面はアスファルト、路肩は少し明るい砂利の目安、区画線は白。
constexpr float kRoadFallbackColor[3] = {0.18f, 0.18f, 0.18f};
constexpr float kMarkingFallbackColor[3] = {0.80f, 0.80f, 0.78f};
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

renderer::GeneratedMeshItem::Cutout Application::MaterialCutout(compositor::MaterialAssetId material,
                                                                float uvRepeatMeters) const {
    renderer::GeneratedMeshItem::Cutout cutout;
    const compositor::MaterialAsset* asset = m_materialLibrary.Find(material);
    // Layered Material の層ごとの不透明度はまだ扱わない（通常の Material だけ）。
    if (asset == nullptr || asset->layerMaterial || asset->AlphaCutoff() <= 0.0f) return cutout;
    cutout.threshold = asset->AlphaCutoff();
    cutout.opacityIndex = m_textureLibrary.SrvIndex(asset->opacity.texture, false);
    cutout.channel = static_cast<uint32_t>(asset->opacity.channel);
    cutout.baseColorIndex = m_textureLibrary.SrvIndex(asset->baseColor, true);
    cutout.value = asset->opacityValue;
    cutout.uvScale = 1.0f / std::max(uvRepeatMeters, 0.01f);
    return cutout;
}

void Application::PrepareRoadMeshes() {
    std::vector<renderer::GeneratedMeshItem> items;
    std::vector<graph::GraphId> alive;
    std::vector<graph::GraphId> aliveFootprints;
    // 作らずに済ませた下流のノードは、前のフレームの状態をそのまま残す（選択を外して戻ったときに空にならないように）。
    const std::unordered_map<graph::GraphId, RoadNodeStatus> previousStatus = std::move(m_roadNodeStatus);
    m_roadNodeStatus.clear();
    for (const graph::CompiledRoadMesh& compiled : m_graph.CompileRoadMeshes()) {
        const graph::Node* meshNode = m_graph.FindNode(compiled.roadMesh);
        const graph::Node* pathNode = m_graph.FindNode(compiled.roadPath);
        const auto* mesh = meshNode ? std::get_if<graph::RoadMeshNodeSettings>(&meshNode->settings) : nullptr;
        const auto* path = pathNode ? std::get_if<graph::RoadPathNodeSettings>(&pathNode->settings) : nullptr;
        if (mesh == nullptr || path == nullptr) continue;
        alive.push_back(compiled.output);
        // 表示フラグで隠している鎖も形と状態は作り続け、描くかだけを切り替える。
        // Mask Mesh だけが読む鎖（drawn が偽）は形を作って足跡にするだけで、描かない。
        const bool visible = compiled.drawn && !OutputHidden(compiled.output);
        const bool reference = OutputReference(compiled.output);
        RoadChainCache& cache = m_roadMeshCache[compiled.output];

        // 選択したノードが鎖の途中なら、その下流は作らない（ファイル冒頭の説明）。
        // cutFrom は鎖の中で最初に作らないノードの添字（鎖の長さなら何も省かない）。
        const auto selectedAt = std::find(compiled.chain.begin(), compiled.chain.end(), m_selectedGraphNode);
        const size_t cutFrom = selectedAt == compiled.chain.end()
                                   ? compiled.chain.size()
                                   : static_cast<size_t>(selectedAt - compiled.chain.begin()) + 1;
        const bool cut = cutFrom < compiled.chain.size();
        const auto isBuilt = [&](graph::GraphId nodeId) {
            const auto at = std::find(compiled.chain.begin(), compiled.chain.end(), nodeId);
            return at == compiled.chain.end() || static_cast<size_t>(at - compiled.chain.begin()) < cutFrom;
        };
        const auto keepPrevious = [&](graph::GraphId nodeId) {
            if (const auto found = previousStatus.find(nodeId); found != previousStatus.end())
                m_roadNodeStatus[nodeId] = found->second;
        };
        if (cut) {
            // 足跡の読み手は生きている（捨てない）。中身は選択を外すまで前のまま。
            for (const graph::GraphId maskId : compiled.maskNodes) aliveFootprints.push_back(maskId);
            for (size_t i = cutFrom; i < compiled.chain.size(); ++i) keepPrevious(compiled.chain[i]);
        }
        if (!isBuilt(compiled.roadMesh)) continue;  // Road Path を選んでいる。カーブはビューポートの重ね描きが出す
        RoadNodeStatus& roadStatus = m_roadNodeStatus[compiled.roadMesh];
        roadStatus = {};

        // --- 路面 ---
        graph::RoadProfileCurve base;
        graph::RoadProfileCurve centerline;
        std::string error;
        bool pending = false;
        if (!BuildRoadCenterline(*pathNode, base, centerline, &error, &pending)) {
            roadStatus.error = error;
            // Base の地形がまだ無いだけなら形は捨てない（次のフレームで続きから）。
            if (!pending) cache = {};
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
            cache.markings.clear();
            continue;
        }
        renderer::GeneratedMeshItem::Track roadTrack;
        roadStatus.vertices = cache.road.mesh.vertices.size();
        roadStatus.triangles = cache.road.mesh.indices.size() / 3;
        {
            renderer::GeneratedMeshItem item;
            item.id = MeshItemId(compiled.output, compiled.roadMesh, 0);
            item.geometryKey = cache.road.key;
            item.geometry = &cache.road.mesh;
            std::copy(std::begin(kRoadFallbackColor), std::end(kRoadFallbackColor), item.track.fallbackColor);
            auto& slot = item.track.materials[0];
            slot.hasMaterial = SurfaceMaterialGpu(mesh->mesh.material, mesh->mesh.uvRepeatMeters, slot.gpu,
                                                  &roadStatus.materialError);
            if (slot.hasMaterial) SetRoadContext(slot.gpu, mesh->mesh);
            roadTrack = item.track;
            item.visible = visible;
            item.reference = reference;
            if (compiled.drawn) items.push_back(item);
        }

        // --- 区画線（路面の上。鎖のどこに挟んでも路面に引く） ---
        cache.markings.resize(compiled.markings.size());
        for (size_t m = 0; m < compiled.markings.size(); ++m) {
            const graph::GraphId markingId = compiled.markings[m];
            const graph::Node* markingNode = m_graph.FindNode(markingId);
            const auto* marking = markingNode ? std::get_if<graph::LaneMarkingNodeSettings>(&markingNode->settings) : nullptr;
            if (marking == nullptr || !isBuilt(markingId)) continue;  // 選択より下流は前の形を残して作らない
            RoadNodeStatus& status = m_roadNodeStatus[markingId];
            RoadMarkingCache& markingCache = cache.markings[m];
            const uint64_t markingKey = MarkingGeometryKey(cache.road.key, mesh->mesh, marking->marking);
            if (markingKey != markingCache.key) {
                markingCache.key = markingKey;
                markingCache.error.clear();
                if (!graph::BuildRoadMarkings(cache.road.mesh, cache.road.stride, mesh->mesh, marking->marking,
                                              markingCache.meshes, &markingCache.error))
                    for (auto& built : markingCache.meshes) built = {};
            }
            status.error = markingCache.error;
            status.lengthMeters = cache.lengthMeters;
            for (size_t kind = 0; kind < graph::kRoadMarkingKindCount; ++kind) {
                const renderer::MeshData& geometry = markingCache.meshes[kind];
                if (geometry.indices.empty()) continue;
                status.vertices += geometry.vertices.size();
                status.triangles += geometry.indices.size() / 3;
                const graph::RoadMarkingLine& line = marking->marking.lines[kind];
                renderer::GeneratedMeshItem item;
                item.id = MeshItemId(compiled.output, markingId, static_cast<int>(kind) + 16);
                item.geometryKey = markingCache.key ^ (0x9e3779b97f4a7c15ull * (kind + 1));
                item.geometry = &geometry;
                item.decal = true;
                std::copy(std::begin(kMarkingFallbackColor), std::end(kMarkingFallbackColor), item.track.fallbackColor);
                auto& slot = item.track.materials[0];
                slot.hasMaterial = SurfaceMaterialGpu(line.material, marking->marking.uvRepeatMeters, slot.gpu,
                                                      &status.materialError);
                item.cutout = MaterialCutout(line.material, marking->marking.uvRepeatMeters);
                item.visible = visible;
                item.reference = reference;
                if (compiled.drawn) items.push_back(item);
            }
        }

        // --- 路肩（内側から順に。左右それぞれ、今の外側の端から張り出す） ---
        // 端の帯。左は列 0 が端（隣は列 1）、右は最後の列が端。路肩の帯では最後の列が外側の端。
        struct Edge {
            const RoadStrip* strip = nullptr;
            uint32_t edgeColumn = 0, innerColumn = 0;
            // 内側の帯の材質の表と、外へ延ばした位置の座標（origin + sign * 外への距離）。
            renderer::GeneratedMeshItem::Track track;
            float origin = 0, sign = 1;
        };
        const float roadWidth = std::clamp(mesh->mesh.widthMeters, graph::kRoadMinWidthMeters, graph::kRoadMaxWidthMeters);
        Edge edges[2] = {{&cache.road, 0, 1, roadTrack, 0.0f, -1.0f},
                         {&cache.road, cache.road.stride - 1, cache.road.stride - 2, roadTrack, roadWidth, 1.0f}};
        // 描く項目（item.geometry）と次の路肩の張り出し元（edges）が帯を指すので、ループの途中で
        // shoulders を伸ばして付け替えさせない。路肩 1 つにつき帯は左右の 2 本まで。
        // 付け替わると解放済みのメッシュを読み、でたらめな大きさのバッファを頼んでデバイスが失われていた。
        cache.shoulders.reserve(compiled.shoulders.size() * 2);
        size_t stripIndex = 0;
        for (const graph::GraphId shoulderId : compiled.shoulders) {
            const graph::Node* shoulderNode = m_graph.FindNode(shoulderId);
            const auto* shoulder = shoulderNode ? std::get_if<graph::ShoulderNodeSettings>(&shoulderNode->settings) : nullptr;
            if (shoulder == nullptr) continue;
            // 選択より下流の路肩は作らない。鎖の順に並んでいるので、ここから先は全部下流。
            if (!isBuilt(shoulderId)) break;
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
                std::copy(std::begin(kShoulderFallbackColor), std::end(kShoulderFallbackColor), item.track.fallbackColor);
                // 帯の幅（断面に沿った長さ。UV の x の範囲）。
                const float stripWidth = graph::ShoulderSectionLength(settings);
                // 区間の表。材質（と繰り返し長）・境界マテリアルの種類ごとに枠を 1 つ使う。
                item.track.spans.clear();
                item.track.materials.clear();
                std::vector<std::pair<compositor::MaterialAssetId, float>> materialKeys;
                std::vector<std::string> boundaryKeys;
                for (const graph::RoadShoulderSpan& span : graph::ShoulderSpans(settings, cache.lengthMeters)) {
                    renderer::GeneratedMeshItem::Span out;
                    out.startMeters = span.startMeters;
                    out.transitionMeters = span.transitionMeters;
                    const std::pair<compositor::MaterialAssetId, float> materialKey{span.material, span.uvRepeatMeters};
                    const auto foundMaterial = std::find(materialKeys.begin(), materialKeys.end(), materialKey);
                    if (foundMaterial != materialKeys.end()) {
                        out.material = static_cast<uint32_t>(foundMaterial - materialKeys.begin());
                    } else if (materialKeys.size() < graph::kShoulderMaxSpanMaterials) {
                        out.material = static_cast<uint32_t>(materialKeys.size());
                        materialKeys.push_back(materialKey);
                        renderer::GeneratedMeshItem::Material slot;
                        slot.hasMaterial = SurfaceMaterialGpu(span.material, span.uvRepeatMeters, slot.gpu,
                                                              &status.materialError);
                        if (slot.hasMaterial) SetShoulderContext(slot.gpu, stripWidth);
                        item.track.materials.push_back(slot);
                    } else {
                        status.error = "区間の材質は 4 種類まで（繰り返し長の違いも別の種類に数える）";
                    }
                    if (!span.boundaryPath.empty() || !span.boundaryUid.empty()) {
                        const std::string boundaryKey = span.boundaryUid + "|" + span.boundaryPath;
                        const auto foundBoundary = std::find(boundaryKeys.begin(), boundaryKeys.end(), boundaryKey);
                        if (foundBoundary != boundaryKeys.end()) {
                            out.boundary = static_cast<uint32_t>(foundBoundary - boundaryKeys.begin());
                        } else if (boundaryKeys.size() >= graph::kShoulderMaxSpanBoundaries) {
                            status.error = "区間の境界マテリアルは 4 種類まで";
                        } else if (const BoundaryAsset* boundary = AcquireBoundary(span.boundaryPath, span.boundaryUid)) {
                            if (!boundary->error.empty()) {
                                status.materialError = boundary->error;
                            } else {
                                out.boundary = static_cast<uint32_t>(boundaryKeys.size());
                                boundaryKeys.push_back(boundaryKey);
                                renderer::GeneratedMeshItem::Boundary b;
                                b.maskIndex = m_textureLibrary.SrvIndex(boundary->mask, false);
                                b.heightIndex = m_textureLibrary.SrvIndex(boundary->height, false);
                                // 幅は路肩の幅に収める（反対側の端まで食い込ませない）。
                                b.widthMeters = std::min(boundary->widthMeters, stripWidth);
                                b.repeatMeters = boundary->repeatMeters;
                                b.depthMeters = boundary->depthMeters;
                                b.heightCenter = boundary->heightCenter;
                                b.alongU = boundary->alongU;
                                b.invertMask = boundary->invertMask;
                                item.boundaries.push_back(b);
                            }
                        }
                    }
                    item.track.spans.push_back(out);
                }
                if (item.track.materials.empty()) item.track.materials.emplace_back();
                // 内側の境界の中で見せる、内側の帯の材質（内側の帯が区間で替わるなら同じ道のりで替わる）。
                item.hasInner = true;
                item.innerTrack = edges[side].track;
                item.innerOrigin = edges[side].origin;
                item.innerSign = edges[side].sign;
                item.visible = visible;
                item.reference = reference;
                if (compiled.drawn) items.push_back(item);
                edges[side] = {&strip, strip.stride - 1, strip.stride - 2, item.track, stripWidth, 1.0f};
            }
        }
        // 下流を省いている間は、省いた路肩の帯を捨てない（選択を外したときにキーが合えばそのまま使う）。
        if (!cut) cache.shoulders.resize(stripIndex);

        // --- 足跡（Mask Mesh が読む） ---
        // 鎖の路面と路肩の三角形を、地形平面の正規化 UV と正規化ハイトへ写す。区画線は路面の上の
        // 帯で足跡は路面に含まれるので入れない。鎖のどのメッシュに繋いだ Mask Mesh も鎖全体を読む。
        // 下流を省いている間は足跡を更新しない（本体スタックの改版が毎フレーム起きないように。読み手は上で生かした）。
        if (!compiled.maskNodes.empty() && !cut) {
            compositor::MeshFootprint footprint;
            const float sizeMeters = std::max(m_renderer.PlaneSize(), 1e-3f);
            const float heightMeters = std::max(m_renderer.DisplacementScale(), 1e-3f);
            const auto append = [&](const renderer::MeshData& mesh) {
                const uint32_t baseVertex = static_cast<uint32_t>(footprint.vertices.size());
                footprint.vertices.reserve(footprint.vertices.size() + mesh.vertices.size());
                for (const renderer::MeshVertex& vertex : mesh.vertices) {
                    compositor::MeshFootprintVertex out;
                    out.u = vertex.position.x / sizeMeters + 0.5f;
                    out.v = vertex.position.z / sizeMeters + 0.5f;
                    out.height = vertex.position.y / heightMeters + 0.5f;
                    footprint.vertices.push_back(out);
                }
                footprint.indices.reserve(footprint.indices.size() + mesh.indices.size());
                for (const uint32_t index : mesh.indices) footprint.indices.push_back(baseVertex + index);
            };
            append(cache.road.mesh);
            for (const RoadStrip& strip : cache.shoulders) append(strip.mesh);
            for (const graph::GraphId maskId : compiled.maskNodes) {
                aliveFootprints.push_back(maskId);
                m_meshFootprints.Set(static_cast<uint32_t>(maskId), footprint);
            }
        }
    }
    for (auto it = m_roadMeshCache.begin(); it != m_roadMeshCache.end();) {
        if (std::find(alive.begin(), alive.end(), it->first) == alive.end()) it = m_roadMeshCache.erase(it);
        else ++it;
    }
    // 建物の敷地（Model Place の Pad）も同じ置き場に置く。
    UpdateModelPlacePads(aliveFootprints);
    // 読み手の無くなった足跡（Mask Mesh の削除・切断、鎖が作れなくなった）は捨てる。
    for (const uint32_t key : m_meshFootprints.Keys()) {
        if (std::find(aliveFootprints.begin(), aliveFootprints.end(), static_cast<graph::GraphId>(key)) ==
            aliveFootprints.end()) {
            m_meshFootprints.Remove(key);
        }
    }
    // 足跡が変わったら本体のスタックを改版する（評価器が op のハッシュで焼き直す）。
    // 配置の点の評価器は自分の footprintRevision で追う。
    if (m_meshFootprintRevisionSeen != m_meshFootprints.Revision()) {
        m_meshFootprintRevisionSeen = m_meshFootprints.Revision();
        m_graphStack.MarkDirty();
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

void Application::DrawBoundaryAssetRows(BoundaryAsset& boundary) {
    const BoundaryAsset defaults;
    bool edited = false;
    edited |= DrawTextureSlotRow("マスク", boundary.mask, m_textureLibrary, m_pendingAssetReveal);
    edited |= DrawTextureSlotRow("ハイト", boundary.height, m_textureLibrary, m_pendingAssetReveal);
    edited |= ui::PropertyFloat("境界の幅", &boundary.widthMeters, 0.01f, 20.0f, defaults.widthMeters,
                                "内側の端から、境目の模様を置く幅（m）。路肩の幅より広くはしない", "%.2f m");
    edited |= ui::PropertyFloat("繰り返し長", &boundary.repeatMeters, 0.01f, 100.0f, defaults.repeatMeters,
                                "道に沿って模様が 1 周する長さ（m）", "%.2f m", ImGuiSliderFlags_Logarithmic);
    edited |= ui::PropertyFloat("深さ", &boundary.depthMeters, 0.0f, 0.5f, defaults.depthMeters,
                                "ハイトの凹凸の深さ（m）。(ハイト - 基準) × 2 × 深さ。今は陰影だけに効く", "%.3f m");
    edited |= ui::PropertyFloat("基準の高さ", &boundary.heightCenter, 0.0f, 1.0f, defaults.heightCenter,
                                "ハイト画像の平らな所の値", "%.2f");
    static const char* const kAxes[] = {"V（道に沿う向き）", "U（横切る向き）"};
    int axis = boundary.alongU ? 1 : 0;
    if (ui::PropertyCombo("繰り返しの向き", &axis, kAxes, 2, 0, "画像のどちらの向きを道に沿って繰り返すか")) {
        boundary.alongU = axis == 1;
        edited = true;
    }
    edited |= ui::PropertyBool("マスクを反転", &boundary.invertMask, defaults.invertMask,
                               "マスクは白が内側の帯（路面）、黒が路肩。逆の画像のときに入れる");
    if (edited) boundary.dirty = true;
    ui::PropertyLabelEmpty("boundarySave");
    ImGui::BeginDisabled(!boundary.dirty);
    if (ui::Button("ファイルへ保存", ui::kWideButtonWidth)) SaveBoundary(boundary);
    ImGui::EndDisabled();
    ui::PropertyEnd();
}

void Application::OpenBoundaryPreview(const std::filesystem::path& file) {
    const nlohmann::json reference = m_workspace.Reference(file);
    m_boundaryPreviewPath = io::ProjectWorkspace::String(reference, "path");
    m_boundaryPreviewUid = io::ProjectWorkspace::String(reference, "uid");
    m_showBoundaryPreview = true;
}

// 境界マテリアルの窓。上にマスクとハイトの画像、下に設定（路肩のプロパティと同じ行）。
// 開くのはアセットブラウザのダブルクリックか、路肩のプロパティのボタン。
void Application::DrawBoundaryPreviewWindow() {
    if (!m_showBoundaryPreview || HiddenWithAssetBand("境界マテリアル")) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(460.0f), ui::Scaled(640.0f)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("境界マテリアル", &m_showBoundaryPreview,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    BoundaryAsset* boundary = (m_boundaryPreviewPath.empty() && m_boundaryPreviewUid.empty())
                                  ? nullptr
                                  : AcquireBoundary(m_boundaryPreviewPath, m_boundaryPreviewUid);
    if (boundary == nullptr) {
        ui::HintText("アセットブラウザで .tgboundary をダブルクリックすると開く");
        ImGui::End();
        return;
    }

    // --- 上: マスクとハイトを横に並べる（U = 横、V = 縦の画像のまま） ---
    const float paneSize = PreviewPaneSize() * 0.5f + ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("boundaryPreviewPane", ImVec2(0.0f, paneSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float imageSize = std::max(
            std::min((ImGui::GetContentRegionAvail().x - spacing) * 0.5f,
                     ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing()),
            ui::Scaled(32.0f));
        const auto image = [&](const char* label, compositor::TextureId id) {
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", label);
            const compositor::LibraryTexture* texture = m_textureLibrary.Find(id);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            if (texture == nullptr || texture->missing) {
                ui::MissingThumbnail(min, ImVec2(min.x + imageSize, min.y + imageSize));
                ImGui::Dummy(ImVec2(imageSize, imageSize));
            } else {
                // R を灰色で見せる（マスクとハイトは R を読む）。
                ImGui::Image(static_cast<ImTextureID>(texture->ChannelHandle(0).ptr), ImVec2(imageSize, imageSize));
            }
            ImGui::EndGroup();
        };
        image("マスク（R）", boundary->mask);
        ImGui::SameLine();
        image("ハイト（R）", boundary->height);
    }
    ImGui::EndChild();
    ImGui::Separator();

    // --- 下: 設定 ---
    ImGui::BeginChild("boundaryPropertyPane", ImVec2(0.0f, 0.0f));
    ui::SectionHeader("境界マテリアル");
    if (!boundary->error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s", boundary->error.c_str());
    } else if (ui::BeginPropertyTable("boundaryWindowRows", "繰り返しの向き")) {
        ui::PropertyValue("名前", "%s", boundary->name.c_str());
        DrawAssetPathRow("場所", boundary->path, m_pendingAssetReveal);
        DrawBoundaryAssetRows(*boundary);
        ui::EndPropertyTable();
    }
    ui::HintText(boundary->alongU
                     ? "画像の V が内側（路面）から外へ横切る向き、U が道に沿って繰り返す向き"
                     : "画像の U（左 → 右）が内側（路面）から外へ横切る向き、V が道に沿って繰り返す向き");
    ui::HintText("マスクの白が内側の帯（路面）、黒が路肩。設定はこのファイルを使うすべての路肩に効き、"
                 "「ファイルへ保存」でファイルへ書く");
    ImGui::EndChild();
    ImGui::End();
}

namespace {
// 断面図の高さ（96 DPI 基準）。
constexpr float kSectionPlotHeight = 110.0f;
}  // namespace

// 路肩の断面の点（断面図と、点ごとの外への距離・高さ）。
bool Application::DrawShoulderSection(graph::RoadShoulderSettings& shoulder) {
    bool changed = false;
    std::vector<graph::RoadSectionPoint>& section = shoulder.section;

    // 断面図。横が内側の端からの外への距離、縦が高さ（縦横同じ縮尺）。内側の帯は左に淡く描く。
    {
        const std::vector<graph::RoadSectionPoint> points = graph::ShoulderSectionPoints(shoulder);
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ui::Scaled(kSectionPlotHeight);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + width, min.y + height);
        ImGui::InvisibleButton("##shoulderSectionPlot", ImVec2(width, height));
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
        drawList->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
        float right = 0.5f, low = 0.0f, high = 0.0f;
        for (const graph::RoadSectionPoint& point : points) {
            right = std::max(right, point.acrossMeters);
            low = std::min(low, point.heightMeters);
            high = std::max(high, point.heightMeters);
        }
        const float left = -std::max(0.5f, right * 0.2f);  // 内側の帯を見せる分
        const float spanX = right - left;
        const float spanY = std::max(high - low, 0.1f);
        const float pad = ui::Scaled(10.0f);
        // 上下は文字の行の分も空ける（寸法の文字と線が重ならないように）。
        const float padY = pad + ImGui::GetTextLineHeight();
        const float scale = std::min((width - pad * 2.0f) / spanX, (height - padY * 2.0f) / spanY);
        const float centerY = (high + low) * 0.5f;
        const auto toScreen = [&](float across, float y) {
            return ImVec2(min.x + pad + (across - left) * scale, (min.y + max.y) * 0.5f - (y - centerY) * scale);
        };
        drawList->PushClipRect(min, max, true);
        drawList->AddLine(toScreen(left, 0.0f), toScreen(0.0f, 0.0f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                          ui::Scaled(2.0f));
        std::vector<ImVec2> line;
        for (const graph::RoadSectionPoint& point : points) line.push_back(toScreen(point.acrossMeters, point.heightMeters));
        drawList->AddPolyline(line.data(), static_cast<int>(line.size()), ImGui::GetColorU32(ImGuiCol_CheckMark), 0,
                              ui::Scaled(2.0f));
        for (size_t i = 1; i < line.size(); ++i)
            drawList->AddCircleFilled(line[i], ui::Scaled(3.0f), ImGui::GetColorU32(ImGuiCol_CheckMark));
        char text[64] = {};
        std::snprintf(text, sizeof(text), "%.2f m", right);
        drawList->AddText(ImVec2(max.x - pad - ImGui::CalcTextSize(text).x, max.y - pad * 0.5f - ImGui::GetTextLineHeight()),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        std::snprintf(text, sizeof(text), "高さ %+.2f 〜 %+.2f m", low, high);
        drawList->AddText(ImVec2(min.x + pad * 0.5f, min.y + pad * 0.3f), ImGui::GetColorU32(ImGuiCol_TextDisabled), text);
        drawList->PopClipRect();
    }
    std::string error;
    if (!graph::ValidateShoulderSection(section, &error))
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s", error.c_str());

    // 点ごとの行。内側の端 (0, 0) は固定なので出さない。
    size_t removeIndex = section.size();
    for (size_t i = 0; i < section.size(); ++i) {
        graph::RoadSectionPoint& point = section[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextDisabled("断面の点 %zu", i + 1);
        if (ui::BeginPropertyTable("shoulderSectionPoint")) {
            changed |= ui::PropertyFloat("外への距離", &point.acrossMeters, 0.0f, graph::kShoulderMaxWidthMeters,
                                         i > 0 ? section[i - 1].acrossMeters : 0.0f,
                                         "内側の端から外への水平の距離（m）。前の点より小さくはしない。同じなら縦の面",
                                         "%.3f m");
            changed |= ui::PropertyFloat("高さ", &point.heightMeters, -graph::kShoulderMaxSectionHeight,
                                         graph::kShoulderMaxSectionHeight, i > 0 ? section[i - 1].heightMeters : 0.0f,
                                         "内側の端からの高さ（m）。正なら上", "%+.3f m");
            if (section.size() > 1) {
                ui::PropertyLabelEmpty("shoulderSectionPointDelete");
                if (ui::Button("削除")) removeIndex = i;
                ui::PropertyEnd();
            }
            ui::EndPropertyTable();
        }
        ImGui::PopID();
    }
    if (removeIndex < section.size()) {
        section.erase(section.begin() + static_cast<std::ptrdiff_t>(removeIndex));
        changed = true;
    }
    ImGui::BeginDisabled(section.size() >= graph::kShoulderMaxSectionPoints);
    if (ui::Button("断面の点を追加", ui::kWideButtonWidth)) {
        const graph::RoadSectionPoint last = section.empty() ? graph::RoadSectionPoint{} : section.back();
        section.push_back({std::min(last.acrossMeters + 1.0f, graph::kShoulderMaxWidthMeters), last.heightMeters});
        changed = true;
    }
    ImGui::EndDisabled();
    ui::HintText("内側の端 (0, 0) から外へ向かう順に並べる。向きが大きく変わる点は稜線を立て、"
                 "区間は約 1 m ごとに割る。材質は断面に沿った長さで貼る");
    return changed;
}

bool Application::DrawLaneMarkingSettings(graph::Node& node) {
    auto* settings = std::get_if<graph::LaneMarkingNodeSettings>(&node.settings);
    if (settings == nullptr) return false;
    graph::RoadMarkingSettings& marking = settings->marking;
    const graph::RoadMarkingSettings defaults;
    bool changed = false;

    // 線の種類ごと（中央線・外側線・車線境界線）。
    static const char* const kTitles[] = {"中央線", "外側線", "車線境界線"};
    static const char* const kHints[] = {
        "進行方向と対向の車線の境に引く。対向の車線が無い道路（Road Mesh の対向の車線数が 0）では出ない",
        "左右の端から「外側線の位置」だけ内側に引く",
        "同じ向きの車線どうしの境に引く（片側 2 車線以上のとき）",
    };
    for (size_t kind = 0; kind < graph::kRoadMarkingKindCount; ++kind) {
        graph::RoadMarkingLine& line = marking.lines[kind];
        const graph::RoadMarkingLine& lineDefaults = defaults.lines[kind];
        ImGui::PushID(static_cast<int>(kind));
        ui::SectionHeader(kTitles[kind]);
        if (ui::BeginPropertyTable("laneMarkingLineRows")) {
            changed |= ui::PropertyBool("引く", &line.enabled, lineDefaults.enabled, kHints[kind]);
            if (line.enabled) {
                changed |= ui::PropertyBool("破線", &line.dashed, lineDefaults.dashed,
                                            "破線にする。線と間隔の長さは下の「破線」で決める");
                changed |= ui::PropertyFloat("幅", &line.widthMeters, 0.05f, 1.0f, lineDefaults.widthMeters,
                                             "線の幅（m）", "%.2f m");
                changed |= DrawMaterialSlotRow("材質", line.material, m_materialLibrary, m_pendingAssetReveal, true, true);
            }
            ui::EndPropertyTable();
        }
        ImGui::PopID();
    }

    ui::SectionHeader("共通");
    if (ui::BeginPropertyTable("laneMarkingCommonRows", "長さの向きを U に")) {
        changed |= ui::PropertyFloat("外側線の位置", &marking.edgeInsetMeters, 0.0f, 5.0f, defaults.edgeInsetMeters,
                                     "外側線の中心の、路面の端からの距離（m）", "%.2f m");
        changed |= ui::PropertyFloat("破線の線", &marking.dashLengthMeters, 0.1f, 50.0f, defaults.dashLengthMeters,
                                     "破線の 1 本の長さ（m）", "%.1f m");
        changed |= ui::PropertyFloat("破線の間隔", &marking.dashGapMeters, 0.0f, 50.0f, defaults.dashGapMeters,
                                     "破線の線と線の間（m）", "%.1f m");
        changed |= ui::PropertyFloat("浮かせる量", &marking.liftMeters, 0.0f, 0.1f, defaults.liftMeters,
                                     "路面から浮かせる高さ（m）。描画でも手前へずらすので、ごく小さくてよい", "%.3f m");
        changed |= ui::PropertyFloat("繰り返し長", &marking.uvRepeatMeters, 0.1f, 100.0f, defaults.uvRepeatMeters,
                                     "線の長さの向きに模様が 1 周する長さ（m）。横は線の幅いっぱいで 1 周", "%.2f m",
                                     ImGuiSliderFlags_Logarithmic);
        changed |= ui::PropertyBool("長さの向きを U に", &marking.uvAlongU, defaults.uvAlongU,
                                    "画像の U（横）を線の長さの向きにする。横長の白線の画像（2048×256 など）向け");
        ui::EndPropertyTable();
    }
    ui::HintText("材質が無ければ白で塗る。不透明度（切り抜き・半透明）の Material なら、かすれた所を抜く"
                 "（半透明もいまは切り抜きで描く）。車線の並びは Road Mesh の車線数と走行側で決まる");

    DrawRoadNodeStatus(node.id, "Road Mesh（か、その先の Shoulder）を繋ぎ、Mesh Output へ繋ぐと描く");
    return changed;
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
        static const char* const kShapes[] = {"勾配", "断面の点"};
        int shape = static_cast<int>(shoulder.shape);
        if (ui::PropertyCombo("形", &shape, kShapes, 2, 0,
                              "勾配: 幅・横断勾配・段差で決める。断面の点: 縁石や側溝のような横断の形を点で決める")) {
            // 断面の点が無ければ、今の勾配の形から作る（切り替えても形が変わらない）。
            if (shape == 1 && shoulder.section.empty()) {
                const std::vector<graph::RoadSectionPoint> points = graph::ShoulderSectionPoints(shoulder);
                shoulder.section.assign(points.begin() + 1, points.end());
            }
            shoulder.shape = static_cast<graph::RoadShoulderShape>(shape);
            changed = true;
        }
        if (shoulder.shape == graph::RoadShoulderShape::Slope) {
            changed |= ui::PropertyFloat("幅", &shoulder.widthMeters, graph::kShoulderMinWidthMeters,
                                         graph::kShoulderMaxWidthMeters, defaults.widthMeters,
                                         "路肩の幅（m）。幅方向は約 1 m ごとに割る", "%.2f m");
            changed |= ui::PropertyFloat("横断勾配", &shoulder.crossSlopePercent, -50.0f, 50.0f,
                                         defaults.crossSlopePercent, "外側へ向かって下がる勾配（%）。負なら上がる",
                                         "%.1f %%");
            changed |= ui::PropertyFloat("段差", &shoulder.stepHeightMeters, 0.0f, 0.5f, defaults.stepHeightMeters,
                                         "内側の端で下げる高さ（m）。舗装の端や縁石の段。0 で段差なし", "%.3f m");
            if (shoulder.stepHeightMeters > 0.0f) {
                changed |= ui::PropertyFloat("段差の幅", &shoulder.stepWidthMeters, 0.005f, 1.0f,
                                             defaults.stepWidthMeters, "段差を下りきるまでの水平の幅（m）", "%.3f m");
            }
        } else {
            static const char* const kTemplates[] = {"選ぶ…", "歩道（縁石）", "側溝（U 字）", "土の路肩"};
            int chosen = 0;
            if (ui::PropertyCombo("ひな形", &chosen, kTemplates, 4, 0,
                                  "断面の点をひな形で置き換える。歩道: 縁石 15 cm と幅 2 m の歩道。"
                                  "側溝: 幅 30 cm・深さ 30 cm の U 字溝と土の路肩。土の路肩: 平らな 50 cm と外の下り") &&
                chosen > 0) {
                shoulder.section = graph::ShoulderSectionTemplate(static_cast<graph::RoadSectionTemplate>(chosen - 1));
                changed = true;
            }
        }
        ui::EndPropertyTable();
    }
    if (shoulder.shape == graph::RoadShoulderShape::Section) changed |= DrawShoulderSection(shoulder);

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
    ui::HintText("路肩の座標（内側の端から断面に沿った長さ、始点からの道のり）で貼る。なしなら砂利色の灰色で塗る");

    // --- 内側の境界（境界マテリアル） ---
    ui::SectionHeader("内側の境界");
    // 境界マテリアルの中身（マスク・ハイト・幅など）は .tgboundary のもの。ここでは選ぶだけで、
    // 中身は読むだけの要約で見せる（編集は境界マテリアルの窓で。同じファイルを使うすべての路肩に効くため）。
    if (ui::BeginPropertyTable("shoulderBoundaryRows", "境界マテリアル")) {
        changed |= DrawBoundaryCombo("境界マテリアル", shoulder.boundaryPath, shoulder.boundaryUid);
        DrawBoundarySummary("shoulderBoundarySummary", shoulder.boundaryPath, shoulder.boundaryUid);
        ui::EndPropertyTable();
    }

    // --- 区間（道のりで材質・境界を切り替える） ---
    // 上の材質・内側の境界は最初の区間。切替位置から先を別の材質・境界にし、切替位置を中心に
    // 移行距離の幅でなめらかにつなぐ（SurfaceLayout の区間の移植）。
    ui::SectionHeader("区間");
    const auto statusFound = m_roadNodeStatus.find(node.id);
    const float roadLength = statusFound != m_roadNodeStatus.end() && statusFound->second.lengthMeters > 0.0f
                                 ? statusFound->second.lengthMeters : 1000.0f;
    // 一覧は道のりの順に並べる（ドラッグ中は並べ替えない。掴んでいる行が入れ替わらないように）。
    // 並びが崩れているときだけ触る。毎フレーム並べ替えると、変えていないのに設定を書き換えることになる。
    const auto byDistance = [](const graph::RoadShoulderSwitch& a, const graph::RoadShoulderSwitch& b) { return a.atMeters < b.atMeters; };
    if (!ImGui::IsAnyItemActive() && !std::is_sorted(shoulder.switches.begin(), shoulder.switches.end(), byDistance))
        std::stable_sort(shoulder.switches.begin(), shoulder.switches.end(), byDistance);
    size_t removeIndex = shoulder.switches.size();
    for (size_t i = 0; i < shoulder.switches.size(); ++i) {
        graph::RoadShoulderSwitch& change = shoulder.switches[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextDisabled("区間 %zu（%.1f m から）", i + 2, change.atMeters);
        if (ui::BeginPropertyTable("shoulderSwitch", "境界マテリアル")) {
            changed |= ui::PropertyFloat("切替位置", &change.atMeters, 0.0f, roadLength, std::min(10.0f, roadLength),
                                         "この区間が始まる道のり（m）。始点から測る", "%.1f m");
            changed |= ui::PropertyFloat("移行距離", &change.transitionMeters, 0.0f, graph::kShoulderMaxTransitionMeters,
                                         2.0f,
                                         "切替位置を中心に、前の区間からなめらかに移る幅（m）。0 でその位置で替わる。"
                                         "前後の区間の長さより長くはならない",
                                         "%.1f m");
            changed |= DrawMaterialSlotRow("材質", change.material, m_materialLibrary, m_pendingAssetReveal, true, true);
            const compositor::MaterialAsset* asset = m_materialLibrary.Find(change.material);
            if (asset == nullptr || !asset->layerMaterial) {
                changed |= ui::PropertyFloat("繰り返し長", &change.uvRepeatMeters, 0.1f, 100.0f, 2.0f,
                                             "模様が 1 周する長さ（m）", "%.2f m", ImGuiSliderFlags_Logarithmic);
            }
            changed |= DrawBoundaryCombo("境界マテリアル", change.boundaryPath, change.boundaryUid);
            DrawBoundarySummary("shoulderSwitchBoundary", change.boundaryPath, change.boundaryUid);
            ui::PropertyLabelEmpty("shoulderSwitchDelete");
            if (ui::Button("削除")) removeIndex = i;
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
        ImGui::PopID();
    }
    if (removeIndex < shoulder.switches.size()) {
        shoulder.switches.erase(shoulder.switches.begin() + static_cast<ptrdiff_t>(removeIndex));
        changed = true;
    }
    ImGui::BeginDisabled(shoulder.switches.size() >= graph::kShoulderMaxSwitches);
    if (ui::Button("区間を追加", ui::kWideButtonWidth)) {
        // 一番長い区間の真ん中で切り、その区間の材質・境界を引き継ぐ（見た目を変えずに足す）。
        const std::vector<graph::RoadShoulderSpan> spans = graph::ShoulderSpans(shoulder, roadLength);
        size_t longest = 0;
        float longestLength = -1.0f;
        for (size_t i = 0; i < spans.size(); ++i) {
            const float end = i + 1 < spans.size() ? spans[i + 1].startMeters : roadLength;
            if (end - spans[i].startMeters > longestLength) longestLength = end - spans[i].startMeters, longest = i;
        }
        graph::RoadShoulderSwitch added;
        added.atMeters = spans[longest].startMeters + std::max(longestLength, 0.0f) * 0.5f;
        added.transitionMeters = std::min(2.0f, std::max(longestLength, 0.0f) * 0.5f);
        added.material = spans[longest].material;
        added.uvRepeatMeters = spans[longest].uvRepeatMeters;
        added.boundaryPath = spans[longest].boundaryPath;
        added.boundaryUid = spans[longest].boundaryUid;
        shoulder.switches.push_back(std::move(added));
        changed = true;
    }
    ImGui::EndDisabled();
    ui::HintText("区間 1 は上の材質と内側の境界。区間を足すと、切替位置から先を別の材質・境界にする。"
                 "材質と境界マテリアルは 1 本の路肩でそれぞれ 4 種類まで");

    DrawRoadNodeStatus(node.id, "Road Mesh（か内側の Shoulder）を繋ぎ、Mesh Output へ繋ぐと描く");
    return changed;
}

// 選んだ境界マテリアルの要約の行（マスクのサムネイル・幅・繰り返し長と、窓で開くボタン）。
// 読むだけ。選んでいなければ何も出さず、読めなければ理由を出す。プロパティ表の中で呼ぶ。
void Application::DrawBoundarySummary(const char* id, const std::string& path, const std::string& uid) {
    if (path.empty() && uid.empty()) return;
    const BoundaryAsset* boundary = AcquireBoundary(path, uid);
    if (boundary == nullptr) return;
    ui::PropertyLabelEmpty(id);
    if (!boundary->error.empty()) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::WarnColor()), "%s", boundary->error.c_str());
        ui::PropertyEnd();
        return;
    }
    // アセットブラウザと同じサムネイル（路肩の端を真上から見た絵。左が路面、右が路肩）。
    const float size = ui::Scaled(40.0f);
    ui::ThumbnailImage(AssetThumbnailHandle(boundary->path), size);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("境目を真上から見た目安（左が路面、右が路肩。色は固定の目安）");
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextDisabled("幅 %.2f m・繰り返し %.2f m%s", boundary->widthMeters, boundary->repeatMeters,
                        boundary->dirty ? "（未保存の変更あり）" : "");
    ImGui::PushID(id);
    if (ui::Button("窓で開く")) OpenBoundaryPreview(boundary->path);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("境界マテリアルの窓で開く。マスク・ハイト・幅などはそこで確かめて編集する"
                          "（このファイルを使うすべての路肩に効く）");
    ImGui::PopID();
    ImGui::EndGroup();
    ui::PropertyEnd();
}

// 境界マテリアル（.tgboundary）を選ぶ行。プロパティ表の中で呼ぶ。
bool Application::DrawBoundaryCombo(const char* label, std::string& path, std::string& uid) {
    bool changed = false;
    // 開いていない間は、いま割り当ててあるものを引くのに ID の表を使う。一覧は開いた瞬間にディスクから探す。
    const std::filesystem::path current =
        (path.empty() && uid.empty()) ? std::filesystem::path{}
                                      : m_workspace.Resolve(nlohmann::json{{"path", path}, {"uid", uid}});
    const auto display = [](const std::filesystem::path& p) {
        const auto text = p.stem().u8string();
        return std::string(reinterpret_cast<const char*>(text.c_str()));
    };
    const std::string preview = current.empty() ? (path.empty() ? "なし" : "（見つからない）") : display(current);
    ui::PropertyLabel(label, "内側の帯（路面か内側の路肩）との境目の形。ルート内の .tgboundary から選ぶ");
    ImGui::SetNextItemWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##boundary", preview.c_str())) {
        ui::ComboFilterInput();
        if (!ui::ComboFilterActive() && ImGui::Selectable("なし", current.empty())) {
            path.clear();
            uid.clear();
            changed = true;
        }
        for (const auto& file : DropdownFiles(L".tgboundary")) {
            const std::string name = display(file);  // 1 行につき 1 度だけ作る
            if (!ui::ComboFilterPass(name)) continue;
            if (ImGui::Selectable(name.c_str(), file == current)) {
                const nlohmann::json reference = m_workspace.Reference(file);
                path = io::ProjectWorkspace::String(reference, "path");
                uid = io::ProjectWorkspace::String(reference, "uid");
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ui::PropertyEnd();
    return changed;
}

}  // namespace tg
