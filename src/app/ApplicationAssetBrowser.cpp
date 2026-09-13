#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Shell.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <algorithm>
#include <functional>

namespace tg {
namespace fs = std::filesystem;
namespace {
std::string Extension(const fs::path& path) {
    auto ext = ToUtf8Portable(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return ext;
}
bool IsImage(const std::string& ext) {
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".exr" ||
           ext == ".tga" || ext == ".bmp";
}
// サムネイル枠の中央へ、タブ付きフォルダの輪郭だけを描く。
void DrawFolderIcon(const ImVec2& min, const ImVec2& max) {
    const float size = std::min(max.x - min.x, max.y - min.y);
    const float left = (min.x + max.x) * 0.5f - size * 0.34f;
    const float top = (min.y + max.y) * 0.5f - size * 0.23f;
    const auto point = [&](float x, float y) { return ImVec2(left + size * x, top + size * y); };
    auto* draw = ImGui::GetWindowDrawList();
    // 閉じる位置は上辺の途中に置き、終点と始点の重複による線の歪みを避ける。
    draw->PathLineTo(point(0.12f, 0.0f));
    draw->PathLineTo(point(0.23f, 0.0f));
    draw->PathLineTo(point(0.31f, 0.08f));
    draw->PathLineTo(point(0.64f, 0.08f));
    draw->PathBezierCubicCurveTo(point(0.67f, 0.08f), point(0.68f, 0.09f), point(0.68f, 0.12f));
    draw->PathLineTo(point(0.68f, 0.44f));
    draw->PathBezierCubicCurveTo(point(0.68f, 0.47f), point(0.67f, 0.48f), point(0.64f, 0.48f));
    draw->PathLineTo(point(0.04f, 0.48f));
    draw->PathBezierCubicCurveTo(point(0.01f, 0.48f), point(0.0f, 0.47f), point(0.0f, 0.44f));
    draw->PathLineTo(point(0.0f, 0.04f));
    draw->PathBezierCubicCurveTo(point(0.0f, 0.01f), point(0.01f, 0.0f), point(0.04f, 0.0f));
    const auto color = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    const float thickness = size / 42.0f;
    draw->PathStroke(color, ImDrawFlags_Closed, thickness);
    draw->AddLine(point(0.0f, 0.15f), point(0.68f, 0.15f), color, thickness);
}
}
bool Application::IsAssetLoaded(const fs::path& path) const {
    const auto matches = [&](const fs::path& candidate) {
        if (candidate.empty()) return false;
        std::error_code ea, eb;
        const auto a = fs::weakly_canonical(candidate, ea), b = fs::weakly_canonical(path, eb);
        return !ea && !eb && _wcsicmp(a.c_str(), b.c_str()) == 0;
    };
    if (matches(m_projectPath)) return true;
    if (!m_projectPath.empty()) {
        const auto paintDirectory = m_projectPath.parent_path() / (m_projectPath.stem().wstring() + L".assets");
        for (auto parent = path.parent_path(); !parent.empty();) {
            std::error_code error;
            if (fs::equivalent(parent, paintDirectory, error)) return true;
            const auto next = parent.parent_path();
            if (next == parent) break;
            parent = next;
        }
    }
    for (const auto& a : m_textureLibrary.Entries()) if (matches(a.path)) return true;
    for (const auto& a : m_materialLibrary.Entries()) if (matches(a.assetPath)) return true;
    for (const auto& a : m_skyLibrary.Entries()) if (matches(a.assetPath) || matches(a.sky.hdriPath)) return true;
    for (const auto& a : m_models) if (matches(a.assetPath) || matches(a.path)) return true;
    return false;
}
ImTextureID Application::AssetThumbnailHandle(const fs::path& path) {
    for (const auto& a : m_textureLibrary.Entries())
        if (a.path.lexically_normal() == path.lexically_normal()) return static_cast<ImTextureID>(a.PreviewHandle().ptr);
    for (const auto& a : m_materialLibrary.Entries())
        if (a.assetPath == path) return static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr);
    for (const auto& a : m_skyLibrary.Entries())
        if (a.assetPath == path) return static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr);
    for (const auto& a : m_models)
        if (a.assetPath == path || a.path == path) {
            const auto found = m_modelPreviews.find(a.id);
            return found != m_modelPreviews.end() && found->second->HasOutput()
                       ? static_cast<ImTextureID>(found->second->OutputHandle().ptr) : ImTextureID{};
        }
    return static_cast<ImTextureID>(m_assetThumbnails.Request(path).ptr);
}

