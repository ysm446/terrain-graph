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
    ui::SectionHeader(ui::Tr("レイヤーマテリアル", "Layered Material"));
    if (ui::BeginPropertyTable("layerAssetBasics")) {
        char name[128]{}; std::snprintf(name, sizeof(name), "%s", asset.name.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name), ui::Tr("共有材質の名前", "Name of the shared material"))) {
            if (asset.assetPath.empty()) { asset.name = name; material.name = name; changed = true; }
            else RequestAssetRename(asset.assetPath, name);
        }
        changed |= ui::PropertyFloat(ui::Tr("合成幅", "Blend Range"), &material.layerBlendRange, 0, 1, defaultMaterial.layerBlendRange, ui::Tr("材質内部のハイトによる境界の混ざり幅", "Width of the height-based blend between layers in the material"));
        changed |= ui::PropertyFloat(ui::Tr("変位量", "Displacement"), &material.displacementMeters, 0, 10, defaultMaterial.displacementMeters, ui::Tr("ハイト0〜1の全幅に対応する起伏。中間グレーが変位ゼロ", "Relief for the full 0-1 height range. Mid gray is zero displacement"), "%.3f m");
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
        ui::HintText("%s", ui::Tr("ノード形式の素材です。変換すると出力につながる層を取り込みます。未使用ノードは除かれます（Undo可能）", "This material uses nodes. Converting imports the layers connected to the output and removes unused nodes (undoable)"));
        if (ui::Button(ui::Tr("レイヤー形式に変換", "Convert to Layers"), ui::kWideButtonWidth)) {
            material.materials = layers; material.materialGraph.reset(); changed = true;
        }
    }
    const bool readOnly = material.materialGraph.has_value();
    bool layersChanged = false;
    m_selectedPresetLayer = std::clamp(m_selectedPresetLayer, 0, static_cast<int>(layers.size()) - 1);
    const auto makeMask = [](graph::PresetMaterial& layer) {
        layer.mask.emplace(); layer.mask->shape = graph::RoadMaskShape::Constant; layer.mask->breakupAmount = 0;
    };
    ui::SectionHeader(ui::Tr("レイヤー", "Layers"));
    ui::HintText("%s", ui::Tr("上の行ほど上層です。ドラッグで順序を変更できます（最大4層）", "Higher rows are upper layers. Drag to reorder (up to 4 layers)"));
    ImGui::BeginDisabled(readOnly);
    ImGui::BeginDisabled(layers.size() >= 4);
    if (ui::Button(ui::Tr("追加", "Add"), 70)) {
        graph::PresetMaterial layer; makeMask(layer); layers.push_back(layer);
        m_selectedPresetLayer = static_cast<int>(layers.size()) - 1; layersChanged = true;
    }
    ImGui::SameLine();
    if (ui::Button(ui::Tr("複製", "Duplicate"), 70)) {
        auto layer = layers[m_selectedPresetLayer]; if (!layer.mask) makeMask(layer);
        layers.insert(layers.begin() + m_selectedPresetLayer + 1, layer);
        ++m_selectedPresetLayer; layersChanged = true;
    }
    ImGui::EndDisabled(); ImGui::SameLine(); ImGui::BeginDisabled(layers.size() <= 1);
    if (ui::Button(ui::Tr("削除", "Delete"), 70)) {
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
        const char* name = source ? source->name.c_str() : ui::Tr("定数マテリアル", "Constant Material");
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
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::Tr("被覆マスク（白: 覆う / 黒: 覆わない）", "Coverage mask (white: covered / black: not covered)"));
        const ImVec2 textPos(origin.x + gap * 4 + eye + side * 2, origin.y + (side - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::SetCursorScreenPos(textPos);
        ImGui::PushClipRect(textPos, ImVec2(origin.x + width, origin.y + side), true);
        if (layer.enabled) ImGui::Text("%s%s", i == 0 ? ui::Tr("下地: ", "Base: ") : "", name);
        else ImGui::TextDisabled(ui::Tr("%s（非表示）", "%s (Hidden)"), name);
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
    ui::SectionHeader(m_selectedPresetLayer == 0 ? ui::Tr("下地の設定", "Base Settings") : ui::Tr("選択レイヤーの設定", "Selected Layer"));
    ui::HintText("%s", ui::Tr("マテリアルを選択、またはアセットから下の欄へドロップして割り当て", "Select a material, or drop one from the assets onto the field below to assign it"));
    ImGui::PushID(m_selectedPresetLayer);
    if (ui::BeginPropertyTable("layerAssetLayer", ui::Tr("高さのしきい値", "Lane Center Offset"))) {
        layersChanged |= ui::PropertyBool(ui::Tr("表示", "Visible"), &p.enabled, true, ui::Tr("下地を隠すと定数マテリアルを表示します", "Hiding the base shows the constant material"));
        layersChanged |= DrawMaterialSlotRow(ui::Tr("マテリアル", "Material"), p.material, m_materialLibrary, m_pendingAssetReveal, false, true);
        layersChanged |= ui::PropertyFloat(ui::Tr("繰り返し長", "Repeat Length"), &p.uvRepeatMeters, 0.01f, 100, defaultLayer.uvRepeatMeters, ui::Tr("素材が一周する距離", "Distance over which the material repeats"), "%.2f m");
        layersChanged |= ui::PropertyBool(ui::Tr("ワールド座標", "World Space"), &p.worldUv, defaultLayer.worldUv, ui::Tr("Surfaceのタイル設定によらず地形の実寸で配置する", "Places it at the terrain's real scale regardless of the Surface tiling"));
        if (!p.material) {
            const graph::PresetMaterial defaults;
            layersChanged |= ui::PropertyColorLinear(ui::Tr("色", "Color"), p.baseColor.data(), defaults.baseColor.data(), ui::Tr("定数素材のベースカラー", "Base color of the constant material"));
            layersChanged |= ui::PropertyFloat(ui::Tr("ラフネス", "Roughness"), &p.roughness, 0, 1, defaultLayer.roughness, ui::Tr("表面の粗さ", "Surface roughness"));
            layersChanged |= ui::PropertyFloat(ui::Tr("メタルネス", "Metalness"), &p.metallic, 0, 1, defaultLayer.metallic, ui::Tr("金属の割合", "Metallic fraction"));
            layersChanged |= ui::PropertyFloat("AO", &p.ambientOcclusion, 0, 1, defaultLayer.ambientOcclusion, ui::Tr("環境光の遮蔽", "Ambient occlusion"));
        }
        if (m_selectedPresetLayer > 0) {
            bool hasMask = p.mask.has_value();
            if (ui::PropertyBool(ui::Tr("マスクを使用", "Use Mask"), &hasMask, true, ui::Tr("オフの層は被覆しません", "When off, the layer does not cover"))) {
                if (hasMask) makeMask(p); else p.mask.reset(); layersChanged = true;
            }
        }
        if (m_selectedPresetLayer > 0 && p.mask) {
            auto& m = *p.mask;
            const char* shapes[]{ui::Tr("定数", "Constant"), ui::Tr("2Dノイズ", "2D Noise"), ui::Tr("長さ方向ノイズ", "Lengthwise Noise"),
                                 ui::Tr("轍（道路専用）", "Wheel Tracks (Road Only)"), ui::Tr("道路端（道路専用）", "Road Edge (Road Only)")};
            const graph::RoadMaskShape shapeValues[]{graph::RoadMaskShape::Constant, graph::RoadMaskShape::WorldNoise, graph::RoadMaskShape::LengthNoise, graph::RoadMaskShape::WheelTracks, graph::RoadMaskShape::EdgeFalloff};
            int shape = 0;
            for (int i = 0; i < 5; ++i) if (m.shape == shapeValues[i]) shape = i;
            if (ui::PropertyCombo(ui::Tr("種類", "Type"), &shape, shapes, 5, 0, ui::Tr("材質内部の被覆マスク", "Coverage mask within the material"))) { m.shape = shapeValues[shape]; layersChanged = true; }
            layersChanged |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &m.strength, 0, 1, defaultMask.strength, ui::Tr("上層の被覆率", "Coverage of the upper layer"));
            // 轍と道路端は Road Mesh の路面の座標（幅・車線）で決まる。Surface では覆わない。
            if (m.shape == graph::RoadMaskShape::WheelTracks) {
                layersChanged |= ui::PropertyBool(ui::Tr("車線に合わせる", "Follow Lanes"), &m.tracksFromLanes, defaultMask.tracksFromLanes,
                                                  ui::Tr("Road Mesh の車線の中央ごとに轍を置く。切ると道路の中心から手入力の距離に置く", "Places tracks at the center of each Road Mesh lane. When off, places them at a manual distance from the road center"));
                layersChanged |= ui::PropertyBool(ui::Tr("対向車線にも", "Opposing Lane Too"), &m.bothLanes, defaultMask.bothLanes,
                                                  ui::Tr("対向の車線にも置く（手入力のときは中心の左右両方）", "Also places them on the opposing lane (both sides of the center when manual)"));
                if (!m.tracksFromLanes)
                    layersChanged |= ui::PropertyFloat(ui::Tr("車線中央の位置", "Lane Center Offset"), &m.laneOffsetMeters, -10, 10, defaultMask.laneOffsetMeters,
                                                       ui::Tr("道路の中心から車線の中央までの距離（正が左）", "Distance from the road center to the lane center (positive is left)"), "%.2f m");
                layersChanged |= ui::PropertyFloat(ui::Tr("タイヤ間隔", "Track Spacing"), &m.trackSpacingMeters, 0.5f, 3, defaultMask.trackSpacingMeters,
                                                   ui::Tr("左右の轍の中心の間隔", "Distance between the left and right track centers"), "%.2f m");
                layersChanged |= ui::PropertyFloat(ui::Tr("轍の幅", "Track Width"), &m.trackWidthMeters, 0.05f, 1.5f, defaultMask.trackWidthMeters,
                                                   ui::Tr("轍 1 本の幅", "Width of one track"), "%.2f m");
                layersChanged |= ui::PropertyFloat(ui::Tr("縁のぼかし", "Edge Blur"), &m.featherMeters, 0, 2, defaultMask.featherMeters,
                                                   ui::Tr("轍の縁を 0 へ落とす幅", "Width over which the track edges fall off to 0"), "%.2f m");
            } else if (m.shape == graph::RoadMaskShape::EdgeFalloff) {
                const char* sides[]{ui::Tr("両側", "Both"), ui::Tr("左", "Left"), ui::Tr("右", "Right")};
                int side = static_cast<int>(m.edgeSide);
                if (ui::PropertyCombo(ui::Tr("側", "Side"), &side, sides, 3, 0, ui::Tr("どちらの端に置くか（進行方向に向かって）", "Which edge to place it on (facing the direction of travel)"))) {
                    m.edgeSide = static_cast<graph::RoadMaskSide>(side);
                    layersChanged = true;
                }
                layersChanged |= ui::PropertyFloat(ui::Tr("端の幅", "Edge Width"), &m.edgeWidthMeters, 0, 10, defaultMask.edgeWidthMeters,
                                                   ui::Tr("端から 1 のまま覆う幅", "Width from the edge that stays fully covered"), "%.2f m");
                layersChanged |= ui::PropertyFloat(ui::Tr("縁のぼかし", "Edge Blur"), &m.featherMeters, 0, 5, defaultMask.featherMeters,
                                                   ui::Tr("端の幅の内側を 0 へ落とす幅", "Width inside the edge width that falls off to 0"), "%.2f m");
            } else if (m.shape != graph::RoadMaskShape::Constant) {
                layersChanged |= ui::PropertyFloat(ui::Tr("ノイズ寸法", "Noise Scale"), &m.noiseScaleMeters, 0.05f, 100, defaultMask.noiseScaleMeters, ui::Tr("ノイズの模様の大きさ", "Size of the noise pattern"), "%.2f m");
                layersChanged |= ui::PropertyFloat(ui::Tr("しきい値", "Threshold"), &m.threshold, 0, 1, defaultMask.threshold, ui::Tr("被覆に使うノイズの境界", "Noise level that bounds the coverage"));
                layersChanged |= ui::PropertyFloat(ui::Tr("ぼかし", "Blur"), &m.softness, 0.0001f, 1, defaultMask.softness, ui::Tr("マスク境界の柔らかさ", "Softness of the mask boundary"));
            }
            int seed = static_cast<int>(m.seed);
            if (ui::PropertyInt(ui::Tr("シード", "Seed"), &seed, 0, 1000000, 1, ui::Tr("模様を変える", "Changes the pattern"))) { m.seed = static_cast<uint32_t>(seed); layersChanged = true; }
            layersChanged |= ui::PropertyFloat(ui::Tr("ムラ", "Breakup"), &m.breakupAmount, 0, 1, defaultMask.breakupAmount, ui::Tr("被覆率に掛けるノイズ。0で一様", "Noise multiplied into the coverage. 0 is uniform"));
            layersChanged |= ui::PropertyFloat(ui::Tr("ムラの寸法", "Breakup Scale"), &m.breakupScaleMeters, 0.05f, 100, defaultMask.breakupScaleMeters, ui::Tr("ムラの大きさ", "Size of the breakup"), "%.2f m");
            layersChanged |= ui::PropertyBool(ui::Tr("反転", "Invert"), &m.invert, defaultMask.invert, ui::Tr("マスクの内外を反転する", "Inverts the mask"));
        }
        if (m_selectedPresetLayer > 0) {
            const char* modes[]{ui::Tr("マスクどおり", "By Mask"), ui::Tr("ハイトで競合", "Height Blend")}; int mode = static_cast<int>(p.blendMode);
            if (ui::PropertyCombo(ui::Tr("合成方法", "Blend Mode"), &mode, modes, 2, 0, ui::Tr("移植元と同じハイト合成の重み", "Height blend weighting, as in the original it was ported from"))) { p.blendMode = mode; layersChanged = true; }
            const char* gates[]{ui::Tr("なし", "None"), ui::Tr("高い所", "High Areas"), ui::Tr("低い所", "Low Areas")}; int gate = static_cast<int>(p.heightGate);
            if (ui::PropertyCombo(ui::Tr("下地の高さ", "Base Height"), &gate, gates, 3, 0, ui::Tr("材質内の下地ハイトで被覆を制限する", "Limits coverage by the base height within the material"))) { p.heightGate = gate; layersChanged = true; }
            if (gate) {
                layersChanged |= ui::PropertyFloat(ui::Tr("高さのしきい値", "Height Threshold"), &p.heightGateThreshold, 0, 1, defaultLayer.heightGateThreshold, ui::Tr("材質のハイト0〜1で指定する", "Given in the material's 0-1 height"));
                layersChanged |= ui::PropertyFloat(ui::Tr("高さのぼかし", "Height Blur"), &p.heightGateSoftness, 0.001f, 1, defaultLayer.heightGateSoftness, ui::Tr("高さによる被覆制限のぼかし幅", "Blur width of the height-based coverage limit"));
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
