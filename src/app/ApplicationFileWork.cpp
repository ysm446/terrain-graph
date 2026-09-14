// ファイルメニュー、ショートカット、ドロップの受け付けと、
// フレームの外で処理する保留ファイル作業（開く / 保存 / 削除）。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "core/Shell.h"
#include "io/ProjectIo.h"
#include "io/SceneComponents.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

// ファイルメニュー。ここでは要求を積むだけで、実際の読み書きは
// ProcessPendingFileWork がフレームの外で行う（GPU 待機を伴うため）。
void Application::RequestOpenProject() {
    const std::filesystem::path path =
        ShowPickFolderDialog(L"プロジェクトのルートフォルダを開く", m_workspace.Root());
    if (!path.empty()) {
        m_pendingRoot = path;
    }
}

// saveAs が偽でも、まだ一度も保存していなければ保存先を聞く。
void Application::RequestSaveProject(bool saveAs) {
    CommitMaterialEdit();
    if (m_componentPreview >= 0) {
        m_pendingProjectSave = m_workspace.Root() / L".terrain-graph/editor/Preview.tgscene";
        return;
    }
    if (!saveAs && m_projectPath.extension() == L".tgscene") {
        m_pendingProjectSave = m_projectPath;
        return;
    }
    const std::filesystem::path path = ShowSaveFileDialog(
        L"シーンを保存", {{L"Terrain Graph シーン", L"*.tgscene"}}, L"tgscene",
        m_projectPath.extension() == L".tgscene" ? m_projectPath :
            m_workspace.Root() / L"Scenes" / (m_projectPath.empty() ? L"Untitled.tgscene" : m_projectPath.stem().wstring() + L".tgscene"));
    if (!path.empty() && m_workspace.Contains(path)) {
        m_pendingProjectSave = path;
        m_pendingProjectSave.replace_extension(L".tgscene");
    } else if (!path.empty()) {
        TG_LOG_ERROR("シーンはプロジェクトルート内に保存してください");
    }
}

// 保存ボタンを押した時点で画面に表示していたビューポートを残す。
// フレーム外で完了させ、保存後に別シーンへ切り替わっても混ざらないようにする。
void Application::SaveSceneThumbnail(const std::filesystem::path& path) {
    if (path.extension() != L".tgscene" || !m_renderer.HasOutput()) return;
    const auto thumbnail = io::SceneThumbnailPath(m_workspace, path);
    if (thumbnail.empty()) { TG_LOG_WARN("シーンサムネイルの保存先がルート外です"); return; }
    std::error_code error;
    std::filesystem::create_directories(thumbnail.parent_path(), error);
    if (error || !m_renderer.SaveOutputToPng(m_device, thumbnail, 256))
        TG_LOG_WARN("シーンは保存しましたが、サムネイルを保存できませんでした");
    m_assetThumbnails.Invalidate();
}

// キーボードショートカット。メニューと同じ入口（Request*）を通す。
//
// テキスト入力中でも効かせる（Ctrl + S は入力欄が食う操作ではない）。
// 実際の読み書きはどれも保留されるので、押された時点では要求が積まれるだけ。
void Application::HandleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();

    // F12 はコンテンツ領域のスクリーンショット。修飾キーは要らないので先に見る。
    // 実際の撮影はフレームの終わり（EndFrame）で行う。
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F12, false)) {
        m_screenshotPending = true;
    }

    if (!io.KeyCtrl || io.KeyAlt) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
        m_pendingProjectNew = true;
    } else if (ImGui::IsKeyPressed(ImGuiKey_B, false) && !io.WantTextInput) {
        // アセットの帯を畳む / 戻す（ウィンドウメニューと同じ）。
        m_settings.Display().showAssetBand = !m_settings.Display().showAssetBand;
        m_settings.Save();
    } else if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        RequestOpenProject();
    } else if (ImGui::IsKeyPressed(ImGuiKey_S, false)) {
        // Ctrl + Shift + S は「名前を付けて保存」。
        RequestSaveProject(io.KeyShift);
    } else if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        // テキスト入力中は InputText 内部のアンドゥに任せる。
        // 文書のアンドゥまで同時に走ると、無関係な編集が巻き戻る。
        if (io.WantTextInput) {
            return;
        }
        // Ctrl + Shift + Z も「やり直す」。Ctrl + Y と同じ。
        if (io.KeyShift) {
            if (m_undoHistory.CanRedo()) {
                m_pendingHistoryStep = 1;
            }
        } else if (m_undoHistory.CanUndo()) {
            m_pendingHistoryStep = -1;
        }
    } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false) && !io.WantTextInput &&
               m_undoHistory.CanRedo()) {
        m_pendingHistoryStep = 1;
    }
}

