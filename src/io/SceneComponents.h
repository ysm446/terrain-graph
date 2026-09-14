#pragma once
#include "io/ProjectWorkspace.h"

namespace tg::io {
// 保存済みの参照形式の文書を扱う。GPUや編集状態には触れない。
// 出力ノードを持つ空のグラフを、重複しない名前で作成する。
std::filesystem::path CreateGraphAsset(ProjectWorkspace& workspace,
                                       const std::filesystem::path& directory, bool cloud);
nlohmann::json AtmosphereAssetBody(const nlohmann::json& preview, const std::string& name);
bool ExpandSceneAtmosphere(ProjectWorkspace& workspace, nlohmann::json& document);
bool AssignGraphComponents(nlohmann::json& graph);
bool SaveSceneComponents(ProjectWorkspace& workspace, const std::filesystem::path& scene,
                         nlohmann::json& document);
bool ExpandSceneComponents(ProjectWorkspace& workspace, nlohmann::json& document);
std::filesystem::path MigrateSceneComponents(ProjectWorkspace& workspace,
                                            const std::filesystem::path& source);
}
