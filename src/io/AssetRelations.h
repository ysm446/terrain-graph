#pragma once
#include "io/ProjectWorkspace.h"
namespace tg::io {
struct AssetRelations {
    std::filesystem::path target;
    std::vector<std::filesystem::path> referencers;
    std::vector<std::filesystem::path> related;
    std::vector<std::filesystem::path> companions;
    std::vector<std::string> companionVersions;
    std::filesystem::file_time_type modified{};
    uintmax_t size = 0;
    bool complete = false;
};
AssetRelations InspectAssetRelations(ProjectWorkspace& workspace, const std::filesystem::path& target);
// 確認時から変わっていない場合だけ、元ファイルと.metaをルート内へ退避する。
bool RetireAsset(ProjectWorkspace& workspace, const AssetRelations& approved);
}
