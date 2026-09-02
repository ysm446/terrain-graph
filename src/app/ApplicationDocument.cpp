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
    snapshot.graphNodes = m_graph.Nodes();
    snapshot.graphLinks = m_graph.Links();
    snapshot.selectedGraphNode = m_selectedGraphNode;
    snapshot.selectedMaterial = m_selectedMaterial;

    snapshot.materials.reserve(m_materialLibrary.Entries().size());
    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        MaterialSnapshot material;
        material.id = asset.id;
        material.name = asset.name;
        material.baseColor = asset.baseColor;
        material.normal = asset.normal;
        material.roughness = asset.roughness;
        material.metallic = asset.metallic;
        material.ambientOcclusion = asset.ambientOcclusion;
        material.height = asset.height;
        material.baseColorTint = asset.baseColorTint;
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
    const auto materialCount = static_cast<int>(m_materialLibrary.Entries().size());
    m_selectedMaterial = std::clamp(snapshot.selectedMaterial, 0, std::max(0, materialCount - 1));
}

void Application::MarkDocumentChanged() {
    m_documentDirty = true;
    // マテリアルの編集はグラフの改版に映らないので、スタック側を直接叩いて
    // 再評価させる（グラフ自体の編集は Revision の変化で再コンパイルされる）。
    m_graphStack.MarkDirty();
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
