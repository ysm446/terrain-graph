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
            nlohmann::json validation;
            if (!next.StartupScene().empty() && !next.ReadScene(next.StartupScene(), validation)) {
                TG_LOG_ERROR("開始シーンを読み込めません。現在のプロジェクトを保持します");
                return;
            }
            m_workspace = std::move(next);
            m_assetDirectory = m_workspace.Root();
            m_assetRefresh = true;
            const auto scene = m_workspace.StartupScene();
            if (!scene.empty()) m_pendingProjectOpen = scene;
            else { ResetProject(); m_projectPath.clear(); UpdateWindowTitle(); }
        }
    }
    io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
                         m_skyLibrary, m_renderer, m_graph, &m_models};
    if (m_pendingAssetsSave) {
        m_pendingAssetsSave = false;
        CommitMaterialEdit();
        if (!io::SaveSharedAssets(m_workspace, refs)) TG_LOG_ERROR("共有アセットを保存できませんでした");
        m_assetRefresh = true;
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
}

void Application::DrawAssetBrowser() {
    if (!ImGui::Begin("アセット")) { ImGui::End(); return; }
    if (ImGui::Button("ルートを開く…")) RequestOpenProject();
    ImGui::SameLine();
    if (ImGui::Button("更新")) { m_assetRefresh = true; m_workspace.Scan(); }
    ImGui::SameLine();
    if (ImGui::Button("アセットを保存")) m_pendingAssetsSave = true;
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ToUtf8Display(m_assetDirectory).c_str());

    if (ImGui::BeginChild("folders", ImVec2(ui::Scaled(190), 0), ImGuiChildFlags_Borders)) {
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
    ImGui::EndChild(); ImGui::SameLine();
    if (ImGui::BeginChild("contents", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (m_assetDirectory != m_workspace.Root() && ImGui::Button("上のフォルダ")) {
            m_assetDirectory = m_assetDirectory.parent_path(); m_assetRefresh = true;
        }
        if (m_assetEntries.empty()) ui::HintText("右クリックでアセットを作成、またはファイルを読み込みます");
        const float size = ui::Scaled(84);
        const int columns = std::max(1, int(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x)));
        int index = 0;
        for (const auto& entry : m_assetEntries) {
            const auto path = entry.path();
            const auto ext = Extension(path);
            std::error_code error;
            const bool folder = entry.is_directory(error);
            ImTextureID handle = 0;
            compositor::TextureId textureId = 0;
            compositor::MaterialAssetId materialId = 0;
            for (const auto& a : m_textureLibrary.Entries()) if (a.path.lexically_normal() == path.lexically_normal()) {
                handle = static_cast<ImTextureID>(a.PreviewHandle().ptr); textureId = a.id; break;
            }
            for (const auto& a : m_materialLibrary.Entries()) if (a.assetPath == path) {
                handle = static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr); materialId = a.id; break;
            }
            for (const auto& a : m_skyLibrary.Entries()) if (a.assetPath == path) {
                handle = static_cast<ImTextureID>(a.thumbnail.srv.gpu.ptr); break;
            }
            for (const auto& a : m_models) if (a.assetPath == path || a.path == path) {
                const auto found = m_modelPreviews.find(a.id);
                if (found != m_modelPreviews.end() && found->second->HasOutput())
                    handle = static_cast<ImTextureID>(found->second->OutputHandle().ptr);
                break;
            }
            ImGui::PushID(ToUtf8Portable(path).c_str()); ImGui::BeginGroup();
            const auto thumb = ui::ThumbnailButton("##asset", handle, size, m_selectedAssetPath == path);
            if (!handle) {
                const char* type = folder ? "フォルダ" : ext == ".tgscene" ? "シーン" : ext == ".tgmat" ? "マテリアル" :
                    ext == ".tgsky" ? "天球" : ext == ".tgmodel" || ext == ".fbx" ? "モデル" : IsImage(ext) ? "画像" : "ファイル";
                const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const auto text = ImGui::CalcTextSize(type);
                ImGui::GetWindowDrawList()->AddText(ImVec2((min.x + max.x - text.x) * 0.5f, (min.y + max.y - text.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), type);
            }
            if (thumb.clicked) m_selectedAssetPath = path;
            if (thumb.doubleClicked || (thumb.clicked && (ext == ".tgmodel" || ext == ".fbx"))) {
                if (folder) { m_assetDirectory = path; m_assetRefresh = true; }
                else m_pendingAssetOpen = path;
            }
            if ((textureId || materialId) && ImGui::BeginDragDropSource()) {
                if (materialId) ImGui::SetDragDropPayload(kMaterialDragDropType, &materialId, sizeof(materialId));
                else ImGui::SetDragDropPayload(kTextureDragDropType, &textureId, sizeof(textureId));
                ImGui::TextUnformatted(ToUtf8Display(path.filename()).c_str()); ImGui::EndDragDropSource();
            }
            if (thumb.hovered) ImGui::SetTooltip("%s\nダブルクリックで開く", ToUtf8Display(path).c_str());
            if (ImGui::BeginPopupContextItem("assetMenu")) {
                if (ImGui::MenuItem("開く")) {
                    if (folder) { m_assetDirectory = path; m_assetRefresh = true; }
                    else m_pendingAssetOpen = path;
                }
                if (ImGui::MenuItem("エクスプローラで表示")) RevealFileInExplorer(path);
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
