#include "io/AssetRelations.h"
#include "io/ThumbnailStore.h"
#include "core/PathUtf8.h"
#include "core/Log.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <cwctype>
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
// 大文字・小文字だけが違う改名か（Windows では同じ場所を指す）。
bool CaseOnlyRename(const fs::path& from, const fs::path& to) {
    return SamePath(from, to) && from.filename().wstring() != to.filename().wstring();
}
// 改名する。大文字・小文字だけの改名は一時的な名前を経由する（そのままでは
// 同じ名前への改名として扱われ、綴りが変わらないことがある）。
void RenamePath(const fs::path& from, const fs::path& to, std::error_code& error) {
    if (!CaseOnlyRename(from, to)) {
        fs::rename(from, to, error);
        return;
    }
    fs::path temporary = from;
    temporary += L".renaming";
    for (int i = 1; fs::exists(temporary, error); ++i) {
        temporary = from;
        temporary += L".renaming" + std::to_wstring(i);
    }
    fs::rename(from, temporary, error);
    if (error) return;
    fs::rename(temporary, to, error);
    if (error) {
        std::error_code rollback;
        fs::rename(temporary, from, rollback);
    }
}
bool IsDocument(const fs::path& path) {
    const auto ext = path.extension().wstring();
    for (const auto* value : {L".tgterrain", L".tgcloud", L".tgatmosphere", L".tgscene", L".tgmat", L".tglayer", L".tgsky", L".tgmodel", L".tgproj", L".mmproj", L".mmmat"})
        if (_wcsicmp(ext.c_str(), value) == 0) return true;
    return false;
}
void Unique(std::vector<fs::path>& paths) {
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}
// 確認したときの状態から変わっていないか。退避も参照の付け替えも、これが通るときだけ行う。
bool Unchanged(const AssetRelations& approved, const AssetRelations& current) {
    return approved.complete && current.complete && current.modified == approved.modified && current.size == approved.size &&
           current.referencers == approved.referencers && current.related == approved.related &&
           current.companions == approved.companions && current.companionVersions == approved.companionVersions;
}
}
bool RemoveEmptyAssetFolder(ProjectWorkspace& workspace, const fs::path& target) {
    std::error_code error;
    const auto attributes = GetFileAttributesW(target.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    const auto resolved = fs::weakly_canonical(target, error);
    if (error || !workspace.Contains(resolved) || SamePath(resolved, workspace.Root())) return false;
    for (const auto& part : resolved.lexically_relative(workspace.Root()))
        if (part.wstring().starts_with(L".")) return false;
    if (fs::is_symlink(target, error) || error || !fs::is_directory(target, error) || error) return false;
    if (!fs::is_empty(target, error) || error) {
        TG_LOG_WARN("空でないフォルダは削除できません: %s", ToUtf8Display(target).c_str());
        return false;
    }
    // 確認後にファイルが追加されても、非再帰のremoveなら削除されない。
    if (!fs::remove(target, error) || error) return false;
    TG_LOG_INFO("空のフォルダを削除しました: %s", ToUtf8Display(target).c_str());
    return true;
}

AssetKind KindOfAsset(const fs::path& path) {
    auto ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".tga" || ext == L".bmp" || ext == L".exr" || ext == L".hdr")
        return AssetKind::Image;
    if (ext == L".tgmat") return AssetKind::Material;
    if (ext == L".tglayer") return AssetKind::LayerMaterial;
    if (ext == L".tgsky") return AssetKind::Sky;
    if (ext == L".tgmodel" || ext == L".fbx") return AssetKind::Model;
    return AssetKind::Other;
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
    result.uid = uid;
    if (header.is_object()) references(header, target, true);
    if (target.extension() == L".tgscene" || target.extension() == L".tgterrain" || target.extension() == L".tgcloud") {
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
    if (!Unchanged(approved, current)) {
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
bool ReplaceAssetReferences(ProjectWorkspace& workspace, const AssetRelations& approved, const fs::path& replacement) {
    const auto current = InspectAssetRelations(workspace, approved.target);
    if (!Unchanged(approved, current)) {
        TG_LOG_WARN("削除対象または参照関係が変わりました。もう一度確認してください");
        return false;
    }
    std::error_code error;
    if (!workspace.Contains(replacement) || !fs::is_regular_file(replacement, error) ||
        SamePath(replacement, approved.target) || KindOfAsset(replacement) != KindOfAsset(approved.target)) {
        TG_LOG_WARN("代わりに割り当てるアセットが不正です: %s", ToUtf8Display(replacement).c_str());
        return false;
    }
    if (current.referencers.empty()) return true;
    // 置き換え先の参照（IDと相対パス）。.meta が無ければここで作られる。
    const nlohmann::json reference = workspace.Reference(replacement);
    if (reference.is_null()) {
        TG_LOG_ERROR("代わりのアセットのIDを用意できません: %s", ToUtf8Display(replacement).c_str());
        return false;
    }
    // 先に全部読んで書き換え、それから書く。読めない文書があれば何も変えない。
    std::vector<std::pair<fs::path, nlohmann::json>> documents;
    for (const auto& owner : current.referencers) {
        nlohmann::json document;
        if (!ProjectWorkspace::ReadJson(owner, document)) {
            TG_LOG_ERROR("参照元を読めません: %s", ToUtf8Display(owner).c_str());
            return false;
        }
        const auto visit = [&](auto&& self, nlohmann::json& value) -> void {
            if (value.is_object() && value.contains("uid") && value.contains("path")) {
                const auto resolved = workspace.Resolve(value);
                if ((!current.uid.empty() && ProjectWorkspace::String(value, "uid") == current.uid) ||
                    (!resolved.empty() && SamePath(resolved, current.target))) value = reference;
                return;
            }
            if (value.is_structured()) { for (auto& child : value) self(self, child); }
            else if (value.is_string()) {
                // 旧形式の素のパス。参照元から見た相対、またはルートからの相対で対象を指していれば差し替える。
                const auto text = value.get<std::string>();
                if (text.empty()) return;
                const auto path = FromUtf8(text);
                if (SamePath(owner.parent_path() / path, current.target))
                    value = ToUtf8Portable(replacement.lexically_relative(owner.parent_path()));
                else if (SamePath(workspace.Root() / path, current.target))
                    value = ToUtf8Portable(replacement.lexically_relative(workspace.Root()));
            }
        };
        visit(visit, document);
        documents.emplace_back(owner, std::move(document));
    }
    for (const auto& [owner, document] : documents) {
        if (!ProjectWorkspace::WriteJson(owner, document)) {
            TG_LOG_ERROR("参照元を書き換えられません: %s", ToUtf8Display(owner).c_str());
            return false;
        }
    }
    workspace.Scan();
    TG_LOG_INFO("%zu 件の参照を %s へ付け替えました", documents.size(), ToUtf8Display(replacement.filename()).c_str());
    return true;
}
namespace {
// 本体を destination へ置き換える。.meta と、シーンならペイントデータも新しい名前へ揃える。
fs::path RelocateAsset(ProjectWorkspace& workspace, const fs::path& target, const fs::path& destination) {
    std::error_code error;
    if (!workspace.Contains(target) || !workspace.Contains(destination) ||
        SamePath(target, workspace.Root() / L"project.tgproj") ||
        target.lexically_relative(workspace.Root()).wstring().starts_with(L".") ||
        destination.lexically_relative(workspace.Root()).wstring().starts_with(L".") ||
        destination.filename().wstring().starts_with(L".") ||
        fs::is_symlink(target, error) || !fs::is_regular_file(target, error) ||
        !fs::is_directory(destination.parent_path(), error)) {
        TG_LOG_WARN("移動できないファイルまたは移動先です");
        return {};
    }
    // 大文字・小文字だけの改名は、同じ場所でも綴りを変える（Windows では「既にある」と見える）。
    const bool caseOnly = CaseOnlyRename(target, destination);
    if (SamePath(target, destination) && !caseOnly) return target;
    // 未保存の素材も移動前にIDを確定し、古いパスを持つ参照から追跡できるようにする。
    // 移動先のサイドカーとの衝突は、ID発行より先に確認する。
    if (!caseOnly && (fs::exists(destination, error) || error ||
                      fs::exists(destination.wstring() + L".meta", error) || error)) return {};
    if (!IsDocument(target) && workspace.Reference(target).is_null()) return {};
    std::vector<std::pair<fs::path, fs::path>> files{{target, destination}};
    const fs::path meta = target.wstring() + L".meta";
    if (fs::exists(meta, error)) files.emplace_back(meta, destination.wstring() + L".meta");
    if (target.extension() == L".tgscene" || target.extension() == L".tgterrain" || target.extension() == L".tgcloud") {
        const auto paint = target.parent_path() / (target.stem().wstring() + L".assets");
        if (fs::is_directory(paint, error))
            files.emplace_back(paint, destination.parent_path() / (destination.stem().wstring() + L".assets"));
    }
    for (const auto& [from, to] : files) {
        if ((fs::exists(to, error) && !SamePath(from, to)) || !workspace.Contains(to)) {
            TG_LOG_WARN("移動先に同じ名前のファイルがあります: %s", ToUtf8Display(to).c_str());
            return {};
        }
    }
    size_t moved = 0;
    for (; moved < files.size(); ++moved) {
        RenamePath(files[moved].first, files[moved].second, error);
        if (error) break;
    }
    if (error) {
        TG_LOG_ERROR("移動できませんでした: %s", ToUtf8Display(files[moved].first).c_str());
        while (moved > 0) {
            --moved;
            std::error_code rollback;
            RenamePath(files[moved].second, files[moved].first, rollback);
            if (rollback) TG_LOG_ERROR("移動したファイルを元に戻せません: %s", ToUtf8Display(files[moved].first).c_str());
        }
        return {};
    }
    workspace.Scan();
    return destination;
}
}
fs::path MoveAsset(ProjectWorkspace& workspace, const fs::path& target, const fs::path& directory) {
    std::error_code error;
    if (!workspace.Contains(directory) || !fs::is_directory(directory, error)) {
        TG_LOG_WARN("移動できないファイルまたは移動先です");
        return {};
    }
    return RelocateAsset(workspace, target, directory / target.filename());
}
fs::path RenameAsset(ProjectWorkspace& workspace, const fs::path& target, const std::string& newName) {
    const fs::path name = FromUtf8(newName);
    if (name.empty() || name.has_parent_path() || name.wstring().starts_with(L".") ||
        name.wstring().find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
        TG_LOG_WARN("使えない名前です: %s", newName.c_str());
        return {};
    }
    const auto destination = target.parent_path() / name;
    std::error_code error;
    if (fs::is_directory(target, error) && !fs::is_symlink(target, error)) {
        if (!workspace.Contains(target) || !workspace.Contains(destination) || SamePath(target, workspace.Root()) ||
            target.lexically_relative(workspace.Root()).wstring().starts_with(L".")) {
            TG_LOG_WARN("このフォルダは改名できません");
            return {};
        }
        const bool caseOnly = CaseOnlyRename(target, destination);
        if (SamePath(target, destination) && !caseOnly) return target;
        if (!caseOnly && fs::exists(destination, error)) {
            TG_LOG_WARN("同じ名前のフォルダがあります: %s", ToUtf8Display(destination).c_str());
            return {};
        }
        RenamePath(target, destination, error);
        if (error) { TG_LOG_ERROR("フォルダを改名できませんでした: %s", ToUtf8Display(target).c_str()); return {}; }
        workspace.Scan();
        return destination;
    }
    return RelocateAsset(workspace, target, destination);
}
}