// 最近使ったシーン。名前を項目に、置き場所を右の列に出す。
// 同じ名前のシーンが別の場所にあっても見分けられるようにするため。
void Application::DrawRecentMenu() {
    const auto& roots = m_recentProjects.Roots();
    if (ImGui::BeginMenu("最近使ったルートフォルダ", !roots.empty())) {
        for (size_t i = 0; i < roots.size(); ++i) {
            const auto& root = roots[i];
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::BeginMenu(ToUtf8Display(root.path).c_str())) {
                if (ImGui::MenuItem("このルートを開く")) m_pendingRoot = root.path;
                ImGui::Separator();
                if (root.scenes.empty()) ImGui::MenuItem("最近使ったシーンはありません", nullptr, false, false);
                for (const auto& scene : root.scenes) {
                    ImGui::PushID(ToUtf8Portable(scene).c_str());
                    if (ImGui::MenuItem(ToUtf8Display(scene.filename()).c_str(), ToUtf8Display(scene.parent_path()).c_str())) {
                        m_pendingRoot = root.path;
                        m_pendingProjectOpen = scene;
                    }
                    ImGui::PopID();
                }
                ImGui::EndMenu();
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("ルートとシーンの履歴を消す")) m_recentProjects.ClearRoots();
        ImGui::EndMenu();
    }
    const std::vector<std::filesystem::path>& entries = m_recentProjects.Entries(m_workspace.Root());
    if (!ImGui::BeginMenu("最近使ったシーン", !entries.empty())) {
        return;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        const std::filesystem::path& path = entries[i];
        ImGui::PushID(static_cast<int>(i));

        const std::string name = ToUtf8Display(path.filename());
        const std::string directory = ToUtf8Display(path.parent_path());
        if (ImGui::MenuItem(name.c_str(), directory.c_str())) {
            m_pendingProjectOpen = path;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ToUtf8Display(path).c_str());
        }

        ImGui::PopID();
    }

    ImGui::Separator();
    if (ImGui::MenuItem("履歴を消す")) {
        m_recentProjects.Clear(m_workspace.Root());
    }
    ImGui::EndMenu();
}

