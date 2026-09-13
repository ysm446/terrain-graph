#include "io/ThumbnailStore.h"
#include "core/PathUtf8.h"
#include "core/Log.h"
#include <unordered_set>
#include <functional>
namespace tg::io {
namespace fs = std::filesystem;
namespace {
uint64_t Hash(std::string_view text, uint64_t hash = 14695981039346656037ull) {
    for (unsigned char c : text) { hash ^= c; hash *= 1099511628211ull; }
    return hash;
}
}
fs::path SceneThumbnailPath(const ProjectWorkspace& workspace, const fs::path& scene) {
    if (!workspace.Contains(scene)) return {};
    nlohmann::json document;
    const auto uid = ProjectWorkspace::ReadJson(scene, document) ? ProjectWorkspace::String(document, "sceneUid") : "";
    const auto key = uid.empty() ? "legacy-" + std::to_string(Hash(ToUtf8Portable(scene.lexically_relative(workspace.Root())))) :
                                  "scene-" + std::to_string(Hash(uid));
    const auto target = workspace.Root() / L".terrain-graph" / L"scene-thumbnails" / FromUtf8(key + ".png");
    return workspace.Contains(target) ? target : fs::path{};
}
void MigrateSceneThumbnails(const ProjectWorkspace& workspace) {
    std::error_code error;
    fs::recursive_directory_iterator it(workspace.Root(), fs::directory_options::skip_permission_denied, error), end;
    std::vector<fs::path> scenes;
    for (; it != end && !error; it.increment(error)) {
        if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(error)) {
            nlohmann::json nested;
            if (it->path().filename().wstring().starts_with(L".") ||
                (ProjectWorkspace::ReadJson(it->path() / L"project.tgproj", nested) &&
                 ProjectWorkspace::String(nested, "format") == "terrain-graph.workspace"))
                it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() == L".tgscene") scenes.push_back(it->path());
    }
    for (const auto& scene : scenes) {
        const auto target = SceneThumbnailPath(workspace, scene);
        if (target.empty()) continue;
        const auto old = scene.parent_path() / (scene.stem().wstring() + L".assets") / L"thumbnail.png";
        if (!workspace.Contains(old) || !workspace.Contains(target) || !fs::is_regular_file(old, error) || fs::exists(target, error)) continue;
        fs::create_directories(target.parent_path(), error);
        if (!error) fs::rename(old, target, error);
        if (error) TG_LOG_WARN("シーンサムネイルを移行できません: %s", ToUtf8Display(old).c_str());
        else {
            // ペイント等が残るフォルダは消さない。
            if (fs::is_empty(old.parent_path(), error)) fs::remove(old.parent_path(), error);
        }
    }
}
ThumbnailRecord AssetThumbnailRecord(ProjectWorkspace& workspace, const fs::path& path) {
    const auto relative = ToUtf8Portable(path.lexically_relative(workspace.Root()));
    const auto directory = workspace.Root() / L".terrain-graph" / L"thumbnails";
    const auto key = std::to_wstring(Hash(relative));
    // 形式・描画条件の変更時に版を上げて古いキャッシュを無効化する。
    uint64_t stamp = Hash("thumbnail-v1");
    std::unordered_set<std::string> visited;
    std::function<void(const fs::path&)> visit;
    visit = [&](const fs::path& file) {
        const auto name = ToUtf8Portable(file.lexically_normal());
        if (!visited.insert(name).second) return;
        std::error_code error;
        const auto time = fs::last_write_time(file, error);
        stamp = Hash(name, stamp);
        stamp = Hash(error ? "missing" : std::to_string(time.time_since_epoch().count()), stamp);
        const auto size = fs::file_size(file, error);
        if (!error) stamp = Hash(std::to_string(size), stamp);
        const auto extension = file.extension().wstring();
        if (_wcsicmp(extension.c_str(), L".tgmat") && _wcsicmp(extension.c_str(), L".tgsky") && _wcsicmp(extension.c_str(), L".tgmodel")) return;
        nlohmann::json document;
        if (!ProjectWorkspace::ReadJson(file, document)) return;
        const auto refs = [&](auto&& self, const nlohmann::json& value) -> void {
            if (value.is_object() && value.contains("uid") && value.contains("path")) {
                const auto dependency = workspace.Resolve(value);
                if (!dependency.empty()) visit(dependency);
                else stamp = Hash("missing-reference", stamp);
                return;
            }
            if (value.is_structured()) { for (const auto& child : value) self(self, child); }
            else if (value.is_string()) {
                // 旧単体マテリアルの相対パス参照。
                const auto candidate = file.parent_path() / FromUtf8(value.get<std::string>());
                std::error_code existsError;
                if (fs::is_regular_file(candidate, existsError)) visit(candidate);
            }
        };
        refs(refs, document);
    };
    visit(path);
    return {directory / (key + L".png"), directory / (key + L".json"), std::to_string(stamp)};
}
bool ThumbnailIsCurrent(const ThumbnailRecord& record) {
    std::error_code error;
    if (!fs::is_regular_file(record.image, error) || !fs::is_regular_file(record.metadata, error)) return false;
    nlohmann::json metadata;
    return ProjectWorkspace::ReadJson(record.metadata, metadata) && ProjectWorkspace::String(metadata, "stamp") == record.stamp;
}
bool CommitThumbnail(const ThumbnailRecord& record) {
    return ProjectWorkspace::WriteJson(record.metadata, {{"stamp", record.stamp}});
}
}
