#include "io/AssetRelations.h"
#include "io/ThumbnailStore.h"
#include "core/PathUtf8.h"
#include "core/Log.h"
#include <algorithm>
#include <fstream>
#include <functional>
namespace tg::io {
namespace fs = std::filesystem;
namespace {
bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code ea, eb;
    const auto ca = fs::weakly_canonical(a, ea), cb = fs::weakly_canonical(b, eb);
    return !ea && !eb && _wcsicmp(ca.c_str(), cb.c_str()) == 0;
}
bool IsDocument(const fs::path& path) {
    const auto ext = path.extension().wstring();
    for (const auto* value : {L".tgscene", L".tgmat", L".tgsky", L".tgmodel", L".tgproj", L".mmproj", L".mmmat"})
        if (_wcsicmp(ext.c_str(), value) == 0) return true;
    return false;
}
void Unique(std::vector<fs::path>& paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}
}
AssetRelations InspectAssetRelations(ProjectWorkspace& workspace, const fs::path& target) {
    AssetRelations result;
    result.target = target;
    std::error_code error;
    if (!workspace.Contains(target) || SamePath(target, workspace.Root() / L"project.tgproj") ||
        target.lexically_relative(workspace.Root()).wstring().starts_with(L".") ||
        fs::is_symlink(target, error) || !fs::is_regular_file(target, error)) return result;
    result.modified = fs::last_write_time(target, error);
    if (error) return result;
    result.size = fs::file_size(target, error);
    if (error || !workspace.Scan()) return result;
    result.complete = true;
    nlohmann::json header;
    if (IsDocument(target) && !ProjectWorkspace::ReadJson(target, header)) result.complete = false;
    auto uid = ProjectWorkspace::String(header, "uid");
    const fs::path meta = target.wstring() + L".meta";
    if (fs::exists(meta, error)) {
        if (!workspace.Contains(meta) || fs::is_symlink(meta, error)) result.complete = false;
        else {
            nlohmann::json metadata;
            if (!ProjectWorkspace::ReadJson(meta, metadata)) result.complete = false;
            else uid = ProjectWorkspace::String(metadata, "uid");
            result.companions.push_back(meta);
            const auto modified = fs::last_write_time(meta, error);
            if (error) result.complete = false;
            const auto size = fs::file_size(meta, error);
            if (error) result.complete = false;
            result.companionVersions.push_back(std::to_string(modified.time_since_epoch().count()) + ":" + std::to_string(size));
        }
    }
    const auto references = [&](const nlohmann::json& document, const fs::path& owner, bool outgoing) {
        bool hit = false;
        const auto visit = [&](auto&& self, const nlohmann::json& value) -> void {
            if (value.is_object() && value.contains("uid") && value.contains("path")) {
                const auto reference = workspace.Resolve(value);
                if (outgoing) {
                    if (!reference.empty()) result.related.push_back(reference);
                } else if ((!uid.empty() && ProjectWorkspace::String(value, "uid") == uid) ||
                           (!reference.empty() && SamePath(reference, target))) hit = true;
                return;
            }
            if (value.is_structured()) { for (const auto& child : value) self(self, child); }
            else if (value.is_string()) {
                const auto text = value.get<std::string>();
                if (text.empty()) return;
                const auto path = FromUtf8(text);
                for (const auto& candidate : {owner.parent_path() / path, workspace.Root() / path}) {
                    if (outgoing) {
                        std::error_code existsError;
                        if (fs::is_regular_file(candidate, existsError) && !SamePath(candidate, owner)) result.related.push_back(candidate.lexically_normal());
                    } else if (SamePath(candidate, target)) hit = true;
                }
            }
        };
        visit(visit, document);
        return hit;
    };
    if (header.is_object()) references(header, target, true);
    if (target.extension() == L".tgscene") {
        const auto thumbnail = SceneThumbnailPath(workspace, target);
        if (!thumbnail.empty() && fs::exists(thumbnail, error)) result.related.push_back(thumbnail);
        const auto paint = target.parent_path() / (target.stem().wstring() + L".assets");
        if (fs::exists(paint, error)) result.related.push_back(paint);
    }
    fs::recursive_directory_iterator it(workspace.Root(), fs::directory_options::none, error), end;
    for (; it != end && !error; it.increment(error)) {
        if (it->is_symlink(error)) { it.disable_recursion_pending(); result.complete = false; continue; }
        if (it->is_directory(error)) {
            if (it->path().filename().wstring().starts_with(L".")) it.disable_recursion_pending();
            continue;
        }
        const auto path = it->path();
        if (SamePath(path, target) || !IsDocument(path)) continue;
        nlohmann::json document;
        if (!ProjectWorkspace::ReadJson(path, document)) { result.complete = false; continue; }
        // プロジェクトの旧開始シーンは自動読み込みを廃止済み。
        if (ProjectWorkspace::String(document, "format") == "terrain-graph.workspace") continue;
        if (references(document, path, false)) result.referencers.push_back(path);
    }
    if (error) result.complete = false;
    Unique(result.related); Unique(result.referencers); Unique(result.companions);
    return result;
}
bool RetireAsset(ProjectWorkspace& workspace, const AssetRelations& approved) {
    const auto current = InspectAssetRelations(workspace, approved.target);
    if (!approved.complete || !current.complete || current.modified != approved.modified || current.size != approved.size ||
        current.referencers != approved.referencers || current.related != approved.related || current.companions != approved.companions || current.companionVersions != approved.companionVersions) {
        TG_LOG_WARN("削除対象または参照関係が変わりました。もう一度確認してください");
        return false;
    }
    const auto directory = workspace.UniquePath(workspace.Root() / L".terrain-graph" / L"trash", "DeletedAsset", "");
    if (directory.empty() || !workspace.Contains(directory)) return false;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return false;
    auto files = current.companions;
    files.insert(files.begin(), current.target);
    nlohmann::json manifest;
    manifest["files"] = nlohmann::json::array();
    for (const auto& file : files) {
        if (!workspace.Contains(file) || !workspace.Contains(directory / file.filename())) return false;
        manifest["files"].push_back({{"original", ToUtf8Portable(file.lexically_relative(workspace.Root()))}, {"stored", ToUtf8Portable(file.filename())}});
    }
    if (!ProjectWorkspace::WriteJson(directory / L"restore.json", manifest)) return false;
    size_t moved = 0;
    for (; moved < files.size(); ++moved) {
        fs::rename(files[moved], directory / files[moved].filename(), error);
        if (error) break;
    }
    if (error) {
        while (moved > 0) {
            --moved;
            std::error_code rollback;
            fs::rename(directory / files[moved].filename(), files[moved], rollback);
            if (rollback) TG_LOG_ERROR("退避ファイルを元に戻せません: %s", ToUtf8Display(directory).c_str());
        }
        return false;
    }
    workspace.Scan();
    TG_LOG_INFO("ファイルを退避しました: %s", ToUtf8Display(directory).c_str());
    return true;
}
}