void Application::DrawFileMenu() {
    if (!ImGui::BeginMenu("ファイル")) {
        return;
    }

    if (ImGui::MenuItem("新規シーン", "Ctrl+N")) {
        m_pendingProjectNew = true;
    }
    if (ImGui::MenuItem("ルートフォルダを開く…", "Ctrl+O")) {
        RequestOpenProject();
    }
    if (ImGui::MenuItem("シーンを開く…")) {
        m_pendingProjectOpen = ShowOpenFileDialog(L"シーンを開く", {{L"シーン / 旧プロジェクト", L"*.tgscene;*.tgproj;*.mmproj"}});
    }
    DrawRecentMenu();
    if (ImGui::MenuItem("保存済みシーンを部品に分離…", nullptr, false,
                        m_projectPath.extension() == L".tgscene" && !m_sceneComponents.is_array()))
        m_pendingComponentMigration = true;
    if (ImGui::MenuItem("保存", "Ctrl+S")) {
        RequestSaveProject(false);
    }
    if (ImGui::MenuItem("名前を付けて保存…", "Ctrl+Shift+S")) {
        RequestSaveProject(true);
    }
    // 未保存のプロジェクトには場所が無いので、項目は出したまま無効にする。
    if (ImGui::MenuItem("ファイルの場所を開く", nullptr, false, !m_projectPath.empty())) {
        RevealFileInExplorer(m_projectPath);
    }

    ImGui::Separator();
    if (ImGui::MenuItem("テクスチャを書き出す…")) {
        // 書き出し先が未設定なら、プロジェクトの隣を初期値にする。
        if (m_exportSettings.directory.empty() && !m_projectPath.empty()) {
            m_exportSettings.directory = m_projectPath.parent_path();
        }
        if (m_exportSettings.baseName.empty() && !m_projectPath.empty()) {
            m_exportSettings.baseName = ToUtf8Display(m_projectPath.stem());
        }
        m_showExport = true;
    }

    ImGui::Separator();
    if (ImGui::MenuItem("終了")) {
        m_window.RequestClose();
    }
    ImGui::EndMenu();
}

void Application::HandleDroppedFiles(const std::vector<std::filesystem::path>& paths) {
    size_t images = 0;
    for (const std::filesystem::path& path : paths) {
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        // 拡張子で行き先を決める。読み込み自体はどれも保留し、フレームの外で処理する。
        // 旧拡張子 (.mmproj / .mmmat) は material-mixer 時代のファイル。読み込みだけ受け付ける。
        if (extension == ".tgscene" || extension == ".tgproj" || extension == ".mmproj") {
            m_pendingProjectOpen = path;
        } else if (extension == ".tgsky" || extension == ".tgmodel") {
            m_pendingAssetOpen = path;
        } else if (extension == ".tgmat" || extension == ".mmmat") {
            m_pendingMaterialImport = path;
        } else if (extension == ".fbx") {
            m_pendingModels.push_back(path);
        } else if (extension == ".hdr") {
            // 選択中の天球へ入れる。天球は必ず 1 つあるので、行き先は常に決まる。
            m_skyLibrary.EnsureDefault();
            if (renderer::SkyAsset* sky = m_skyLibrary.ActiveMutable(); sky != nullptr) {
                sky->sky.source = renderer::SkySource::Hdri;
                sky->sky.hdriPath = path;
                m_skyLibrary.MarkThumbnailDirty(sky->id);
                TG_LOG_INFO("天球「%s」に %s を割り当てました", sky->name.c_str(),
                            ToUtf8Display(path.filename()).c_str());
            }
        } else if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
                   extension == ".tga" || extension == ".bmp" || extension == ".exr") {
            m_pendingTexturePaths.push_back(path);
            ++images;
        } else {
            TG_LOG_WARN("扱えない形式です: %s", ToUtf8Display(path.filename()).c_str());
        }
    }
    if (images > 0) {
        TG_LOG_INFO("%zu 枚の画像を読み込みます", images);
    }
}

void Application::FinishComponentPreview(bool place) {
    if (m_componentPreview < 0) return;
    if (place) {
        // 差し替えで、元グラフの未保存編集を失わない。
        const auto path = m_projectPath.extension() == L".tgscene" ? m_projectPath :
            m_workspace.Root() / L"Scenes/Untitled.tgscene";
        io::ProjectRefs previous{m_textureLibrary, m_materialLibrary, m_paintMasks,
            m_skyLibrary, m_renderer, m_previewOriginalGraph, &m_models, &m_previewOriginalComponents, m_componentPreview};
        if (!io::SaveProject(path, m_device, previous, &m_workspace)) {
            TG_LOG_ERROR("元のグラフを保存できないため、差し替えを中止しました");
            return;
        }
    }
    if (!place) { m_graph = std::move(m_previewOriginalGraph); m_sceneComponents = m_previewOriginalComponents; }
    m_previewOriginalGraph = graph::NodeGraph{}; m_previewOriginalComponents = nullptr;
    m_componentPreview = -1; m_componentPreviewPath.clear();
    m_undoHistory.Clear(); m_documentDirty = false; m_committed = CaptureDocument();
    m_compiledGraphRevision = 0; m_graphStack.MarkDirty();
    for (auto& slot : m_cloudMasks) slot.graphRevision = 0;
    const int component = m_editComponent;
    m_editComponent = -1; OpenComponentEditor(component);
}

