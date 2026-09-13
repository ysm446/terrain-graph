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
std::filesystem::path SceneThumbnailPath(const ProjectWorkspace& workspace, const std::filesystem::path& scene);
void MigrateSceneThumbnails(const ProjectWorkspace& workspace);
}
