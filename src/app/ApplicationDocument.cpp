// アンドゥ用の文書スナップショット。写し取り / 書き戻し / 変更の記録と、
// 参照されなくなったペイントマスクの回収。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

compositor::TextureId Application::ValidTexture(compositor::TextureId id) const {
    return (m_textureLibrary.Find(id) != nullptr) ? id : compositor::kNoTexture;
}

DocumentSnapshot Application::CaptureDocument() const {
    DocumentSnapshot snapshot;
    auto& atmosphere = snapshot.atmosphere;
    atmosphere.valid = true;
    atmosphere.uid = io::ProjectWorkspace::String(m_sceneAtmosphere, "uid");
    atmosphere.path = io::ProjectWorkspace::String(m_sceneAtmosphere, "path");
    const auto& sun = m_renderer.AtmosphericLight();
    const auto& sky = m_renderer.AtmosphericSettings();
    atmosphere.azimuth = sun.azimuth; atmosphere.elevation = sun.elevation; atmosphere.illuminance = sun.illuminance;
    atmosphere.density = sky.density; atmosphere.mie = sky.mie; atmosphere.eccentricity = sky.eccentricity;
    atmosphere.altitude = sky.altitude; atmosphere.groundAlbedo = sky.groundAlbedo; atmosphere.lowerHemisphere = sky.lowerHemisphere;
    atmosphere.nightEnabled = sky.nightEnabled;
    atmosphere.moonAzimuth = sky.moonAzimuth;
    atmosphere.moonElevation = sky.moonElevation;
    atmosphere.moonIlluminance = sky.moonIlluminance;
    atmosphere.moonPhase = sky.moonPhase;
    atmosphere.starIntensity = sky.starIntensity;
    atmosphere.starRotation = sky.starRotation;
    atmosphere.starLatitude = sky.starLatitude;
    atmosphere.celestial = m_renderer.Celestial();
    atmosphere.skylightIntensity = m_renderer.AtmosphericEnvironmentIntensity();

    snapshot.models = m_models;
    snapshot.graphNodes = m_graph.Nodes();
    snapshot.graphLinks = m_graph.Links();
    snapshot.selectedGraphNode = m_selectedGraphNode;
    snapshot.selectedMaterial = m_selectedMaterial;

    snapshot.materials.reserve(m_materialLibrary.Entries().size());
    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        MaterialSnapshot material;
        material.id = asset.id;
        material.name = asset.name;
        material.assetPath = asset.assetPath;
        material.assetUid = asset.assetUid;
        material.baseColor = asset.baseColor;
        material.normal = asset.normal;
        material.roughness = asset.roughness;
        material.metallic = asset.metallic;
        material.ambientOcclusion = asset.ambientOcclusion;
        material.height = asset.height;
        material.baseColorTint = asset.baseColorTint;
        material.hueShiftDegrees = asset.hueShiftDegrees;
        material.saturation = asset.saturation;
        material.brightness = asset.brightness;
        material.flipNormalGreen = asset.flipNormalGreen;
        material.roughnessValue = asset.roughnessValue;
        material.metallicValue = asset.metallicValue;
        material.ambientOcclusionValue = asset.ambientOcclusionValue;
        snapshot.materials.push_back(std::move(material));
    }
    return snapshot;
}

