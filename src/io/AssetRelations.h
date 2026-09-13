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
// ファイルと付随物（.meta、シーンなら <名前>.assets）をルート内の別フォルダへ移す。
// 参照はIDで解決されるので、移動後に再スキャンすれば切れない。
// 成功したら移動先のパスを返し、失敗したら空を返す（途中で失敗した分は戻す）。
std::filesystem::path MoveAsset(ProjectWorkspace& workspace, const std::filesystem::path& target,
                                const std::filesystem::path& directory);
// 同じフォルダ内で名前を変える。ファイルなら付随物も新しい名前へ揃え、フォルダはそのまま改名する。
// 成功したら新しいパスを返し、失敗したら空を返す。
std::filesystem::path RenameAsset(ProjectWorkspace& workspace, const std::filesystem::path& target,
                                  const std::string& newName);
}