void Application::ResetProject() {
    m_componentPreview = -1; m_componentPreviewPath.clear();
    m_previewOriginalGraph = graph::NodeGraph{}; m_previewOriginalComponents = nullptr;
    m_sceneComponents = nlohmann::json::array();
    m_editComponent = 0;
    m_materialEditPending = false;
    m_materialEditAppearanceChanged = false;
    // どれも GPU 待機を伴う。フレームの外から呼ぶこと。
    m_paintMasks.Clear(m_device);
    m_models.clear();
    m_selectedModel = 0;
    m_modelLod = 0;
    m_renderedModelThumbnails.clear();
    m_materialLibrary.Clear(m_device);
    m_skyLibrary.Clear(m_device);
    m_skyLibrary.EnsureDefault();
    m_textureLibrary.Clear(m_device);

    // グラフを既定（ベース → 出力）へ戻す。位置はエディタへ流し込み直す。
    // m_graphStack は代入で作り直さず MarkDirty で改版する（revision が戻ると
    // 評価器が「変わっていない」と判断してしまう）。
    m_graph = graph::NodeGraph::CreateDefault();
    m_selectedGraphNode = 0;
    m_previewGraphNode = 0;
    m_previewGraphPin = 0;
    m_compiledGraphRevision = 0;
    for (auto& slot : m_cloudMasks) slot.graphRevision = 0;
    m_graphStack.MarkDirty();
    RequestGraphNodePlacement();

    // **プレビュー設定も既定へ戻す。** 形状・変位量・カメラ・ライト・露出・
    // 被写界深度はプロジェクトが持つ値なので、戻さないと前の中身が残る。
    m_renderer.ResetSettings();

    m_selectedMaterial = 0;
    m_selectedTexture = 0;
    m_ordTexture = compositor::kNoTexture;
    m_paintMode = false;
    m_strokeActive = false;

    // 別の文書になるので履歴は捨てる。戻せてしまうと中身が混ざる。
    m_undoHistory.Clear();
    m_documentDirty = false;
    m_pendingHistoryStep = 0;
    m_committed = CaptureDocument();
}

void Application::UpdateWindowTitle() {
    std::wstring title;
    if (!m_projectPath.empty()) {
        title = m_projectPath.filename().wstring() + L" - ";
    }
    title += L"Terrain Graph";
    m_window.SetTitle(title.c_str());
}

