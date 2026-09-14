#pragma once
#include "io/ProjectWorkspace.h"

namespace tg::io {
// 保存済みの参照形式の文書を扱う。GPUや編集状態には触れない。
bool AssignGraphComponents(nlohmann::json& graph);
bool SaveSceneComponents(ProjectWorkspace& workspace, const std::filesystem::path& scene,
                         nlohmann::json& document);
bool ExpandSceneComponents(ProjectWorkspace& workspace, nlohmann::json& document);
std::filesystem::path MigrateSceneComponents(ProjectWorkspace& workspace,
                                            const std::filesystem::path& source);
}
