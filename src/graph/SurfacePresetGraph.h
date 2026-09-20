#pragma once
#include "graph/LayerMaterial.h"

namespace tg::graph {
bool ValidatePresetMaterial(const PresetMaterial& material, std::string& error);
bool ValidatePresetGraph(const PresetGraph& graph, std::string& error);
PresetGraph MakePresetGraph(const std::vector<PresetMaterial>& materials);
// 未接続の下地・出力は定数材質、未接続の上層・マスクは被覆なし。
// 編集用。非表示レイヤーの素材・マスク設定を保持する。
bool ExtractPresetLayers(const LayerMaterial& material, std::vector<PresetMaterial>& layers, std::string& error);
bool CompilePresetMaterials(const LayerMaterial& material, std::vector<PresetMaterial>& layers, std::string& error);
uint32_t AddPresetNode(PresetGraph& graph, PresetNodeKind kind, std::array<float, 2> position);
bool ConnectPresetNodes(PresetGraph& graph, uint32_t source, uint32_t target, uint32_t input, std::string& error);
bool DeletePresetNode(PresetGraph& graph, uint32_t id);
bool AppendPresetLayer(PresetGraph& graph, std::string& error);
}  // namespace tg::graph
