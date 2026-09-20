#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"
#include "graph/SurfacePresetGraph.h"
#include <algorithm>
#include <cstdio>
namespace tg {
bool Application::DrawLayerMaterialProperties(compositor::MaterialAsset& asset) {
    auto& material = *asset.layerMaterial;
    material.id = asset.id; material.name = asset.name;
    bool changed = false;
    const graph::LayerMaterial defaultMaterial;
    const graph::PresetMaterial defaultLayer;
    const graph::RoadMaskNodeSettings defaultMask;
    ui::SectionHeader("レイヤーマテリアル");
    if (ui::BeginPropertyTable("layerAssetBasics")) {
        char name[128]{}; std::snprintf(name, sizeof(name), "%s", asset.name.c_str());
        if (ui::PropertyTextInput("名前", name, sizeof(name), "共有材質の名前")) {
            if (asset.assetPath.empty()) { asset.name = name; material.name = name; changed = true; }
            else RequestAssetRename(asset.assetPath, name);
        }
        changed |= ui::PropertyFloat("合成幅", &material.layerBlendRange, 0, 1, defaultMaterial.layerBlendRange, "材質内部のハイトによる境界の混ざり幅");
        changed |= ui::PropertyFloat("変位量", &material.displacementMeters, 0, 10, defaultMaterial.displacementMeters, "ハイト0〜1の全幅に対応する起伏。中間グレーが変位ゼロ", "%.3f m");
        ui::EndPropertyTable();
    }
    if (m_presetEditorId != asset.id) {
        m_presetEditorId = asset.id; m_selectedPresetLayer = 0; m_surfacePresetError.clear();
    }
    std::vector<graph::PresetMaterial> layers;
    if (!graph::ExtractPresetLayers(material, layers, m_surfacePresetError)) {
        ui::HintText(m_surfacePresetError.c_str()); return changed;
    }
    if (material.materialGraph) {
        ui::HintText("ノード形式の素材です。変換すると出力につながる層を取り込みます。未使用ノードは除かれます（Undo可能）");
        if (ui::Button("レイヤー形式に変換", ui::kWideButtonWidth)) {
            material.materials = layers; material.materialGraph.reset(); changed = true;
        }
    }
    const bool readOnly = material.materialGraph.has_value();
    bool layersChanged = false;
    m_selectedPresetLayer = std::clamp(m_selectedPresetLayer, 0, static_cast<int>(layers.size()) - 1);
    const auto makeMask = [](graph::PresetMaterial& layer) {
        layer.mask.emplace(); layer.mask->shape = graph::RoadMaskShape::Constant; layer.mask->breakupAmount = 0;
    };
    ui::SectionHeader("レイヤー");
    ui::HintText("上の行ほど上層です。ドラッグで順序を変更できます（最大4層）");
    ImGui::BeginDisabled(readOnly);
    ImGui::BeginDisabled(layers.size() >= 4);
    if (ui::Button("追加", 70)) {
        graph::PresetMaterial layer; makeMask(layer); layers.push_back(layer);
        m_selectedPresetLayer = static_cast<int>(layers.size()) - 1; layersChanged = true;
    }
    ImGui::SameLine();
    if (ui::Button("複製", 70)) {
        auto layer = layers[m_selectedPresetLayer]; if (!layer.mask) makeMask(layer);
        layers.insert(layers.begin() + m_selectedPresetLayer + 1, layer);
        ++m_selectedPresetLayer; layersChanged = true;
    }
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(layers.size() <= 1);
    if (ui::Button("削除", 70)) {
        layers.erase(layers.begin() + m_selectedPresetLayer);
        m_selectedPresetLayer = std::min(m_selectedPresetLayer, static_cast<int>(layers.size()) - 1); layersChanged = true;
    }
    ImGui::EndDisabled();
    int moveFrom = -1, moveTo = -1;
    for (int i = static_cast<int>(layers.size()) - 1; i >= 0; --i) {
        auto& layer = layers[i]; ImGui::PushID(i);
        const auto rowWidget = ImGui::GetID("##layer");
        if (g_assetSelectionContext) layersChanged |= g_assetSelectionContext->Consume(rowWidget, layer.material);
        const auto* source = m_materialLibrary.Find(layer.material);
        const char* name = source ? source->name.c_str() : "定数マテリアル";
        const float side = ui::Scaled(40), gap = ui::Scaled(12), eye = ui::Scaled(20);
        const auto origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (ImGui::Selectable("##layer", i == m_selectedPresetLayer, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, side))) m_selectedPresetLayer = i;
        const auto next = ImGui::GetCursorScreenPos();
        AcceptAssetSlotDrop(rowWidget, layer.material, false, false);
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("TG_MATERIAL_LAYER", &i, sizeof(i)); ImGui::TextUnformatted(name); ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload("TG_MATERIAL_LAYER"); payload && payload->DataSize == sizeof(int)) {
                moveFrom = *static_cast<const int*>(payload->Data); moveTo = i;
            }
            if (const auto* payload = ImGui::AcceptDragDropPayload(kMaterialDragDropType); payload && payload->DataSize == sizeof(compositor::MaterialAssetId)) {
                const auto id = *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                if (const auto* dropped = m_materialLibrary.Find(id); dropped && !dropped->layerMaterial) {
                    layer.material = id; m_selectedPresetLayer = i; layersChanged = true;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetCursorScreenPos(ImVec2(origin.x + gap, origin.y + (side - eye) * 0.5f));
        if (ui::EyeToggle("enabled", &layer.enabled, eye)) { m_selectedPresetLayer = i; layersChanged = true; }
        ImGui::SetCursorScreenPos(ImVec2(origin.x + gap * 2 + eye, origin.y));
        if (source) ui::ThumbnailImage(static_cast<ImTextureID>(m_materialLibrary.ThumbnailHandle(layer.material).ptr), side);
        else ui::ColorSwatch(ImVec4(layer.baseColor[0], layer.baseColor[1], layer.baseColor[2], 1), side);
        const ImVec2 maskPos(origin.x + gap * 3 + eye + side, origin.y);
        ImGui::SetCursorScreenPos(maskPos);
        if (ImGui::InvisibleButton("mask", ImVec2(side, side))) m_selectedPresetLayer = i;
        if (m_materialSphere.HasMasks()) {
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(m_materialSphere.MaskHandle().ptr), maskPos,
                ImVec2(maskPos.x + side, maskPos.y + side), ImVec2(i * 0.25f, 0), ImVec2((i + 1) * 0.25f, 1));
        }
        ImGui::GetWindowDrawList()->AddRect(maskPos, ImVec2(maskPos.x + side, maskPos.y + side), ImGui::GetColorU32(ImGuiCol_Border));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("被覆マスク（白: 覆う / 黒: 覆わない）");
        const ImVec2 textPos(origin.x + gap * 4 + eye + side * 2, origin.y + (side - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::SetCursorScreenPos(textPos);
        ImGui::PushClipRect(textPos, ImVec2(origin.x + width, origin.y + side), true);
        if (layer.enabled) ImGui::Text("%s%s", i == 0 ? "下地: " : "", name);
        else ImGui::TextDisabled("%s（非表示）", name);
        ImGui::PopClipRect(); ImGui::SetCursorScreenPos(next); ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 0));
    if (moveFrom >= 0 && moveFrom < static_cast<int>(layers.size()) && moveTo >= 0 && moveTo < static_cast<int>(layers.size()) && moveFrom != moveTo) {
        auto moved = layers[moveFrom]; layers.erase(layers.begin() + moveFrom); layers.insert(layers.begin() + moveTo, moved);
        // 下地だった層を上へ動かしたときは全面マスクを与える。
        if (moveFrom == 0 && !layers[moveTo].mask) makeMask(layers[moveTo]);
        m_selectedPresetLayer = moveTo; layersChanged = true;
    }
    auto& p = layers[m_selectedPresetLayer];
    ui::SectionHeader(m_selectedPresetLayer == 0 ? "下地の設定" : "選択レイヤーの設定");
    ui::HintText("マテリアルを選択、またはアセットから下の欄へドロップして割り当て");
    ImGui::PushID(m_selectedPresetLayer);
    if (ui::BeginPropertyTable("layerAssetLayer", "高さのしきい値")) {
        layersChanged |= ui::PropertyBool("表示", &p.enabled, true, "下地を隠すと定数マテリアルを表示します");
        layersChanged |= DrawMaterialSlotRow("マテリアル", p.material, m_materialLibrary, m_pendingAssetReveal, false, true);
        layersChanged |= ui::PropertyFloat("繰り返し長", &p.uvRepeatMeters, 0.01f, 100, defaultLayer.uvRepeatMeters, "素材が一周する距離", "%.2f m");
        layersChanged |= ui::PropertyBool("ワールド座標", &p.worldUv, defaultLayer.worldUv, "Surfaceのタイル設定によらず地形の実寸で配置する");
        if (!p.material) {
            const graph::PresetMaterial defaults;
            layersChanged |= ui::PropertyColorLinear("色", p.baseColor.data(), defaults.baseColor.data(), "定数素材のベースカラー");
            layersChanged |= ui::PropertyFloat("ラフネス", &p.roughness, 0, 1, defaultLayer.roughness, "表面の粗さ");
            layersChanged |= ui::PropertyFloat("メタルネス", &p.metallic, 0, 1, defaultLayer.metallic, "金属の割合");
            layersChanged |= ui::PropertyFloat("AO", &p.ambientOcclusion, 0, 1, defaultLayer.ambientOcclusion, "環境光の遮蔽");
        }
        if (m_selectedPresetLayer > 0) {
            bool hasMask = p.mask.has_value();
            if (ui::PropertyBool("マスクを使用", &hasMask, true, "オフの層は被覆しません")) {
                if (hasMask) makeMask(p); else p.mask.reset(); layersChanged = true;
            }
        }
        if (m_selectedPresetLayer > 0 && p.mask) {
            auto& m = *p.mask;
            const char* shapes[]{"定数", "2Dノイズ", "長さ方向ノイズ", "轍（道路専用）", "道路端（道路専用）"};
            const graph::RoadMaskShape shapeValues[]{graph::RoadMaskShape::Constant, graph::RoadMaskShape::WorldNoise, graph::RoadMaskShape::LengthNoise, graph::RoadMaskShape::WheelTracks, graph::RoadMaskShape::EdgeFalloff};
            int shape = 0;
            for (int i = 0; i < 5; ++i) if (m.shape == shapeValues[i]) shape = i;
            if (ui::PropertyCombo("種類", &shape, shapes, 5, 0, "材質内部の被覆マスク")) { m.shape = shapeValues[shape]; layersChanged = true; }
            layersChanged |= ui::PropertyFloat("強さ", &m.strength, 0, 1, defaultMask.strength, "上層の被覆率");
            if (m.shape != graph::RoadMaskShape::Constant) {
                layersChanged |= ui::PropertyFloat("ノイズ寸法", &m.noiseScaleMeters, 0.05f, 100, defaultMask.noiseScaleMeters, "ノイズの模様の大きさ", "%.2f m");
                layersChanged |= ui::PropertyFloat("しきい値", &m.threshold, 0, 1, defaultMask.threshold, "被覆に使うノイズの境界");
                layersChanged |= ui::PropertyFloat("ぼかし", &m.softness, 0.0001f, 1, defaultMask.softness, "マスク境界の柔らかさ");
            }
            int seed = static_cast<int>(m.seed);
            if (ui::PropertyInt("シード", &seed, 0, 1000000, 1, "模様を変える")) { m.seed = static_cast<uint32_t>(seed); layersChanged = true; }
            layersChanged |= ui::PropertyFloat("ムラ", &m.breakupAmount, 0, 1, defaultMask.breakupAmount, "被覆率に掛けるノイズ。0で一様");
            layersChanged |= ui::PropertyFloat("ムラの寸法", &m.breakupScaleMeters, 0.05f, 100, defaultMask.breakupScaleMeters, "ムラの大きさ", "%.2f m");
            layersChanged |= ui::PropertyBool("反転", &m.invert, defaultMask.invert, "マスクの内外を反転する");
        }
        if (m_selectedPresetLayer > 0) {
            const char* modes[]{"マスクどおり", "ハイトで競合"}; int mode = static_cast<int>(p.blendMode);
            if (ui::PropertyCombo("合成方法", &mode, modes, 2, 0, "移植元と同じハイト合成の重み")) { p.blendMode = mode; layersChanged = true; }
            const char* gates[]{"なし", "高い所", "低い所"}; int gate = static_cast<int>(p.heightGate);
            if (ui::PropertyCombo("下地の高さ", &gate, gates, 3, 0, "材質内の下地ハイトで被覆を制限する")) { p.heightGate = gate; layersChanged = true; }
            if (gate) {
                layersChanged |= ui::PropertyFloat("高さのしきい値", &p.heightGateThreshold, 0, 1, defaultLayer.heightGateThreshold, "材質のハイト0〜1で指定する");
                layersChanged |= ui::PropertyFloat("高さのぼかし", &p.heightGateSoftness, 0.001f, 1, defaultLayer.heightGateSoftness, "高さによる被覆制限のぼかし幅");
            }
        }
        ui::EndPropertyTable();
    }
    ImGui::PopID();
    ImGui::EndDisabled();
    if (layersChanged && !readOnly) { material.materials = std::move(layers); changed = true; }
    if (const auto* source = m_materialLibrary.Find(asset.id); source && !source->layerError.empty()) ui::HintText(source->layerError.c_str());
    return changed;
}
}