// 写し取った文書を書き戻す。
//
// **参照している ID は、いま実在するものだけ残す。** テクスチャとペイントマスクは
// 履歴の対象外なので、写し取った後に消えていることがある。
// 宙に浮いた ID を残すと、次に同じ番号が払い出されたとき別の画像が現れる。
void Application::ApplyDocument(const DocumentSnapshot& snapshot) {
    if (snapshot.atmosphere.valid) {
        const auto& source = snapshot.atmosphere;
        m_sceneAtmosphere = source.uid.empty() ? nlohmann::json() : nlohmann::json{{"uid", source.uid}, {"path", source.path}};
        auto& sun = m_renderer.AtmosphericLight();
        auto& sky = m_renderer.AtmosphericSettings();
        sun.azimuth = source.azimuth; sun.elevation = source.elevation; sun.illuminance = source.illuminance;
        sky.density = source.density; sky.mie = source.mie; sky.eccentricity = source.eccentricity;
        sky.altitude = source.altitude; sky.groundAlbedo = source.groundAlbedo; sky.lowerHemisphere = source.lowerHemisphere;
        sky.nightEnabled = source.nightEnabled;
        sky.moonAzimuth = source.moonAzimuth;
        sky.moonElevation = source.moonElevation;
        sky.moonIlluminance = source.moonIlluminance;
        sky.moonPhase = source.moonPhase;
        sky.starIntensity = source.starIntensity;
        sky.starRotation = source.starRotation;
        sky.starLatitude = source.starLatitude;
        m_renderer.Celestial() = source.celestial;
        m_renderer.AtmosphericEnvironmentIntensity() = source.skylightIntensity;
    }

    m_materialEditPending = false;
    m_materialEditAppearanceChanged = false;
    auto models = snapshot.models;
    for (auto& model : models) if (model.assetUid.empty()) {
        const auto current = std::find_if(m_models.begin(), m_models.end(), [&](const auto& a) { return a.id == model.id; });
        if (current != m_models.end()) { model.assetUid = current->assetUid; model.assetPath = current->assetPath; }
    }
    m_models = std::move(models);
    m_renderedModelThumbnails.clear();
    // --- マテリアル ---------------------------------------------------------
    // 写し取った時点に無かったものを消す。破棄は GPU 待機を伴う。
    std::vector<compositor::MaterialAssetId> removed;
    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        const bool kept = std::any_of(
            snapshot.materials.begin(), snapshot.materials.end(),
            [&asset](const MaterialSnapshot& m) { return m.id == asset.id; });
        if (!kept) {
            removed.push_back(asset.id);
        }
    }
    for (const compositor::MaterialAssetId id : removed) {
        m_materialLibrary.Remove(m_device, id);
    }

    for (const MaterialSnapshot& material : snapshot.materials) {
        // 消えていれば ID を保ったまま作り直す。残っていれば中身を上書きする。
        compositor::MaterialAsset& asset =
            m_materialLibrary.RestoreAsset(material.id, material.name);
        asset.name = material.name;
        // 初回保存で付いた永続IDは、保存前に作った編集履歴へ戻っても保持する。
        if (!material.assetUid.empty() || asset.assetUid.empty()) {
            asset.assetPath = material.assetPath;
            asset.assetUid = material.assetUid;
        }
        asset.baseColor = ValidTexture(material.baseColor);
        asset.normal = ValidTexture(material.normal);
        asset.roughness = material.roughness;
        asset.metallic = material.metallic;
        asset.ambientOcclusion = material.ambientOcclusion;
        asset.height = material.height;
        asset.roughness.texture = ValidTexture(asset.roughness.texture);
        asset.metallic.texture = ValidTexture(asset.metallic.texture);
        asset.ambientOcclusion.texture = ValidTexture(asset.ambientOcclusion.texture);
        asset.height.texture = ValidTexture(asset.height.texture);
        asset.baseColorTint = material.baseColorTint;
        asset.hueShiftDegrees = material.hueShiftDegrees;
        asset.saturation = material.saturation;
        asset.brightness = material.brightness;
        asset.flipNormalGreen = material.flipNormalGreen;
        asset.roughnessValue = material.roughnessValue;
        asset.metallicValue = material.metallicValue;
        asset.ambientOcclusionValue = material.ambientOcclusionValue;
        asset.thumbnailDirty = true;
    }

    // --- グラフ -------------------------------------------------------------
    std::vector<graph::Node> nodes = snapshot.graphNodes;
    for (graph::Node& node : nodes) {
        auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings);
        if (settings == nullptr) {
            continue;
        }
        compositor::MaterialLayer& layer = settings->layer;
        if (m_materialLibrary.Find(layer.material) == nullptr) {
            layer.material = compositor::kNoMaterialAsset;
        }
        layer.mask.texture.texture = ValidTexture(layer.mask.texture.texture);
        layer.heightTexture.texture = ValidTexture(layer.heightTexture.texture);
        layer.pathUv.mask.texture = ValidTexture(layer.pathUv.mask.texture);
        if (m_paintMasks.Find(layer.mask.paint) == nullptr) {
            layer.mask.paint = compositor::kNoPaintMask;
        }
    }
    m_graph.Replace(std::move(nodes), snapshot.graphLinks);
    // ノードの位置も一緒に戻すので、エディタへ流し込み直す。視点は動かさない。
    RequestGraphNodePlacement(false);

    m_selectedGraphNode =
        (m_graph.FindNode(snapshot.selectedGraphNode) != nullptr) ? snapshot.selectedGraphNode
                                                                  : 0;
    if (const auto* selected = m_graph.FindNode(m_selectedGraphNode);
        selected && m_sceneComponents.is_array() && selected->component != m_editComponent) {
        const auto id = m_selectedGraphNode;
        OpenComponentEditor(selected->component);
        m_selectedGraphNode = id;
    }
    const auto materialCount = static_cast<int>(m_materialLibrary.Entries().size());
    m_selectedMaterial = std::clamp(snapshot.selectedMaterial, 0, std::max(0, materialCount - 1));
}

void Application::MarkDocumentChanged(bool terrainChanged) {
    m_documentDirty = true;
    m_renderedModelThumbnails.clear();
    // マテリアルの編集はグラフの改版に映らないので、スタック側を直接叩いて
    // 再評価させる（グラフ自体の編集は Revision の変化で再コンパイルされる）。
    if (terrainChanged) m_graphStack.MarkDirty();
}

// 文書からも履歴からも参照されなくなったペイントマスクを破棄する。
//
// レイヤーを消したときにすぐ捨ててしまうと、アンドゥで戻したときに
// 描いた内容が失われる。参照が完全に無くなるまで持っておき、ここで回収する。
void Application::SweepPaintMasks() {
    if (m_paintMasks.Count() == 0) {
        return;
    }

    std::vector<compositor::PaintMaskId> referenced;
    const auto collectNodes = [&referenced](const std::vector<graph::Node>& nodes) {
        for (const graph::Node& node : nodes) {
            const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings);
            if (settings != nullptr &&
                settings->layer.mask.paint != compositor::kNoPaintMask) {
                referenced.push_back(settings->layer.mask.paint);
            }
        }
    };

    collectNodes(m_graph.Nodes());
    if (m_componentPreview >= 0) collectNodes(m_previewOriginalGraph.Nodes());
    collectNodes(m_committed.graphNodes);
    for (const DocumentSnapshot& snapshot : m_undoHistory.UndoStack()) {
        collectNodes(snapshot.graphNodes);
    }
    for (const DocumentSnapshot& snapshot : m_undoHistory.RedoStack()) {
        collectNodes(snapshot.graphNodes);
    }

    for (const compositor::PaintMaskId id : m_paintMasks.Ids()) {
        if (std::find(referenced.begin(), referenced.end(), id) == referenced.end()) {
            m_paintMasks.Remove(m_device, id);
        }
    }
}

}  // namespace tg
