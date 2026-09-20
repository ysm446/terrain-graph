#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "ui/UiStyle.h"
#include "graph/SurfacePresetGraph.h"
#include <imgui_internal.h>
#include <imgui-node-editor/imgui_node_editor.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
namespace tg {
namespace ed = ax::NodeEditor;
namespace {
const char* PresetNodeLabel(graph::PresetNodeKind kind) {
    switch (kind) {
        case graph::PresetNodeKind::Material: return "素材";
        case graph::PresetNodeKind::Mask: return "マスク";
        case graph::PresetNodeKind::Blend: return "合成";
        default: return "出力";
    }
}
void PresetPin(uint32_t id, const char* label, bool output, bool mask) {
    ed::BeginPin(ed::PinId(id), output ? ed::PinKind::Output : ed::PinKind::Input);
    const auto color = ImGui::GetStyleColorVec4(mask ? ImGuiCol_PlotHistogramHovered : ImGuiCol_Text);
    if (output) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ui::TextScaled(130) - ImGui::CalcTextSize(label).x);
    ImGui::TextColored(color, "%s", label);
    const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const ImVec2 pivot(output ? max.x + 8 : min.x - 8, (min.y + max.y) * 0.5f);
    ImGui::GetWindowDrawList()->AddCircleFilled(pivot, 4, ImGui::ColorConvertFloat4ToU32(color));
    ed::PinPivotRect(pivot, pivot);
    ed::PinRect(ImVec2(min.x - 14, min.y), ImVec2(max.x + 14, max.y));
    ed::EndPin();
}
}
bool Application::DrawSurfacePresetGraph(graph::LayerMaterial& preset) {
    auto& graph = *preset.materialGraph;
    bool changed = false;
    bool frameAll = false;
    if (!m_presetNodeEditor || m_presetEditorId != preset.id) {
        if (m_presetNodeEditor) ed::DestroyEditor(m_presetNodeEditor);
        ed::Config config{}; config.SettingsFile = nullptr; config.NavigateButtonIndex = 2;
        m_presetNodeEditor = ed::CreateEditor(&config);
        m_presetEditorId = preset.id;
        m_selectedPresetNode = graph.nodes.front().id;
        frameAll = true;
    }
    ed::SetCurrentEditor(m_presetNodeEditor);
    if (frameAll) { ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode)); }
    if (ui::Button("層を追加", 100)) {
        if (graph::AppendPresetLayer(graph, m_surfacePresetError)) {
            changed = true; frameAll = true;
            m_selectedPresetNode = graph.nodes.back().id;
            ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode));
        }
    }
    ImGui::BeginDisabled(graph.nodes.size() >= 32);
    for (const auto kind : {graph::PresetNodeKind::Material, graph::PresetNodeKind::Mask, graph::PresetNodeKind::Blend}) {
        if (kind != graph::PresetNodeKind::Material) ImGui::SameLine();
        const std::string label = std::string(PresetNodeLabel(kind)) + "を追加";
        if (ui::Button(label.c_str(), 100)) {
            const auto position = ed::ScreenToCanvas(ImGui::GetCursorScreenPos());
            m_selectedPresetNode = graph::AddPresetNode(graph, kind, {position.x + 40, position.y + 100});
            ed::SetNodePosition(ed::NodeId(m_selectedPresetNode), ImVec2(position.x + 40, position.y + 100));
            ed::ClearSelection(); ed::SelectNode(ed::NodeId(m_selectedPresetNode));
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (ui::Button("全体を表示", 100)) frameAll = true;
    ImGui::SameLine();
    if (ui::Button("選択を削除", 100)) changed |= graph::DeletePresetNode(graph, m_selectedPresetNode);
    ui::HintText("丸をドラッグして接続。合成の上層には素材を接続します（最大4層）");
    const float height = std::clamp(ImGui::GetContentRegionAvail().y * 0.48f, ui::Scaled(180), ui::Scaled(370));
    auto gridColor = ImGui::GetStyleColorVec4(ImGuiCol_Border); gridColor.w = 0;
    ed::PushStyleColor(ed::StyleColor_Bg, gridColor);
    ed::PushStyleColor(ed::StyleColor_Grid, gridColor);
    ed::Begin("presetMaterialGraph", ImVec2(0, height));
    const bool moving = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    for (const auto& node : graph.nodes) {
        const ed::NodeId id(node.id);
        if (!moving || frameAll) ed::SetNodePosition(id, ImVec2(node.position[0], node.position[1]));
        ed::PushStyleColor(ed::StyleColor_NodeBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ed::PushStyleColor(ed::StyleColor_NodeBorder, ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ed::BeginNode(id);
        ImGui::TextUnformatted(PresetNodeLabel(node.kind));
        ImGui::Dummy(ImVec2(ui::TextScaled(130), 3));
        if (node.kind == graph::PresetNodeKind::Material) {
            const auto* asset = m_materialLibrary.Find(node.settings.material);
            ui::ThumbnailImage(static_cast<ImTextureID>(m_materialLibrary.ThumbnailHandle(node.settings.material).ptr),
                ui::Scaled(ui::kNodeThumbnail));
            const char* name = asset ? asset->name.c_str() : "定数マテリアル";
            std::string label = name;
            while (!label.empty() && ImGui::CalcTextSize(label.c_str()).x > ui::TextScaled(130)) {
                size_t end = label.size() - 1;
                while (end && (static_cast<unsigned char>(label[end]) & 0xC0) == 0x80) --end;
                label.resize(end);
            }
            ImGui::TextUnformatted(label.c_str());
        }
        if (node.kind == graph::PresetNodeKind::Blend || node.kind == graph::PresetNodeKind::Output)
            PresetPin(node.id * 8 + 1, node.kind == graph::PresetNodeKind::Output ? "面" : "下地", false, false);
        if (node.kind == graph::PresetNodeKind::Blend) {
            PresetPin(node.id * 8 + 2, "上層素材", false, false);
            PresetPin(node.id * 8 + 3, "マスク", false, true);
            ImGui::TextUnformatted(node.settings.blendMode ? "ハイトで競合" : "マスクどおり");
        }
        if (node.kind != graph::PresetNodeKind::Output)
            PresetPin(node.id * 8 + 4, node.kind == graph::PresetNodeKind::Mask ? "マスク出力" : "マテリアル出力", true,
                node.kind == graph::PresetNodeKind::Mask);
        ed::EndNode(); ed::PopStyleColor(2);
    }
    if (frameAll || changed) ed::SelectNode(ed::NodeId(m_selectedPresetNode));
    for (const auto& node : graph.nodes) for (uint32_t input = 0; input < 3; ++input) if (node.inputs[input]) {
        const auto color = ImGui::GetStyleColorVec4(input == 2 ? ImGuiCol_PlotHistogramHovered : ImGuiCol_Text);
        ed::Link(ed::LinkId(node.id * 8 + input + 1), ed::PinId(node.inputs[input] * 8 + 4),
            ed::PinId(node.id * 8 + input + 1), color, 2);
    }
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b) && a && b) {
            uint32_t source = static_cast<uint32_t>(a.Get()), target = static_cast<uint32_t>(b.Get());
            if (target % 8 == 4) std::swap(source, target);
            auto candidate = graph;
            std::string error;
            if (source % 8 == 4 && target % 8 >= 1 && target % 8 <= 3 &&
                graph::ConnectPresetNodes(candidate, source / 8, target / 8, target % 8 - 1, error)) {
                if (ed::AcceptNewItem()) { graph = std::move(candidate); changed = true; m_surfacePresetError.clear(); }
            } else {
                ed::RejectNewItem();
                m_surfacePresetError = error.empty() ? "出力と入力の丸を接続してください" : error;
            }
        }
    }
    ed::EndCreate();
    if (ed::BeginDelete()) {
        ed::LinkId link;
        while (ed::QueryDeletedLink(&link)) if (ed::AcceptDeletedItem()) {
            const auto id = static_cast<uint32_t>(link.Get());
            std::string error;
            changed |= graph::ConnectPresetNodes(graph, 0, id / 8, id % 8 - 1, error);
        }
        ed::NodeId node;
        while (ed::QueryDeletedNode(&node)) {
            const auto id = static_cast<uint32_t>(node.Get());
            const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(), [id](const auto& n) { return n.id == id; });
            if (found != graph.nodes.end() && found->kind != graph::PresetNodeKind::Output) {
                if (ed::AcceptDeletedItem()) changed |= graph::DeletePresetNode(graph, id);
            } else ed::RejectDeletedItem();
        }
    }
    ed::EndDelete();
    ed::NodeId selected;
    if (ed::GetSelectedNodes(&selected, 1)) m_selectedPresetNode = static_cast<uint32_t>(selected.Get());
    for (auto& node : graph.nodes) {
        const auto position = ed::GetNodePosition(ed::NodeId(node.id));
        if (moving && std::isfinite(position.x) && std::isfinite(position.y) && std::abs(position.x) < 1e6f && std::abs(position.y) < 1e6f && (std::abs(position.x - node.position[0]) > 0.1f || std::abs(position.y - node.position[1]) > 0.1f)) {
            node.position = {position.x, position.y}; changed = true;
        }
    }
    if (frameAll) m_presetNavigateFrames = 3;
    if (m_presetNavigateFrames > 0 && --m_presetNavigateFrames == 0) ed::NavigateToContent(0);
    ed::End(); ed::PopStyleColor(2); ed::SetCurrentEditor(nullptr);
    return changed;
}
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
    if (!material.materialGraph) material.materialGraph = graph::MakePresetGraph(material.materials);
    changed |= DrawSurfacePresetGraph(material);
    if (!m_surfacePresetError.empty()) ui::HintText(m_surfacePresetError.c_str());
    const auto current = std::find_if(material.materialGraph->nodes.begin(), material.materialGraph->nodes.end(), [&](const auto& n) { return n.id == m_selectedPresetNode; });
    if (current != material.materialGraph->nodes.end()) {
        auto& node = *current; auto& p = node.settings;
        ui::SectionHeader(PresetNodeLabel(node.kind));
        if (ui::BeginPropertyTable("layerAssetNode", "高さのしきい値")) {
            if (node.kind == graph::PresetNodeKind::Material) {
                std::vector<const char*> names{"定数"}; std::vector<uint32_t> ids{0}; int selected = 0;
                for (const auto& source : m_materialLibrary.Entries()) if (!source.layerMaterial) { if (source.id == p.material) selected = static_cast<int>(ids.size()); names.push_back(source.name.c_str()); ids.push_back(source.id); }
                if (ui::PropertyCombo("素材", &selected, names.data(), static_cast<int>(names.size()), 0, "通常のPBR素材。合成材質の入れ子には対応しません")) { p.material = ids[selected]; changed = true; }
                changed |= ui::PropertyFloat("繰り返し長", &p.uvRepeatMeters, 0.01f, 100, defaultLayer.uvRepeatMeters, "素材が一周する距離", "%.2f m");
                changed |= ui::PropertyBool("ワールド座標", &p.worldUv, defaultLayer.worldUv, "Surfaceのタイル設定によらず地形の実寸で配置する");
                if (!p.material) {
                    const graph::PresetMaterial defaults;
                    changed |= ui::PropertyColorLinear("色", p.baseColor.data(), defaults.baseColor.data(), "定数素材のベースカラー");
                    changed |= ui::PropertyFloat("ラフネス", &p.roughness, 0, 1, defaultLayer.roughness, "表面の粗さ");
                    changed |= ui::PropertyFloat("メタルネス", &p.metallic, 0, 1, defaultLayer.metallic, "金属の割合");
                    changed |= ui::PropertyFloat("AO", &p.ambientOcclusion, 0, 1, defaultLayer.ambientOcclusion, "環境光の遮蔽");
                }
            } else if (node.kind == graph::PresetNodeKind::Mask && p.mask) {
                auto& m = *p.mask;
                const char* shapes[]{"定数", "2Dノイズ", "長さ方向ノイズ"};
                int shape = m.shape == graph::RoadMaskShape::Constant ? 0 : m.shape == graph::RoadMaskShape::WorldNoise ? 1 : 2;
                if (ui::PropertyCombo("種類", &shape, shapes, 3, 0, "材質内部の被覆マスク")) { m.shape = shape == 0 ? graph::RoadMaskShape::Constant : shape == 1 ? graph::RoadMaskShape::WorldNoise : graph::RoadMaskShape::LengthNoise; changed = true; }
                changed |= ui::PropertyFloat("強さ", &m.strength, 0, 1, defaultMask.strength, "上層の被覆率");
                if (m.shape != graph::RoadMaskShape::Constant) {
                    changed |= ui::PropertyFloat("ノイズ寸法", &m.noiseScaleMeters, 0.05f, 100, defaultMask.noiseScaleMeters, "ノイズの模様の大きさ", "%.2f m");
                    changed |= ui::PropertyFloat("しきい値", &m.threshold, 0, 1, defaultMask.threshold, "被覆に使うノイズの境界");
                    changed |= ui::PropertyFloat("ぼかし", &m.softness, 0.0001f, 1, defaultMask.softness, "マスク境界の柔らかさ");
                }
                int seed = static_cast<int>(m.seed);
                if (ui::PropertyInt("シード", &seed, 0, 1000000, 1, "模様を変える")) { m.seed = static_cast<uint32_t>(seed); changed = true; }
                changed |= ui::PropertyFloat("ムラ", &m.breakupAmount, 0, 1, defaultMask.breakupAmount, "被覆率に掛けるノイズ。0で一様");
                changed |= ui::PropertyFloat("ムラの寸法", &m.breakupScaleMeters, 0.05f, 100, defaultMask.breakupScaleMeters, "ムラの大きさ", "%.2f m");
                changed |= ui::PropertyBool("反転", &m.invert, defaultMask.invert, "マスクの内外を反転する");
            } else if (node.kind == graph::PresetNodeKind::Blend) {
                const char* modes[]{"マスクどおり", "ハイトで競合"}; int mode = static_cast<int>(p.blendMode);
                if (ui::PropertyCombo("合成方法", &mode, modes, 2, 0, "移植元と同じハイト合成の重み")) { p.blendMode = mode; changed = true; }
                const char* gates[]{"なし", "高い所", "低い所"}; int gate = static_cast<int>(p.heightGate);
                if (ui::PropertyCombo("下地の高さ", &gate, gates, 3, 0, "材質内の下地ハイトで被覆を制限する")) { p.heightGate = gate; changed = true; }
                if (gate) {
                    changed |= ui::PropertyFloat("高さのしきい値", &p.heightGateThreshold, 0, 1, defaultLayer.heightGateThreshold, "材質のハイト0〜1で指定する");
                    changed |= ui::PropertyFloat("高さのぼかし", &p.heightGateSoftness, 0.001f, 1, defaultLayer.heightGateSoftness, "高さによる被覆制限のぼかし幅");
                }
            }
            ui::EndPropertyTable();
        }
    }
    if (const auto* source = m_materialLibrary.Find(asset.id); source && !source->layerError.empty()) ui::HintText(source->layerError.c_str());
    return changed;
}
}