void Application::DrawAssetDeleteDialog() {
    if (m_assetDeleteDialog && !ImGui::IsPopupOpen("アセットファイルの削除")) ImGui::OpenPopup("アセットファイルの削除");
    if (!ImGui::BeginPopupModal("アセットファイルの削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    const auto& report = m_assetDeleteRelations;
    // 名前だけでは見分けにくいので、対象と関連ファイルにはサムネイルを添える。
    // 絵の無いもの（.meta やフォルダ）も枠だけ出して行の高さを揃える。
    const auto row = [&](const fs::path& path, float size, bool fullPath) {
        ui::ThumbnailImage(AssetThumbnailHandle(path), size);
        if (!AssetThumbnailCache::Supports(path) || m_assetThumbnails.Failed(path)) {
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            std::error_code error;
            const char* type = fs::is_directory(path, error) ? "フォルダ" : path.extension() == L".meta" ? "meta" : "ファイル";
            const auto text = ImGui::CalcTextSize(type);
            if (text.x < size - ui::Scaled(4))
                ImGui::GetWindowDrawList()->AddText(ImVec2((min.x + max.x - text.x) * 0.5f, (min.y + max.y - text.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), type);
        }
        ImGui::SameLine();
        const auto label = ToUtf8Display(fullPath ? path : path.lexically_relative(m_workspace.Root()));
        // 文字は箱の上下中央へ。
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (size - ImGui::GetTextLineHeight()) * 0.5f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ui::Scaled(520) - size);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopTextWrapPos();
    };
    row(report.target, ui::Scaled(64), true);
    ui::HintText("元ファイルと付随する.metaを、ルート内の退避フォルダへ移します。");
    const auto list = [&](const char* title, const std::vector<fs::path>& paths) {
        if (paths.empty()) return;
        ImGui::Separator(); ImGui::TextUnformatted(title);
        const float thumb = ui::Scaled(36);
        const float rowHeight = thumb + ImGui::GetStyle().ItemSpacing.y;
        if (ImGui::BeginChild(title, ImVec2(ui::Scaled(560), std::min(float(paths.size()), 4.0f) * rowHeight + ui::Scaled(12)), ImGuiChildFlags_Borders))
            for (const auto& path : paths) row(path, thumb, false);
        ImGui::EndChild();
    };
    list("一緒に退避するファイル", report.companions);
    list("直接の参照元（削除すると参照切れになります）", report.referencers);
    if (!report.referencers.empty() && io::KindOfAsset(report.target) != io::AssetKind::Other) {
        // 参照切れにしない道。同じ種類のアセットを選ぶと、参照元を書き換えてから退避する。
        ImGui::TextUnformatted("代わりに割り当てる");
        if (m_assetReplacement.empty()) {
            ImGui::TextDisabled("割り当てない（参照切れのまま）");
        } else {
            row(m_assetReplacement, ui::Scaled(36), false);
        }
        if (ui::Button("選ぶ…")) {
            m_assetPickerOpen = true; m_assetPickerRefresh = true;
            m_assetPickerSelection = m_assetReplacement; m_assetPickerFilter[0] = '\0';
        }
        if (!m_assetReplacement.empty()) {
            ImGui::SameLine();
            if (ui::Button("解除")) m_assetReplacement.clear();
        }
        DrawAssetPicker();
    }
    list("関連ファイル（削除せず残します）", report.related);
    const bool loaded = IsAssetLoaded(report.target);
    if (!report.complete) ui::HintText("参照関係をすべて確認できませんでした。読めないファイルやリンクを確認してください。");
    if (loaded) ui::HintText("現在のシーンに読み込まれています。新規シーンなどへ切り替えてから削除してください。");
    if (!m_assetDeleteQueue.empty())
        ImGui::TextDisabled("このあと %zu 件を続けて確認します", m_assetDeleteQueue.size());
    ImGui::Separator();
    ImGui::BeginDisabled(!report.complete || loaded);
    if (ImGui::Button("削除する")) {
        m_pendingAssetDelete = true; m_assetDeleteDialog = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    const char* skipLabel = m_assetDeleteQueue.empty() ? "キャンセル" : "スキップ";
    // ピッカーを重ねている間の Esc はピッカーが受ける。
    const bool escape = !m_assetPickerOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    if (ImGui::Button(skipLabel) || escape) {
        m_assetDeleteDialog = false; ImGui::CloseCurrentPopup();
        // Esc はまとめて止める。「スキップ」はこの 1 件を飛ばして次へ進む。
        if (escape || m_assetDeleteQueue.empty()) m_assetDeleteQueue.clear();
        else { m_pendingAssetDeleteInspect = m_assetDeleteQueue.front(); m_assetDeleteQueue.erase(m_assetDeleteQueue.begin()); }
    }
    ImGui::EndPopup();
}

void Application::DrawAssetRenameDialog() {
    if (m_assetRenameDialog && !ImGui::IsPopupOpen("名前の変更")) ImGui::OpenPopup("名前の変更");
    if (!ImGui::BeginPopupModal("名前の変更", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::error_code error;
    const bool folder = fs::is_directory(m_assetRenameTarget, error);
    const auto extension = folder ? fs::path{} : m_assetRenameTarget.extension();
    ImGui::TextDisabled("%s", ToUtf8Display(m_assetRenameTarget.lexically_relative(m_workspace.Root())).c_str());
    bool confirmed = false;
    if (ui::BeginPropertyTable("assetRenameRows")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ui::PropertyTextInput("名前", m_assetRenameBuffer, sizeof(m_assetRenameBuffer));
        // 入力欄で Enter を押したら確定。拡張子は変えられないので隣に添える。
        if (ImGui::IsItemDeactivatedAfterEdit() && ImGui::IsKeyPressed(ImGuiKey_Enter, false)) confirmed = true;
        if (!extension.empty()) ui::PropertyValue("拡張子", "%s", ToUtf8Display(extension).c_str());
        ui::EndPropertyTable();
    }
    if (folder) ui::HintText("フォルダ内のファイルの参照はIDで解決されるので、改名しても切れません。");
    ImGui::Separator();
    const bool empty = m_assetRenameBuffer[0] == '\0';
    ImGui::BeginDisabled(empty);
    if (ImGui::Button("変更する") || (confirmed && !empty)) {
        m_pendingAssetRename = m_assetRenameTarget;
        m_pendingAssetRenameName = std::string(m_assetRenameBuffer) + ToUtf8Portable(extension);
        m_assetRenameDialog = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ImGui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_assetRenameDialog = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::CollectAssetPickerCandidates() {
    m_assetPickerCandidates.clear();
    const auto& target = m_assetDeleteRelations.target;
    const auto kind = io::KindOfAsset(target);
    std::error_code error;
    const auto consider = [&](const fs::path& path) {
        if (io::KindOfAsset(path) != kind || path.lexically_normal() == target.lexically_normal()) return;
        if (path.filename().wstring().starts_with(L".")) return;
        m_assetPickerCandidates.push_back(path);
    };
    if (m_assetPickerSameFolder) {
        fs::directory_iterator it(target.parent_path(), fs::directory_options::skip_permission_denied, error), end;
        for (; it != end && !error; it.increment(error))
            if (it->is_regular_file(error) && !it->is_symlink(error)) consider(it->path());
    } else {
        fs::recursive_directory_iterator it(m_workspace.Root(), fs::directory_options::skip_permission_denied, error), end;
        for (; it != end && !error; it.increment(error)) {
            if (it->is_symlink(error)) { it.disable_recursion_pending(); continue; }
            if (it->is_directory(error)) {
                if (it->path().filename().wstring().starts_with(L".")) it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(error)) consider(it->path());
        }
    }
    std::sort(m_assetPickerCandidates.begin(), m_assetPickerCandidates.end(), [](const auto& a, const auto& b) {
        const int byName = _wcsicmp(a.filename().c_str(), b.filename().c_str());
        return byName != 0 ? byName < 0 : a < b;
    });
    m_assetPickerRefresh = false;
}

void Application::DrawAssetPicker() {
    if (m_assetPickerOpen && !ImGui::IsPopupOpen("代わりのアセットを選ぶ")) ImGui::OpenPopup("代わりのアセットを選ぶ");
    if (!ImGui::BeginPopupModal("代わりのアセットを選ぶ", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (m_assetPickerRefresh) CollectAssetPickerCandidates();
    if (ui::BeginPropertyTable("assetPickerRows")) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ui::PropertyTextInput("名前で絞る", m_assetPickerFilter, sizeof(m_assetPickerFilter), "部分一致。大文字と小文字は区別しません");
        if (ui::PropertyBool("同じフォルダだけ", &m_assetPickerSameFolder, true, "外すとルート全体から探します")) m_assetPickerRefresh = true;
        ui::EndPropertyTable();
    }
    // 絞り込みは小文字にして部分一致。
    std::string filter = m_assetPickerFilter;
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    std::vector<const fs::path*> shown;
    for (const auto& path : m_assetPickerCandidates) {
        std::string name = ToUtf8Display(path.filename());
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (filter.empty() || name.find(filter) != std::string::npos) shown.push_back(&path);
    }
    const float size = ui::Scaled(84);
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const int columns = 6;
    const float width = columns * (size + spacing) + ui::Scaled(16);
    bool chosen = false;
    if (ImGui::BeginChild("candidates", ImVec2(width, ui::Scaled(3.0f) * (size + ImGui::GetTextLineHeightWithSpacing() * 2.0f)), ImGuiChildFlags_Borders)) {
        if (shown.empty()) ui::HintText(m_assetPickerCandidates.empty() ? "同じ種類のアセットがありません" : "一致するものがありません");
        int index = 0;
        for (const auto* pathPtr : shown) {
            const auto& path = *pathPtr;
            ImGui::PushID(ToUtf8Portable(path).c_str()); ImGui::BeginGroup();
            const ImTextureID handle = ImGui::IsRectVisible(ImVec2(size, size)) ? AssetThumbnailHandle(path) : ImTextureID{};
            const auto thumb = ui::ThumbnailButton("##candidate", handle, size, m_assetPickerSelection == path);
            if (!handle && m_assetThumbnails.Failed(path)) ui::MissingBadge(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (thumb.clicked) m_assetPickerSelection = path;
            if (thumb.doubleClicked) { m_assetPickerSelection = path; chosen = true; }
            if (thumb.hovered) ImGui::SetTooltip("%s", ToUtf8Display(path.lexically_relative(m_workspace.Root())).c_str());
            ui::GridCaption(ToUtf8Display(path.filename()).c_str(), size);
            ImGui::EndGroup(); ImGui::PopID();
            if (++index % columns && index < int(shown.size())) ImGui::SameLine();
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("%zu 件", shown.size());
    ImGui::Separator();
    ImGui::BeginDisabled(m_assetPickerSelection.empty());
    if (ui::Button("決定") || chosen) {
        m_assetReplacement = m_assetPickerSelection;
        m_assetPickerOpen = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled(); ImGui::SameLine();
    if (ui::Button("キャンセル") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        m_assetPickerOpen = false; ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Application::OpenAssetRename(const fs::path& path) {
    if (path.empty() || path.filename() == L"project.tgproj") return;
    std::error_code error;
    const auto name = fs::is_directory(path, error) ? path.filename() : path.stem();
    std::snprintf(m_assetRenameBuffer, sizeof(m_assetRenameBuffer), "%s", ToUtf8Portable(name).c_str());
    m_assetRenameTarget = path;
    m_assetRenameDialog = true;
}

bool Application::IsAssetSelected(const fs::path& path) const {
    return std::find(m_selectedAssets.begin(), m_selectedAssets.end(), path) != m_selectedAssets.end();
}

void Application::SelectAsset(const fs::path& path, bool toggle, bool range) {
    const auto indexOf = [&](const fs::path& target) -> int {
        for (size_t i = 0; i < m_assetEntries.size(); ++i) if (m_assetEntries[i].path() == target) return int(i);
        return -1;
    };
    const int anchor = indexOf(m_assetSelectionAnchor), current = indexOf(path);
    if (range && anchor >= 0 && current >= 0) {
        if (!toggle) m_selectedAssets.clear();
        for (int i = std::min(anchor, current); i <= std::max(anchor, current); ++i)
            if (!IsAssetSelected(m_assetEntries[i].path())) m_selectedAssets.push_back(m_assetEntries[i].path());
        return;
    }
    if (toggle) {
        const auto found = std::find(m_selectedAssets.begin(), m_selectedAssets.end(), path);
        if (found != m_selectedAssets.end()) m_selectedAssets.erase(found);
        else m_selectedAssets.push_back(path);
    } else if (!IsAssetSelected(path)) {
        // 選択済みを普通にクリックしたときは残す（複数をそのままドラッグできるように）。
        m_selectedAssets.assign(1, path);
    }
    m_assetSelectionAnchor = path;
}

void Application::QueueAssetDelete() {
    m_assetDeleteQueue.clear();
    for (const auto& path : m_selectedAssets) {
        std::error_code error;
        if (!fs::is_directory(path, error) && path.filename() != L"project.tgproj") m_assetDeleteQueue.push_back(path);
    }
    if (m_assetDeleteQueue.empty()) return;
    m_pendingAssetDeleteInspect = m_assetDeleteQueue.front();
    m_assetDeleteQueue.erase(m_assetDeleteQueue.begin());
}

void Application::RelinkAssetPaths(const fs::path& from, const fs::path& to) {
    const auto source = from.lexically_normal();
    const auto remap = [&](fs::path& path) {
        if (path.empty()) return;
        const auto relative = path.lexically_normal().lexically_relative(source);
        if (relative.empty() || *relative.begin() == L"..") return;
        path = relative == L"." ? to : to / relative;
    };
    for (const auto& a : m_textureLibrary.Entries()) remap(m_textureLibrary.FindMutable(a.id)->path);
    for (const auto& a : m_materialLibrary.Entries()) remap(m_materialLibrary.FindMutable(a.id)->assetPath);
    for (const auto& a : m_skyLibrary.Entries()) {
        auto* sky = m_skyLibrary.FindMutable(a.id);
        remap(sky->assetPath); remap(sky->sky.hdriPath);
    }
    for (auto& a : m_models) { remap(a.assetPath); remap(a.path); }
    const auto previousProject = m_projectPath;
    remap(m_projectPath);
    if (m_projectPath != previousProject) {
        m_recentProjects.Remove(m_workspace.Root(), previousProject);
        m_recentProjects.Add(m_workspace.Root(), m_projectPath);
        UpdateWindowTitle();
    } else {
        std::error_code error;
        if (!fs::is_directory(to, error)) m_recentProjects.Remove(m_workspace.Root(), from);
    }
    for (auto& path : m_selectedAssets) remap(path);
    remap(m_assetSelectionAnchor);
    remap(m_assetDirectory);
    m_assetThumbnails.Invalidate();
}

void Application::ResumeSceneSwitch() {
    m_pendingRoot = std::move(m_deferredRoot); m_deferredRoot.clear();
    m_pendingProjectOpen = std::move(m_deferredScene); m_deferredScene.clear();
    m_pendingProjectNew = m_deferredNew; m_deferredNew = false;
    m_allowSceneSwitch = true;
    m_sceneSwitchDialog = false;
}
void Application::DrawSceneSwitchDialog() {
    if (m_sceneSwitchDialog && !ImGui::IsPopupOpen("シーンの切り替え")) ImGui::OpenPopup("シーンの切り替え");
    if (!ImGui::BeginPopupModal("シーンの切り替え", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ui::HintText("現在のシーンと共有アセットを保存してから切り替えますか？");
    if (ImGui::Button("保存して切り替え")) {
        RequestSaveProject(false);
        if (!m_pendingProjectSave.empty()) {
            m_saveThenSwitch = true;
            m_sceneSwitchDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("保存せず切り替え")) { ResumeSceneSwitch(); ImGui::CloseCurrentPopup(); }
    ImGui::SameLine();
    if (ImGui::Button("キャンセル")) {
        m_sceneSwitchDialog = false; m_deferredRoot.clear(); m_deferredScene.clear(); m_deferredNew = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
void Application::RefreshAssetBrowser() {
    m_assetEntries.clear();
    std::error_code error;
    fs::directory_iterator it(m_assetDirectory, fs::directory_options::skip_permission_denied, error), end;
    for (; it != end && !error; it.increment(error)) {
        const auto& entry = *it;
        if (entry.is_symlink(error) || entry.path().filename().wstring().starts_with(L".")) continue;
        const auto ext = Extension(entry.path());
        if (!entry.is_directory(error) && !IsImage(ext) && ext != ".fbx" && ext != ".hdr" &&
            ext != ".tgmat" && ext != ".tgsky" && ext != ".tgmodel" &&
            ext != ".tgscene" && ext != ".tgproj" && ext != ".mmproj") continue;
        m_assetEntries.push_back(entry);
    }
    std::sort(m_assetEntries.begin(), m_assetEntries.end(), [](const auto& a, const auto& b) {
        std::error_code errorA, errorB;
        const bool directoryA = a.is_directory(errorA), directoryB = b.is_directory(errorB);
        if (directoryA != directoryB) return directoryA;
        return a.path().filename() < b.path().filename();
    });
    m_assetRefresh = false;
}

void Application::ProcessAssetWork() {
    if (m_pendingAssetDelete) {
        m_pendingAssetDelete = false;
        const auto path = m_assetDeleteRelations.target;
        bool ready = !IsAssetLoaded(path);
        // 代わりを選んでいれば、先に参照元を書き換える。参照関係が変わるので確認データを取り直す。
        if (ready && !m_assetReplacement.empty()) {
            ready = io::ReplaceAssetReferences(m_workspace, m_assetDeleteRelations, m_assetReplacement);
            if (ready) m_assetDeleteRelations = io::InspectAssetRelations(m_workspace, path);
            m_assetReplacement.clear();
        }
        if (ready && io::RetireAsset(m_workspace, m_assetDeleteRelations)) {
            m_recentProjects.Remove(m_workspace.Root(), path);
            std::erase(m_selectedAssets, path);
            m_assetRefresh = true; m_assetThumbnails.Invalidate();
            if (!m_assetDeleteQueue.empty()) {
                m_pendingAssetDeleteInspect = m_assetDeleteQueue.front();
                m_assetDeleteQueue.erase(m_assetDeleteQueue.begin());
            }
        } else {
            TG_LOG_WARN("削除できませんでした。対象と参照関係を再確認してください");
            m_pendingAssetDeleteInspect = path;
        }
    }
    if (!m_pendingAssetDeleteInspect.empty()) {
        m_assetDeleteRelations = io::InspectAssetRelations(m_workspace, m_pendingAssetDeleteInspect);
        m_pendingAssetDeleteInspect.clear(); m_assetDeleteDialog = true;
        m_assetReplacement.clear();
    }
    if (!m_pendingAssetMoves.empty()) {
        const auto sources = std::move(m_pendingAssetMoves); m_pendingAssetMoves.clear();
        const auto directory = m_pendingAssetMoveTarget; m_pendingAssetMoveTarget.clear();
        size_t count = 0;
        for (const auto& source : sources) {
            const auto moved = io::MoveAsset(m_workspace, source, directory);
            if (moved.empty() || moved == source) continue;
            // 読み込み済みのアセットは絶対パスを持つので、移動先へ付け替える。
            RelinkAssetPaths(source, moved);
            ++count;
        }
        if (count > 0)
            TG_LOG_INFO("%zu 件を移動しました → %s", count,
                        ToUtf8Display(directory.lexically_relative(m_workspace.Root())).c_str());
        m_assetRefresh = true;
    }
    if (!m_pendingAssetRename.empty()) {
        const auto source = m_pendingAssetRename; m_pendingAssetRename.clear();
        const auto renamed = io::RenameAsset(m_workspace, source, m_pendingAssetRenameName);
        if (!renamed.empty() && renamed != source) {
            RelinkAssetPaths(source, renamed);
            TG_LOG_INFO("名前を変更しました: %s → %s", ToUtf8Display(source.filename()).c_str(),
                        ToUtf8Display(renamed.filename()).c_str());
        }
        m_assetRefresh = true;
    }
    // --projectには従来のシーンだけでなくルートと管理ファイルも渡せる。
    if (!m_pendingProjectOpen.empty()) {
        std::error_code error;
        nlohmann::json project;
        if (fs::is_directory(m_pendingProjectOpen, error)) {
            m_pendingRoot = m_pendingProjectOpen; m_pendingProjectOpen.clear();
        } else if (m_pendingProjectOpen.extension() == L".tgproj" &&
                   io::ProjectWorkspace::ReadJson(m_pendingProjectOpen, project) &&
                   io::ProjectWorkspace::String(project, "format") == "terrain-graph.workspace") {
            m_pendingRoot = m_pendingProjectOpen.parent_path(); m_pendingProjectOpen.clear();
        }
    }
    if (!m_pendingRoot.empty()) {
        const auto root = m_pendingRoot; m_pendingRoot.clear();
        io::ProjectWorkspace next;
        if (next.Open(root)) {
            // ルートの選択とシーンの選択を分ける。履歴から指定されたシーンだけ開く。
            if (!m_pendingProjectOpen.empty() && m_pendingProjectOpen.extension() == L".tgscene") {
                nlohmann::json validation;
                if (!next.ReadScene(m_pendingProjectOpen, validation)) {
                    TG_LOG_ERROR("シーンを読み込めません。現在のプロジェクトを保持します");
                    m_pendingProjectOpen.clear();
                    return;
                }
            }
            m_workspace = std::move(next);
            io::MigrateSceneThumbnails(m_workspace);
            m_assetDirectory = m_workspace.Root();
            m_assetRefresh = true;
            if (!Headless()) m_recentProjects.AddRoot(m_workspace.Root());
            ResetProject(); m_projectPath.clear(); UpdateWindowTitle();
        } else {
            m_pendingProjectOpen.clear();
        }
    }
    io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
                         m_skyLibrary, m_renderer, m_graph, &m_models};
    if (m_pendingAssetsSave) {
        m_pendingAssetsSave = false;
        CommitMaterialEdit();
        if (!io::SaveSharedAssets(m_workspace, refs)) TG_LOG_ERROR("共有アセットを保存できませんでした");
        m_assetRefresh = true;
        m_assetThumbnails.Invalidate();
    }
    if (!m_pendingAssetOpen.empty()) {
        const auto path = m_pendingAssetOpen; m_pendingAssetOpen.clear();
        const auto ext = Extension(path);
        if (ext == ".tgmat" || ext == ".tgsky" || ext == ".tgmodel") {
            nlohmann::json assetHeader;
            if (ext == ".tgmat" && io::ProjectWorkspace::ReadJson(path, assetHeader) &&
                io::ProjectWorkspace::String(assetHeader, "format") == "terrain-graph.material") {
                m_pendingMaterialImport = path;
                return;
            }
            if (io::LoadSharedAsset(m_workspace, path, m_device, m_pipelineCache, refs)) {
                if (ext == ".tgmat") {
                    for (size_t i = 0; i < m_materialLibrary.Entries().size(); ++i)
                        if (m_materialLibrary.Entries()[i].assetPath == path) m_selectedMaterial = static_cast<int>(i);
                    m_showMaterialSphere = true;
                } else if (ext == ".tgsky") m_showSkyPreview = true;
                else {
                    for (const auto& a : m_models) if (a.assetPath == path) m_selectedModel = a.id;
                    m_modelLod = 0; m_showModelPreview = true;
                }
                MarkDocumentChanged();
            } else TG_LOG_ERROR("アセットを開けません: %s", ToUtf8Display(path).c_str());
        } else if (IsImage(ext)) {
            const auto id = m_textureLibrary.Load(m_device, m_pipelineCache, path);
            if (id) {
                for (size_t i = 0; i < m_textureLibrary.Entries().size(); ++i)
                    if (m_textureLibrary.Entries()[i].id == id) m_selectedTexture = static_cast<int>(i);
                m_showTexturePreview = true;
            }
        } else HandleDroppedFiles({path});
        m_assetRefresh = true;
    }
    if (m_assetRefresh) RefreshAssetBrowser();
    m_assetThumbnails.Process(m_device, m_pipelineCache, m_workspace, m_assetDirectory, m_renderer);
}

void Application::AssetFolderDropTarget(const fs::path& directory) {
    if (!ImGui::BeginDragDropTarget()) return;
    // 読み込み済みのテクスチャ / マテリアルは ID で運ばれてくるので、パスへ引き直す。
    // 複数選択のパスは改行区切りで運ばれてくる。
    std::vector<fs::path> sources;
    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPathDragDropType);
        payload != nullptr && payload->DataSize > 0) {
        const std::string text(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
        for (size_t begin = 0; begin < text.size();) {
            const size_t end = std::min(text.find('\n', begin), text.size());
            if (end > begin) sources.push_back(FromUtf8(text.substr(begin, end - begin)));
            begin = end + 1;
        }
    } else if (const ImGuiPayload* texture = ImGui::AcceptDragDropPayload(kTextureDragDropType);
               texture != nullptr && texture->DataSize == sizeof(compositor::TextureId)) {
        if (const auto* entry = m_textureLibrary.Find(*static_cast<const compositor::TextureId*>(texture->Data)))
            sources.push_back(entry->path);
    } else if (const ImGuiPayload* material = ImGui::AcceptDragDropPayload(kMaterialDragDropType);
               material != nullptr && material->DataSize == sizeof(compositor::MaterialAssetId)) {
        if (const auto* asset = m_materialLibrary.Find(*static_cast<const compositor::MaterialAssetId*>(material->Data)))
            sources.push_back(asset->assetPath);
    }
    std::error_code error;
    for (const auto& source : sources) {
        if (!source.empty() && m_workspace.Contains(source) &&
            !fs::equivalent(source.parent_path(), directory, error)) {
            m_pendingAssetMoves.push_back(source); m_pendingAssetMoveTarget = directory;
        }
    }
    ImGui::EndDragDropTarget();
}

void Application::DrawAssetBrowser() {
    if (!ImGui::Begin("アセット")) { ImGui::End(); return; }
    if (ImGui::Button("ルートを開く…")) RequestOpenProject();
    ImGui::SameLine();
    if (ImGui::Button("更新")) { m_assetRefresh = true; m_workspace.Scan(); m_assetThumbnails.Invalidate(); }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ToUtf8Display(m_assetDirectory).c_str());

    const auto available = ImGui::GetContentRegionAvail();
    const float margin = ui::Scaled(ui::kSplitterMargin);
    const float usable = std::max(2.0f, available.x - margin * 2.0f - ui::Scaled(ui::kSplitterGrabWidth));
    const float minimum = std::min(ui::Scaled(120.0f), usable * 0.5f);
    float folderWidth = std::clamp(ui::Scaled(m_settings.Ui().assetFolderWidth), minimum, usable - minimum);
    if (ImGui::BeginChild("folders", ImVec2(folderWidth, 0), ImGuiChildFlags_Borders)) {
        const auto tree = [&](auto&& self, const fs::path& directory, int depth) -> void {
            if (depth > 32) return;
            const auto label = ToUtf8Display(directory.filename());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
            if (directory == m_workspace.Root()) flags |= ImGuiTreeNodeFlags_DefaultOpen;
            if (directory == m_assetDirectory) flags |= ImGuiTreeNodeFlags_Selected;
            ImGui::PushID(ToUtf8Portable(directory).c_str());
            const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                m_assetDirectory = directory; m_assetRefresh = true;
            }
            AssetFolderDropTarget(directory);
            if (open) {
                std::error_code error;
                fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, error), end;
                for (; it != end && !error; it.increment(error))
                    if (it->is_directory(error) && !it->is_symlink(error) &&
                        !it->path().filename().wstring().starts_with(L".")) self(self, it->path(), depth + 1);
                ImGui::TreePop();
            }
            ImGui::PopID();
        };
        tree(tree, m_workspace.Root(), 0);
    }
    ImGui::EndChild(); ImGui::SameLine(0.0f, margin);
    const float previousWidth = folderWidth;
    const bool released = ui::VerticalSplitter("assetFolderSplitter", &folderWidth, minimum, usable - minimum, available.y);
    if (folderWidth != previousWidth) m_settings.Ui().assetFolderWidth = folderWidth / ui::Scaled(1.0f);
    if (released) m_settings.Save();
    ImGui::SameLine(0.0f, margin);
    if (ImGui::BeginChild("contents", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (m_assetDirectory != m_workspace.Root() && ImGui::Button("上のフォルダ")) {
            m_assetDirectory = m_assetDirectory.parent_path(); m_assetRefresh = true;
        }
        if (m_assetEntries.empty()) ui::HintText("右クリックでアセットを作成、またはファイルを読み込みます");
        // 一覧にフォーカスがあるときのキー操作。テキスト入力中やダイアログ表示中は効かせない。
        if ((ImGui::IsWindowFocused() || ImGui::IsWindowHovered()) && !ImGui::GetIO().WantTextInput &&
            !m_assetDeleteDialog && !m_assetRenameDialog && !m_selectedAssets.empty()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) QueueAssetDelete();
            else if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) OpenAssetRename(m_selectedAssets.front());
        }
        const float size = ui::Scaled(84);
        const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x)));
        int index = 0;
        for (const auto& entry : m_assetEntries) {
            const auto path = entry.path();
            const auto ext = Extension(path);
            std::error_code error;
            const bool folder = entry.is_directory(error);
            compositor::TextureId textureId = 0;
            compositor::MaterialAssetId materialId = 0;
            for (const auto& a : m_textureLibrary.Entries()) if (a.path.lexically_normal() == path.lexically_normal()) { textureId = a.id; break; }
            for (const auto& a : m_materialLibrary.Entries()) if (a.assetPath == path) { materialId = a.id; break; }
            // 画面外の分は要求しない（生成は 1 フレームに 1 枚なので、見えているものを優先する）。
            const ImTextureID handle = (!folder && ImGui::IsRectVisible(ImVec2(size, size))) ? AssetThumbnailHandle(path) : ImTextureID{};
            ImGui::PushID(ToUtf8Portable(path).c_str()); ImGui::BeginGroup();
            const auto thumb = ui::ThumbnailButton("##asset", handle, size, IsAssetSelected(path));
            if (folder) {
                DrawFolderIcon(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            } else if (!handle) {
                const char* type = ext == ".tgscene" ? "シーン" : ext == ".tgmat" ? "マテリアル" :
                    ext == ".tgsky" ? "天球" : ext == ".tgmodel" || ext == ".fbx" ? "モデル" : IsImage(ext) ? "画像" : "ファイル";
                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const auto text = ImGui::CalcTextSize(type);
                ImGui::GetWindowDrawList()->AddText(ImVec2((min.x + max.x - text.x) * 0.5f, (min.y + max.y - text.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), type);
            }
            if (!handle && m_assetThumbnails.Failed(path))
                ui::MissingBadge(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (thumb.clicked) SelectAsset(path, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
            if (thumb.doubleClicked || (thumb.clicked && (ext == ".tgmodel" || ext == ".fbx"))) {
                if (folder) { m_assetDirectory = path; m_assetRefresh = true; }
                else m_pendingAssetOpen = path;
            }
            // ImGui のペイロードは 1 つしか持てない。ノードやマップ欄へ割り当てられるものは
            // 従来どおり ID を積み、それ以外のファイルはパスを積んでフォルダへ移せるようにする。
            const bool movable = !folder && path.filename() != L"project.tgproj";
            // 複数選んでいるうちの 1 つを掴んだら、選択全部をパスの一覧で運ぶ。
            std::vector<fs::path> dragged;
            if (movable && IsAssetSelected(path) && m_selectedAssets.size() > 1)
                for (const auto& selected : m_selectedAssets) {
                    std::error_code selectedError;
                    if (!fs::is_directory(selected, selectedError) && selected.filename() != L"project.tgproj")
                        dragged.push_back(selected);
                }
            if ((textureId || materialId || movable) && ImGui::BeginDragDropSource()) {
                if (dragged.size() > 1) {
                    std::string utf8;
                    for (const auto& selected : dragged) utf8 += ToUtf8Portable(selected) + "\n";
                    ImGui::SetDragDropPayload(kAssetPathDragDropType, utf8.data(), utf8.size());
                    ImGui::Text("%zu 件", dragged.size());
                } else {
                    if (materialId) ImGui::SetDragDropPayload(kMaterialDragDropType, &materialId, sizeof(materialId));
                    else if (textureId) ImGui::SetDragDropPayload(kTextureDragDropType, &textureId, sizeof(textureId));
                    else {
                        const auto utf8 = ToUtf8Portable(path);
                        ImGui::SetDragDropPayload(kAssetPathDragDropType, utf8.data(), utf8.size());
                    }
                    ImGui::TextUnformatted(ToUtf8Display(path.filename()).c_str());
                }
                ImGui::EndDragDropSource();
            }
            if (folder) AssetFolderDropTarget(path);
            // ドラッグ中は移動先を示すツールチップの邪魔になるので出さない。
            if (thumb.hovered && ImGui::GetDragDropPayload() == nullptr)
                ImGui::SetTooltip("%s\nダブルクリックで開く\nCtrl / Shift + クリックで複数選択", ToUtf8Display(path).c_str());
            if (ImGui::BeginPopupContextItem("assetMenu")) {
                if (!IsAssetSelected(path)) SelectAsset(path, false, false);
                if (ImGui::MenuItem("開く")) {
                    if (folder) { m_assetDirectory = path; m_assetRefresh = true; }
                    else m_pendingAssetOpen = path;
                }
                if (ImGui::MenuItem("エクスプローラで表示")) RevealFileInExplorer(path);
                if (path.filename() != L"project.tgproj" && ImGui::MenuItem("名前を変更…", "F2")) OpenAssetRename(path);
                if (!folder && path.filename() != L"project.tgproj" && ImGui::MenuItem("削除…", "Del")) QueueAssetDelete();
                ImGui::EndPopup();
            }
            ui::GridCaption(ToUtf8Display(path.filename()).c_str(), size);
            ImGui::EndGroup(); ImGui::PopID();
            if (++index % columns && index < int(m_assetEntries.size())) ImGui::SameLine();
        }
        if (ImGui::BeginPopupContextWindow("createAsset", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("フォルダを作成")) {
                const auto path = m_workspace.UniquePath(m_assetDirectory, "NewFolder", "");
                std::error_code error;
                if (!path.empty()) fs::create_directory(path, error);
                if (error) TG_LOG_ERROR("フォルダを作成できませんでした");
                m_assetRefresh = true;
            }
            if (ImGui::MenuItem("マテリアルを作成")) {
                const auto id = m_materialLibrary.Add("新規マテリアル");
                auto* asset = m_materialLibrary.FindMutable(id);
                asset->assetPath = m_workspace.UniquePath(m_assetDirectory, asset->name, ".tgmat");
                m_selectedMaterial = static_cast<int>(m_materialLibrary.Entries().size()) - 1;
                m_showMaterialSphere = true; m_pendingAssetsSave = true; MarkDocumentChanged();
            }
            if (ImGui::MenuItem("天球を作成")) {
                const auto id = m_skyLibrary.Add("新規天球");
                auto* asset = m_skyLibrary.FindMutable(id);
                asset->assetPath = m_workspace.UniquePath(m_assetDirectory, asset->name, ".tgsky");
                m_skyLibrary.SetActive(id); m_showSkyPreview = true; m_pendingAssetsSave = true;
            }
            if (ImGui::MenuItem("ファイルを読み込む…")) {
                const auto paths = ShowOpenFilesDialog(L"アセットを読み込む", {{L"画像 / モデル / マテリアル", L"*.png;*.jpg;*.jpeg;*.tga;*.bmp;*.exr;*.hdr;*.fbx;*.tgmat"}});
                HandleDroppedFiles(paths);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild(); ImGui::End();
}
}  // namespace tg