void Application::ProcessPendingFileWork() {
    if (m_pendingPreviewFinish) { FinishComponentPreview(m_pendingPreviewFinish == 1); m_pendingPreviewFinish = 0; }
    if (m_componentPreview >= 0 && (!m_pendingRoot.empty() || !m_pendingProjectOpen.empty() || m_pendingProjectNew)) {
        m_pendingRoot.clear(); m_pendingProjectOpen.clear(); m_pendingProjectNew = false;
        TG_LOG_WARN("先にグラフの一時プレビューを終了してください");
    }
    if (m_pendingAssetOpen.extension() == L".tgscene" || m_pendingAssetOpen.extension() == L".tgproj" ||
        m_pendingAssetOpen.extension() == L".mmproj") {
        m_pendingProjectOpen = std::move(m_pendingAssetOpen); m_pendingAssetOpen.clear();
    }
    // 対話中のシーン/ルート切り替えは、保存の機会を設けてから実行する。
    if ((!m_pendingRoot.empty() || !m_pendingProjectOpen.empty() || m_pendingProjectNew) &&
        m_frameCounter > 1 && !Headless() && !m_allowSceneSwitch) {
        m_deferredRoot = std::move(m_pendingRoot); m_pendingRoot.clear();
        m_deferredScene = std::move(m_pendingProjectOpen); m_pendingProjectOpen.clear();
        m_deferredNew = m_pendingProjectNew; m_pendingProjectNew = false;
        m_sceneSwitchDialog = true;
    }
    // 読み込みは同期で、その間は画面が止まる。読む前に 1 フレームだけ描いて
    // 「読み込み中」を見せてから、次のフレームの頭で実際に読む。
    if ((!m_pendingRoot.empty() || !m_pendingProjectOpen.empty()) && !m_sceneLoadAnnounced &&
        !Headless()) {
        m_sceneLoadAnnounced = true;
        m_allowSceneSwitch = true;  // 次のフレームで保存の確認を出し直さない
        return;
    }
    m_allowSceneSwitch = false;
    const auto loadStart = std::chrono::steady_clock::now();
    ProcessAssetWork();
    if (m_pendingComponentMigration) {
        m_pendingComponentMigration = false;
        const auto migrated = io::MigrateSceneComponents(m_workspace, m_projectPath);
        if (!migrated.empty()) {
            m_pendingProjectOpen = migrated;
            m_assetRefresh = true;
            TG_LOG_INFO("保存済みの元シーンを保持して分離しました: %s", ToUtf8Display(migrated).c_str());
        } else TG_LOG_ERROR("シーンを分離できませんでした。元データは保持しています");
        return;
    }
    // どれもリソースの生成・破棄と GPU 待機を伴う。フレームの外で処理すること。

    // アンドゥ / リドゥ。マテリアルの破棄を伴うのでここで処理する。
    if (m_pendingHistoryStep != 0) {
        const int step = m_pendingHistoryStep;
        m_pendingHistoryStep = 0;

        const DocumentSnapshot current = CaptureDocument();
        if (step < 0 && m_undoHistory.CanUndo()) {
            ApplyDocument(m_undoHistory.Undo(current));
        } else if (step > 0 && m_undoHistory.CanRedo()) {
            ApplyDocument(m_undoHistory.Redo(current));
        }
        m_committed = CaptureDocument();
        m_pendingPaintSweep = true;
    }

    if (m_pendingPaintSweep) {
        m_pendingPaintSweep = false;
        SweepPaintMasks();
    }

    if (m_pendingProjectNew) {
        m_pendingProjectNew = false;
        ResetProject();
        m_projectPath.clear();
        m_sceneLoadSeconds = -1.0f;
        UpdateWindowTitle();
    }

    if (!m_pendingProjectOpen.empty()) {
        const std::filesystem::path path = m_pendingProjectOpen;
        m_pendingProjectOpen.clear();

        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
                             m_skyLibrary,     m_renderer,       m_graph, &m_models, &m_sceneComponents};
        if (io::LoadProject(path, m_device, m_pipelineCache, refs,
                            path.extension() == L".tgscene" ? &m_workspace : nullptr)) {
            m_editComponent = m_sceneComponents.is_array() ? 0 : -1;
            m_materialEditPending = false;
            m_materialEditAppearanceChanged = false;
            // 比較用の起動引数は保存された品質設定より優先する。
            if (m_options.referenceCloudLighting) m_renderer.CloudLightingCache() = false;
            if (m_options.fullResolutionClouds) m_renderer.FullResolutionClouds() = true;
            if (m_options.disableTemporalClouds) m_renderer.TemporalClouds() = false;
            m_recentProjects.Add(m_workspace.Root(), path);
            m_projectPath = path;
            m_assetRefresh = true;
            m_selectedGraphNode = m_graph.FindNode(m_options.selectNode) ? m_options.selectNode : 0;
            if (m_sceneComponents.is_array() && m_selectedGraphNode)
                m_editComponent = m_graph.FindNode(m_selectedGraphNode)->component;
            m_previewGraphNode = 0;
            m_previewGraphPin = 0;
            m_compiledGraphRevision = 0;
            for (auto& slot : m_cloudMasks) slot.graphRevision = 0;
            m_graphStack.MarkDirty();
            RequestGraphNodePlacement();
            m_selectedModel = m_models.empty() ? 0 : m_models.front().id;
            m_modelLod = 0;
            m_renderedModelThumbnails.clear();
            m_selectedMaterial = 0;
            m_selectedTexture = 0;
            m_ordTexture = compositor::kNoTexture;
            m_paintMode = false;
            m_strokeActive = false;
            // 読み込んだ文書が新しい起点になる。前の文書の履歴は捨てる。
            m_undoHistory.Clear();
            m_documentDirty = false;
            m_pendingHistoryStep = 0;
            m_committed = CaptureDocument();
            UpdateWindowTitle();
            // ルートの切り替えも含めた、このフレームで読み込みに掛かった時間。
            m_sceneLoadSeconds =
                std::chrono::duration<float>(std::chrono::steady_clock::now() - loadStart).count();
            TG_LOG_INFO("シーンを読み込みました: %s（%.2f 秒）",
                        ToUtf8Display(path.filename()).c_str(), m_sceneLoadSeconds);
        } else {
            // 消えた / 壊れたプロジェクトを履歴に残しても、選べるだけで意味がない。
            m_recentProjects.Remove(m_workspace.Root(), path);
        }
    }
    m_sceneLoadAnnounced = false;

    if (!m_pendingProjectSave.empty()) {
        const std::filesystem::path path = m_pendingProjectSave;
        m_pendingProjectSave.clear();

        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
                             m_skyLibrary,     m_renderer,       m_graph, &m_models, &m_sceneComponents};
        if (m_componentPreview >= 0) refs.componentOnly = m_componentPreview;
        if (io::SaveProject(path, m_device, refs, &m_workspace)) {
            if (m_componentPreview >= 0) {
                m_assetRefresh = true;
                TG_LOG_INFO("グラフアセットを保存しました: %s", ToUtf8Display(m_componentPreviewPath).c_str());
                return;
            }
            SaveSceneThumbnail(path);
            m_assetRefresh = true;
            m_recentProjects.Add(m_workspace.Root(), path);
            m_projectPath = path;
            UpdateWindowTitle();
            if (m_saveThenSwitch) { m_saveThenSwitch = false; ResumeSceneSwitch(); }
        } else {
            m_saveThenSwitch = false;
            m_deferredRoot.clear(); m_deferredScene.clear(); m_deferredNew = false;
            TG_LOG_ERROR("シーンの保存に失敗しました。現在の作業を保持しています");
        }
    }

    if (!m_pendingMaterialExport.empty()) {
        const std::filesystem::path path = m_pendingMaterialExport;
        const compositor::MaterialAssetId id = m_pendingExportMaterial;
        m_pendingMaterialExport.clear();
        m_pendingExportMaterial = compositor::kNoMaterialAsset;

        if (const compositor::MaterialAsset* asset = m_materialLibrary.Find(id);
            asset != nullptr) {
            io::SaveMaterial(path, *asset, m_textureLibrary);
        }
    }

    if (!m_pendingMaterialImport.empty()) {
        const std::filesystem::path path = m_pendingMaterialImport;
        m_pendingMaterialImport.clear();

        nlohmann::json assetDocument;
        if (io::ProjectWorkspace::ReadJson(path, assetDocument) &&
            io::ProjectWorkspace::String(assetDocument, "format") == "terrain-graph.material-asset") {
            m_pendingAssetOpen = path;
            return;
        }
        const compositor::MaterialAssetId id = io::LoadMaterial(
            path, m_device, m_pipelineCache, m_textureLibrary, m_materialLibrary);
        if (id != compositor::kNoMaterialAsset) {
            m_selectedMaterial = static_cast<int>(m_materialLibrary.Entries().size()) - 1;
            m_pendingAssetsSave = true;
        }
    }

    if (m_pendingTextureRemove != compositor::kNoTexture) {
        const compositor::TextureId removed = m_pendingTextureRemove;
        m_pendingTextureRemove = compositor::kNoTexture;

        // 参照を先に外す。無効な ID を残すと、次に同じ番号が払い出されたときに
        // 別の画像が割り当たってしまう。
        const auto clearSlot = [removed](compositor::TextureId& slot) {
            const bool hit = (slot == removed);
            if (hit) {
                slot = compositor::kNoTexture;
            }
            return hit;
        };
        const auto clearMap = [removed](compositor::MapSlot& slot) {
            const bool hit = (slot.texture == removed);
            if (hit) {
                slot = compositor::MapSlot{};
            }
            return hit;
        };

        for (const compositor::MaterialAsset& entry : m_materialLibrary.Entries()) {
            compositor::MaterialAsset* asset = m_materialLibrary.FindMutable(entry.id);
            bool hit = clearSlot(asset->baseColor);
            hit |= clearSlot(asset->normal);
            hit |= clearMap(asset->roughness);
            hit |= clearMap(asset->metallic);
            hit |= clearMap(asset->ambientOcclusion);
            hit |= clearMap(asset->height);
            if (hit) {
                asset->thumbnailDirty = true;
            }
        }
        // グラフのノードが持つレイヤーから外す。
        bool graphChanged = false;
        for (graph::Node& node : m_graph.MutableNodes()) {
            if (auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings)) {
                graphChanged |= clearMap(settings->layer.mask.texture);
                graphChanged |= clearMap(settings->layer.heightTexture);
            }
        }
        if (graphChanged) {
            m_graph.MarkDirty();
        }
        clearSlot(m_ordTexture);

        // 解放は DeferRelease でフレーム同期後に行われるため、GPU 待機は不要。
        m_textureLibrary.Remove(m_device, removed);
        m_graphStack.MarkDirty();
    }

    if (m_pendingMaterialRemove != compositor::kNoMaterialAsset) {
        const compositor::MaterialAssetId removed = m_pendingMaterialRemove;
        m_pendingMaterialRemove = compositor::kNoMaterialAsset;

        if (m_materialLibrary.Find(removed) != nullptr) {
            m_materialLibrary.Remove(m_device, removed);
            // 参照していたノードは「なし」へ戻す。無効な ID を残さない。
            bool graphChanged = false;
            for (graph::Node& node : m_graph.MutableNodes()) {
                auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings);
                if (settings != nullptr && settings->layer.material == removed) {
                    settings->layer.material = compositor::kNoMaterialAsset;
                    graphChanged = true;
                }
            }
            if (graphChanged) {
                m_graph.MarkDirty();
            }
            m_selectedMaterial = std::max(0, m_selectedMaterial - 1);
            MarkDocumentChanged();
        }
    }

    if (m_pendingExport) {
        m_pendingExport = false;
        SyncGraphStack();
        // プレビューと同じくグラフのコンパイル結果を書き出す。
        const io::ExportRefs refs{m_graphStack, m_textureLibrary, m_materialLibrary,
                                  m_paintMasks};
        const uint32_t written =
            io::ExportMaterialTextures(m_device, m_pipelineCache, refs, m_exportSettings);
        if (written > 0) {
            m_toasts.Push("テクスチャを " + std::to_string(written) + " 枚書き出しました");
            m_showExport = false;
        }
    }

}

}  // namespace tg
