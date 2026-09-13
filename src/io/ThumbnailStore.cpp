#include "io/ThumbnailStore.h"
#include "core/PathUtf8.h"
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
fs::path SceneThumbnailPath(const fs::path& scene) {
    return scene.parent_path() / (scene.stem().wstring() + L".assets") / L"thumbnail.png";
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
