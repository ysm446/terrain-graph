#pragma once
#include "io/ProjectWorkspace.h"
namespace tg::io {
struct ThumbnailRecord {
    std::filesystem::path image;
    std::filesystem::path metadata;
    std::string stamp;
};
ThumbnailRecord AssetThumbnailRecord(ProjectWorkspace& workspace, const std::filesystem::path& path);
bool ThumbnailIsCurrent(const ThumbnailRecord& record);
bool CommitThumbnail(const ThumbnailRecord& record);
// 移動したアセットの保存済みサムネイルを、移動先の名前へ引き継ぐ（作り直しを避ける）。
// 移動先に既にあれば何もしない。引き継いだ画像は「古いもの」として出る。
void CarryAssetThumbnail(const ProjectWorkspace& workspace, const std::filesystem::path& from,
                         const std::filesystem::path& to);
std::filesystem::path SceneThumbnailPath(const ProjectWorkspace& workspace, const std::filesystem::path& scene);
void MigrateSceneThumbnails(const ProjectWorkspace& workspace);
}
