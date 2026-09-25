#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <tuple>

#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/ImageIo.h"
#include "core/Log.h"
#include "io/AssetRelations.h"
#include "ui/UiStyle.h"

namespace tg {
namespace {
// 配置用メッシュの共有キー。自動 LOD は全段を持つので固定 LOD と分ける。
std::string InstanceMeshKey(uint64_t model, const graph::ModelScatterSettings& settings) {
    return std::to_string(model) + ":" + (settings.autoLod ? std::string("auto") : std::to_string(settings.lod));
}
}  // namespace
void Application::PrepareModelScatters() {
    m_modelScatters = m_graph.CompileModelScatters();
    std::vector<graph::GraphId> sources;
    std::vector<std::string> meshKeys;
    for (const auto& scatter : m_modelScatters) {
        for (const auto& choice : scatter.settings.models) {
            const auto model = std::find_if(m_models.begin(),m_models.end(),[&](const auto& m) { return m.id == choice.model; });
            if (model == m_models.end() || !model->geometry || choice.weight <= 0) continue;
            const auto key = InstanceMeshKey(choice.model, scatter.settings);
            meshKeys.push_back(key);
            auto& mesh = m_instanceMeshes[key];
            if (!mesh) mesh = std::make_unique<renderer::ModelPreview>();
            // 自動 LOD でインポスターを焼いてあれば、最終段として使う。
            mesh->Prepare(m_device,*model,scatter.settings.autoLod ? renderer::kAllLods : scatter.settings.lod,
                          scatter.settings.autoLod && m_impostors.Find(model->id) != nullptr);
        }
        if (std::find(sources.begin(),sources.end(),scatter.source) != sources.end()) continue;
        // 本体の評価器が点まで作っていれば、元ごとの評価器は持たない（VRAM と GPU 時間の節約）。
        if (std::find(m_mainPointSources.begin(),m_mainPointSources.end(),scatter.source) != m_mainPointSources.end()) continue;
        sources.push_back(scatter.source);
        auto& slot = m_modelPoints[scatter.source];
        if (!slot) slot = std::make_unique<ModelPointSlot>();
        const uint32_t resolution = m_renderer.MaterialResolution();
        if (slot->evaluator.Resolution() == 0) slot->evaluator.Create(m_device,resolution);
        else if (slot->evaluator.Resolution() != resolution) slot->evaluator.Resize(m_device,resolution);
        const auto* scale = m_graph.FindChainScale(scatter.source);
        slot->stack.SetTerrainScale(scale ? scale->sizeMeters : m_renderer.PlaneSize(),
                                   scale ? scale->heightMeters : m_renderer.DisplacementScale());
        if (slot->graphRevision != m_graph.TerrainRevision() ||
            slot->documentRevision != m_graphStack.Revision() || slot->paintRevision != m_paintMasks.Revision()) {
            auto compiled = m_graph.CompileLayersTo(scatter.source);
            for (size_t i=0;i<compiled.layerSources.size();++i)
                if (compiled.layerSources[i] == scatter.source) {
                    compiled.layers[i].maskOnly = true;
                    compiled.layers[i].pointsOnly = true;
                    compiled.layers[i].pointsId = static_cast<uint32_t>(scatter.source);
                }
            slot->stack.Layers() = std::move(compiled.layers);
            slot->stack.MaskOps() = std::move(compiled.maskOps);
            slot->stack.MarkDirty();
            slot->evaluator.Invalidate();
            slot->graphRevision = m_graph.TerrainRevision();
            slot->documentRevision = m_graphStack.Revision();
            slot->paintRevision = m_paintMasks.Revision();
        }
    }
    for (auto it=m_modelPoints.begin();it!=m_modelPoints.end();) {
        if (std::find(sources.begin(),sources.end(),it->first)==sources.end()) {
            it->second->evaluator.Destroy(m_device); it=m_modelPoints.erase(it);
        } else ++it;
    }
    for (auto it=m_instanceMeshes.begin();it!=m_instanceMeshes.end();) {
        if (std::find(meshKeys.begin(),meshKeys.end(),it->first)==meshKeys.end()) {
            it->second->Destroy(m_device); it=m_instanceMeshes.erase(it);
        } else ++it;
    }
    m_renderer.drawInstances = [this](auto* list,const auto& matrix,bool shadow) { DrawModelScatters(list,matrix,shadow); };
}
Application::PlacementPointsState Application::PlacementPointsOf(
    graph::GraphId source, const compositor::PlacementPointSet** out) const {
    *out = nullptr;
    const compositor::MaterialEvaluator* evaluator = nullptr;
    bool current = false;
    if (std::find(m_mainPointSources.begin(), m_mainPointSources.end(), source) != m_mainPointSources.end()) {
        evaluator = &m_renderer.Evaluator();
        current = evaluator->EvaluatedRevision() == m_graphStack.Revision();
    } else if (const auto slot = m_modelPoints.find(source); slot != m_modelPoints.end()) {
        evaluator = &slot->second->evaluator;
        current = slot->second->graphRevision == m_graph.TerrainRevision() &&
                  evaluator->EvaluatedRevision() == slot->second->stack.Revision();
    } else {
        return PlacementPointsState::Missing;
    }
    if (!current || evaluator->HasPendingPostprocess()) return PlacementPointsState::Evaluating;
    *out = evaluator->PlacementPoints(static_cast<uint32_t>(source));
    return PlacementPointsState::Ready;
}
void Application::CollectModelScatterStats() {
    for (auto& [key, mesh] : m_instanceMeshes) mesh->CollectInstanceStats(m_device);
}
void Application::DrawModelScatters(ID3D12GraphicsCommandList* commandList,
                                   const DirectX::XMFLOAT4X4& viewProjection, bool shadow) {
    static_assert(renderer::kMaxInstanceLods == std::tuple_size_v<decltype(renderer::RenderStats::instancesPerLod)>);
    // 本描画で使ったメッシュ。複数の配置で共有していても、描画量は 1 回だけ足す（読み戻しは合計）。
    std::vector<const renderer::ModelPreview*> drawn;
    for (const auto& scatter : m_modelScatters) {
        const compositor::PlacementPointSet* points = nullptr;
        if (PlacementPointsOf(scatter.source, &points) != PlacementPointsState::Ready || points == nullptr ||
            !points->points.IsValid() || points->count == 0) continue;
        float total = 0;
        for (const auto& choice : scatter.settings.models)
            if (std::any_of(m_models.begin(),m_models.end(),[&](const auto& m){return m.id==choice.model && m.geometry;})) total += std::max(choice.weight,0.0f);
        if (total <= 0) continue;
        float cumulative = 0;
        for (const auto& choice : scatter.settings.models) {
            const auto model=std::find_if(m_models.begin(),m_models.end(),[&](const auto& m){return m.id==choice.model;});
            if (model==m_models.end() || !model->geometry || choice.weight<=0) continue;
            const auto mesh=m_instanceMeshes.find(InstanceMeshKey(choice.model, scatter.settings));
            if (mesh==m_instanceMeshes.end()) continue;
            renderer::ModelInstanceDraw draw;
            draw.points=points->points.SrvIndex();
            draw.rows=points->points.height/2; draw.count=points->count;
            draw.attributes=points->attributes.IsValid() ? points->attributes.SrvIndex() : compositor::kInvalidTextureIndex;
            draw.seed=static_cast<uint32_t>(scatter.settings.seed);
            draw.weightStart=cumulative/total; cumulative+=choice.weight; draw.weightEnd=cumulative/total;
            draw.scaleMin=scatter.settings.scaleMin; draw.scaleMax=std::max(draw.scaleMin,scatter.settings.scaleMax);
            draw.align=scatter.settings.alignToNormal; draw.offset=scatter.settings.offset;
            draw.maxDistance=scatter.settings.maxDistance;
            draw.lodBias=scatter.settings.lodBias;
            draw.usePointSize=scatter.settings.usePointSize; draw.shadow=shadow;
            draw.lodView=m_renderer.Debug()==renderer::DebugView::Lod;
            draw.viewProjection=viewProjection; draw.cameraPosition=m_renderer.GetCamera().Position();
            if (!shadow) {
                draw.shadows=m_renderer.InstanceShadows();
                const auto& clouds=m_renderer.InstanceClouds();
                draw.atmosphere=clouds.atmosphere; draw.cloudNoiseIndex=clouds.noiseIndex; draw.atmosphericMode=clouds.mode;
                draw.ambient=m_renderer.InstanceAmbient();
            }
            m_renderer.RecordInstanceDrawCalls(mesh->second->Render(m_device,m_pipelineCache,commandList,*model,
                m_materialLibrary,m_textureLibrary,m_renderer.GetEnvironment(),m_renderer.EnvironmentIntensity(),
                m_renderer.EffectiveLight(),m_renderer.Exposure().Exposure(),m_renderer.Tonemap(),&draw,
                m_impostors.Find(model->id)));
            if (!shadow && std::find(drawn.begin(),drawn.end(),mesh->second.get())==drawn.end())
                drawn.push_back(mesh->second.get());
        }
    }
    // 影も含めた 1 フレームぶんの合計が、完了したフレームから読み戻してある。
    for (const auto* mesh : drawn) {
        const auto& stats = mesh->LatestInstanceStats();
        m_renderer.RecordInstanceStats(stats.vertices, stats.triangles, stats.instances);
    }
}

void Application::ProcessModelWork() {
    for (const auto& asset : m_models) m_nextModelId = std::max(m_nextModelId, asset.id + 1);
    for (const auto& inputPath : m_pendingModels) {
        const auto path = m_workspace.Import(inputPath, m_assetDirectory);
        if (path.empty()) continue;
        if (std::any_of(m_models.begin(), m_models.end(), [&](const auto& a) { return a.path == path; })) continue;
        renderer::ModelAsset asset;
        asset.id = m_nextModelId++;
        asset.name = ToUtf8Display(path.stem());
        asset.path = path;
        asset.assetPath = m_workspace.UniquePath(m_assetDirectory, asset.name, ".tgmodel");
        if (renderer::LoadModel(path, asset)) {
            m_selectedModel = asset.id;
            m_modelLod = 0;
            m_showModelPreview = true;
            m_focusModelLibrary = true;
            m_models.push_back(std::move(asset));
            m_pendingAssetsSave = true;
            m_assetRefresh = true;
            MarkDocumentChanged(false);
            TG_LOG_INFO("モデルを読み込みました: %s", ToUtf8Display(path).c_str());
        } else
            TG_LOG_ERROR("モデルの読み込みに失敗: %s", asset.error.c_str());
    }
    m_pendingModels.clear();
    if (m_options.previewModel >= 0 && m_options.previewModel < static_cast<int>(m_models.size())) {
        m_selectedModel = m_models[m_options.previewModel].id;
        m_modelLod = m_options.previewModelLod;
        m_options.previewModel = -1;
        m_showModelPreview = true;
        m_focusModelLibrary = true;
    }
    for (auto it = m_modelPreviews.begin(); it != m_modelPreviews.end();) {
        if (std::none_of(m_models.begin(), m_models.end(),
                         [&](const auto& a) { return a.id == it->first; })) {
            it->second->Destroy(m_device);
            m_renderedModelThumbnails.erase(it->first);
            m_impostors.Remove(m_device, it->first);
            it = m_modelPreviews.erase(it);
        } else
            ++it;
    }
    for (const auto& asset : m_models) {
        m_nextModelId = std::max(m_nextModelId, asset.id + 1);
        auto& preview = m_modelPreviews[asset.id];
        if (!preview) preview = std::make_unique<renderer::ModelPreview>();
        preview->Prepare(m_device, asset, asset.id == m_selectedModel ? m_modelLod : 0);
    }
    ProcessImpostorWork();
}
bool Application::BakeRequestedImpostors() {
    std::vector<uint64_t> targets;
    for (const auto& request : m_options.bakeImpostors) {
        if (request == L"all") {
            for (const auto& asset : m_models) targets.push_back(asset.id);
            continue;
        }
        const auto path = std::filesystem::absolute(request);
        const auto matches = [&](const renderer::ModelAsset& asset) {
            std::error_code error;
            return !asset.assetPath.empty() && std::filesystem::equivalent(asset.assetPath, path, error);
        };
        auto found = std::find_if(m_models.begin(), m_models.end(), matches);
        if (found == m_models.end()) {
            // シーンで使っていないモデルは、その場で読み込む。
            const io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks, m_skyLibrary,
                                       m_renderer, m_graph, &m_models, &m_sceneComponents, -1, &m_sceneAtmosphere};
            if (!m_workspace.Contains(path) ||
                !io::LoadSharedAsset(m_workspace, path, m_device, m_pipelineCache, refs)) {
                TG_LOG_ERROR("インポスターを焼くモデルを読み込めません: %s", ToUtf8Display(path).c_str());
                return false;
            }
            m_materialLibrary.ProcessPendingWork(m_device, m_pipelineCache, m_textureLibrary, false);
            found = std::find_if(m_models.begin(), m_models.end(), matches);
            if (found == m_models.end()) {
                TG_LOG_ERROR("インポスターを焼くモデルが見つかりません: %s", ToUtf8Display(path).c_str());
                return false;
            }
        }
        targets.push_back(found->id);
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    for (const uint64_t id : targets) {
        auto asset = std::find_if(m_models.begin(), m_models.end(), [&](const auto& a) { return a.id == id; });
        if (asset == m_models.end() || !asset->geometry || asset->assetPath.empty()) continue;
        // アプリの「作成」ボタン（ProcessImpostorWork）と同じ焼き方・置き場所。
        std::filesystem::path colorPath, normalPath, variationPath;
        renderer::ImpostorPaths(*asset, colorPath, normalPath, variationPath);
        renderer::ModelImpostor result;
        std::string error;
        if (!m_impostors.Bake(m_device, m_pipelineCache, *asset, m_materialLibrary, m_textureLibrary,
                              colorPath, normalPath, variationPath, result, error)) {
            TG_LOG_ERROR("インポスターを作れませんでした: %s（%s）", asset->name.c_str(), error.c_str());
            return false;
        }
        asset->impostor = result;
        // .tgmodel だけを保存する（シーンや他のアセットは書かない）。
        io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks, m_skyLibrary,
                             m_renderer, m_graph, &m_models, &m_sceneComponents, -1, &m_sceneAtmosphere};
        refs.saveSharedAssets = false;
        const auto target = asset->assetPath;
        if (!io::SaveSharedAssets(m_workspace, refs, &target)) {
            TG_LOG_ERROR("モデルを保存できませんでした: %s", ToUtf8Display(target).c_str());
            return false;
        }
    }
    TG_LOG_INFO("インポスターを焼き直しました: %zu モデル", targets.size());
    return true;
}

void Application::ProcessImpostorWork() {
    const auto find = [&](uint64_t id) {
        const auto found = std::find_if(m_models.begin(), m_models.end(), [&](const auto& a) { return a.id == id; });
        return found == m_models.end() ? nullptr : &*found;
    };
    if (auto* asset = find(std::exchange(m_pendingImpostorBake, 0))) {
        std::filesystem::path colorPath, normalPath, variationPath;
        renderer::ImpostorPaths(*asset, colorPath, normalPath, variationPath);
        renderer::ModelImpostor result;
        std::string error;
        if (m_impostors.Bake(m_device, m_pipelineCache, *asset, m_materialLibrary, m_textureLibrary,
                             colorPath, normalPath, variationPath, result, error)) {
            asset->impostor = result;
            m_modelShowImpostor = true;
            m_assetRefresh = true;
            MarkDocumentChanged(false);
        } else {
            TG_LOG_ERROR("インポスターを作れませんでした: %s", error.c_str());
        }
    }
    if (auto* asset = find(std::exchange(m_pendingImpostorDelete, 0)); asset && asset->impostor.baked) {
        // 画像と .meta はアセットの削除と同じく、ルート内の退避フォルダへ移す（戻せる）。
        for (const auto& path : {asset->impostor.colorPath, asset->impostor.normalPath, asset->impostor.variationPath}) {
            std::error_code error;
            if (path.empty() || !std::filesystem::exists(path, error)) continue;
            if (!m_workspace.Contains(path) || !io::RetireAsset(m_workspace, io::InspectAssetRelations(m_workspace, path)))
                TG_LOG_WARN("インポスターの画像を退避できませんでした: %s", ToUtf8Display(path).c_str());
        }
        const auto settings = asset->impostor.settings;
        asset->impostor = {};
        asset->impostor.settings = settings;
        m_impostors.Remove(m_device, asset->id);
        m_modelShowImpostor = false;
        m_assetRefresh = true;
        MarkDocumentChanged(false);
    }
    // 焼いた画像を GPU へ（読み込み済みなら何もしない）。
    // まだ読んでいないモデルの画像（3072² の PNG が 1 モデル 2〜3 枚）は先にまとめて
    // 裏でデコードしておく。順に読むと数十本の木で数秒かかる。
    std::vector<std::filesystem::path> prefetch;
    for (const auto& asset : m_models) {
        if (!asset.impostor.baked || m_impostors.Find(asset.id) != nullptr) continue;
        prefetch.push_back(asset.impostor.colorPath);
        prefetch.push_back(asset.impostor.normalPath);
        if (!asset.impostor.variationPath.empty()) prefetch.push_back(asset.impostor.variationPath);
    }
    if (!prefetch.empty()) PrefetchImages(prefetch);
    for (const auto& asset : m_models) m_impostors.Sync(m_device, m_pipelineCache, asset);
    if (!prefetch.empty()) DiscardPrefetchedImages();
}
void Application::RenderModelPreviews(ID3D12GraphicsCommandList* commandList) {
    for (const auto& asset : m_models) {
        const auto found = m_modelPreviews.find(asset.id);
        if (found == m_modelPreviews.end()) continue;
        if (!(m_modelPreviewVisible && asset.id == m_selectedModel) &&
            m_renderedModelThumbnails.contains(asset.id))
            continue;
        const auto* impostor =
            asset.id == m_selectedModel && m_modelShowImpostor ? m_impostors.Find(asset.id) : nullptr;
        found->second->Render(m_device, m_pipelineCache, commandList, asset, m_materialLibrary,
                              m_textureLibrary, m_renderer.GetEnvironment(),
                              m_renderer.EnvironmentIntensity(), m_renderer.EffectiveLight(),
                              m_renderer.Exposure().Exposure(), m_renderer.Tonemap(), nullptr, impostor);
        if (found->second->HasOutput()) m_renderedModelThumbnails.insert(asset.id);
    }
}
void Application::DrawModelLibraryPanel() {
    // 初回のドック構築が終わってから、読み込んだ一覧のタブへ移る。
    if (m_focusModelLibrary && m_frameCounter >= 2) {
        ImGui::SetNextWindowFocus();
        m_focusModelLibrary = false;
    }
    if (!ImGui::Begin("モデル")) {
        ImGui::End();
        return;
    }
    uint64_t remove = 0;
    const float size = ui::Scaled(84);
    if (ImGui::BeginChild("modelGrid", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
        if (m_models.empty()) ui::HintText("FBXファイルをドロップ、または右クリックの「読み込み」から追加できます");
        const int columns = std::max(
            1, int(ImGui::GetContentRegionAvail().x / (size + ImGui::GetStyle().ItemSpacing.x)));
        int index = 0;
        for (const auto& asset : m_models) {
            ImGui::PushID(static_cast<int>(asset.id));
            ImGui::BeginGroup();
            const auto preview = m_modelPreviews.find(asset.id);
            const auto handle = preview != m_modelPreviews.end() && preview->second->HasOutput()
                                    ? preview->second->OutputHandle().ptr
                                    : 0;
            const auto thumb = ui::ThumbnailButton("##model", static_cast<ImTextureID>(handle),
                                                   size, m_selectedModel == asset.id);
            if (thumb.clicked) {
                m_selectedModel = asset.id;
                m_modelLod = 0;
                m_showModelPreview = true;
            }
            if (!asset.geometry) ui::MissingBadge(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (thumb.hovered)
                ImGui::SetTooltip("%s\nクリックでプレビューとプロパティ", asset.name.c_str());
            if (ImGui::BeginPopupContextItem("modelMenu")) {
                if (ImGui::MenuItem("削除")) remove = asset.id;
                ImGui::EndPopup();
            }
            ui::GridCaption(asset.name.c_str(), size);
            ImGui::EndGroup();
            ImGui::PopID();
            if (++index % columns && index < int(m_models.size())) ImGui::SameLine();
        }
        if (ImGui::BeginPopupContextWindow("modelImport", ImGuiPopupFlags_MouseButtonRight |
                                                              ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem("読み込み…"))
                m_pendingModels =
                    ShowOpenFilesDialog(L"モデルを読み込む", {{L"FBXモデル", L"*.fbx"}});
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();
    ImGui::End();
    if (remove) {
        std::erase_if(m_models, [&](const auto& a) { return a.id == remove; });
        MarkDocumentChanged(false);
    }
}
void Application::DrawModelPreviewWindow() {
    m_modelPreviewVisible = false;
    if (!m_showModelPreview) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(440), ui::Scaled(740)), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("モデルプレビュー", &m_showModelPreview,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }
    auto found = std::find_if(m_models.begin(), m_models.end(),
                              [&](const auto& a) { return a.id == m_selectedModel; });
    if (found == m_models.end()) {
        ui::HintText("モデルを選択してください");
        ImGui::End();
        return;
    }
    auto& asset = *found;
    auto previewIt = m_modelPreviews.find(asset.id);
    auto* preview = previewIt != m_modelPreviews.end() ? previewIt->second.get() : nullptr;
    m_modelPreviewVisible = true;
    const float size = PreviewPaneSize();
    ImGui::BeginChild("modelPreviewPane", ImVec2(0, size), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    const float side = std::max(ui::Scaled(32), std::min(ImGui::GetContentRegionAvail().x, size));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         std::max(0.0f, (ImGui::GetContentRegionAvail().x - side) * 0.5f));
    const auto pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##mesh", ImVec2(side, side),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    if (preview) {
        if (ImGui::IsItemActive()) {
            const auto d = ImGui::GetIO().MouseDelta;
            // Camera::Orbit はラジアン。ビューポートと同じ感度でピクセルから変換する。
            constexpr float kOrbitRadiansPerPixel = 0.006f;
            if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::GetIO().KeyShift)
                preview->GetCamera().Pan(d.x, d.y);
            else
                preview->GetCamera().Orbit(d.x * kOrbitRadiansPerPixel, d.y * kOrbitRadiansPerPixel);
        }
        if (ImGui::IsItemHovered()) {
            const auto& io = ImGui::GetIO();
            preview->GetCamera().Zoom(io.MouseWheel);
            if (!io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
                if (ImGui::IsKeyPressed(ImGuiKey_F, false)) preview->FocusView();
                else if (ImGui::IsKeyPressed(ImGuiKey_A, false)) preview->FrameView();
            }
        }
        if (preview->HasOutput())
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(preview->OutputHandle().ptr), pos,
                ImVec2(pos.x + side, pos.y + side));
    }
    ImGui::EndChild();
    ImGui::Separator();
    ImGui::BeginChild("modelProperties", ImVec2(0, 0));
    ui::HintText("左ドラッグ: 回転 / 中・Shift+左ドラッグ: パン / ホイール: ズーム");
    ui::HintText("プレビュー上で F: 中央へフォーカス / A: 全体表示。寸法の単位はm");
    if (preview && ui::Button("視点を戻す", ui::kWideButtonWidth)) preview->ResetView();
    bool changed = false;
    if (ui::BeginPropertyTable("modelBasic")) {
        char name[256];
        std::snprintf(name, sizeof(name), "%s", asset.name.c_str());
        if (ui::PropertyTextInput("名前", name, sizeof(name),
                                  "ファイル名（拡張子なし）と同じ。変えるとファイルも改名する")) {
            if (asset.assetPath.empty()) {
                asset.name = name;
                changed = true;
            } else {
                RequestAssetRename(asset.assetPath, name);
            }
        }
        DrawAssetPathRow("ファイル", asset.path, m_pendingAssetReveal);
        if (asset.geometry) {
            const auto& geo = *asset.geometry;
            ui::PropertyValue("寸法 X / Y / Z", "%.4f / %.4f / %.4f m",
                              geo.maximum.x - geo.minimum.x, geo.maximum.y - geo.minimum.y,
                              geo.maximum.z - geo.minimum.z);
            m_modelLod = std::clamp(m_modelLod, 0, int(geo.lods.size()) - 1);
            ui::PropertyInt("表示LOD", &m_modelLod, 0, int(geo.lods.size()) - 1, 0,
                            "0が最も詳細です。FBXに含まれるLODから選びます");
            ui::PropertyValue("三角形数", "%u", geo.lods[m_modelLod].triangles);
        }
        ui::EndPropertyTable();
    }
    if (!asset.error.empty()) ui::HintText(asset.error.c_str());
    // インポスターを焼いてあれば、メッシュの段の後ろにインポスターの距離を足す。
    if (asset.geometry && (asset.geometry->lods.size() > 1 || asset.impostor.baked)) {
        const size_t count = asset.geometry->lods.size() + (asset.impostor.baked ? 1 : 0);
        ui::SectionHeader("LOD");
        if (ui::BeginPropertyTable("modelLods", "インポスターの距離")) {
            renderer::ModelAsset defaults;
            defaults.geometry = asset.geometry;
            for (size_t lod = 1; lod < count; ++lod) {
                ImGui::PushID(static_cast<int>(lod));
                float value = renderer::LodStartDistance(asset, lod);
                const bool impostorLevel = asset.impostor.baked && lod + 1 == count;
                const std::string label = impostorLevel ? "インポスターの距離" : "LOD" + std::to_string(lod) + " の距離";
                if (ui::PropertyFloat(label.c_str(), &value, 0.0f, 100000.0f,
                                      renderer::LodStartDistance(defaults, lod),
                                      "カメラからこの距離より遠いと、この段階以降を使います。"
                                      "等倍のときの値で、配置の倍率に合わせて伸び縮みします",
                                      "%.1f m")) {
                    // 未設定の段も今の値で埋めてから書き換える。手前の段より近くはしない。
                    std::vector<float> distances(count - 1);
                    for (size_t i = 1; i < count; ++i) distances[i - 1] = renderer::LodStartDistance(asset, i);
                    distances[lod - 1] = std::max(value, lod > 1 ? distances[lod - 2] : 0.0f);
                    asset.lodDistances = std::move(distances);
                    changed = true;
                }
                ImGui::PopID();
            }
            ui::EndPropertyTable();
        }
        ui::HintText("Model Scatter の「LOD 自動」で使います");
    }
    if (asset.geometry) DrawImpostorSection(asset);
    if (asset.geometry) {
        ui::SectionHeader("マテリアルスロット");
        if (ui::BeginPropertyTable("modelMaterials")) {
            for (size_t i = 0; i < asset.geometry->slots.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                const std::string label = "スロット " + std::to_string(i + 1);
                changed |= DrawMaterialSlotRow(label.c_str(), asset.materials[i], m_materialLibrary, m_pendingAssetReveal, false);
                ImGui::PopID();
            }
            ui::EndPropertyTable();
        }
        ui::HintText("既存のマテリアルアセットを割り当てます。未割り当てはグレーで表示します");
    }
    if (changed) {
        MarkDocumentChanged(false);
        m_renderedModelThumbnails.erase(asset.id);
    }
    ImGui::EndChild();
    ImGui::End();
}
// モデルプレビューのインポスター節。作成・作り直し・削除と、焼いた画像での表示の切り替え。
void Application::DrawImpostorSection(renderer::ModelAsset& asset) {
    auto& impostor = asset.impostor;
    bool changed = false;
    static constexpr uint32_t kFrames[] = {8, 12, 16};
    static const char* const kFrameLabels[] = {"8 × 8", "12 × 12", "16 × 16"};
    static constexpr uint32_t kSizes[] = {128, 256, 512};
    static const char* const kSizeLabels[] = {"128 px", "256 px", "512 px"};
    static const char* const kRangeLabels[] = {"半球", "全球"};
    const auto indexOf = [](const auto& values, uint32_t value) {
        for (int i = 0; i < static_cast<int>(std::size(values)); ++i)
            if (values[i] == value) return i;
        return 1;
    };
    ui::SectionHeader("インポスター");
    if (ui::BeginPropertyTable("modelImpostor", "インポスターで表示")) {
        int range = impostor.settings.fullSphere ? 1 : 0;
        if (ui::PropertyCombo("範囲", &range, kRangeLabels, IM_ARRAYSIZE(kRangeLabels), 0,
                              "半球は上から見る物（地面に置く植生や岩）向けで、同じ方向数なら上からの解像度が倍になる。"
                              "下からも見る物は全球")) {
            impostor.settings.fullSphere = range == 1;
            changed = true;
        }
        int frames = indexOf(kFrames, impostor.settings.frames);
        if (ui::PropertyCombo("方向数", &frames, kFrameLabels, IM_ARRAYSIZE(kFrameLabels), 1,
                              "撮る方向の数。多いほど向きの変化が滑らかになるが、画像が大きくなる")) {
            impostor.settings.frames = kFrames[frames];
            changed = true;
        }
        int size = indexOf(kSizes, impostor.settings.frameSize);
        if (ui::PropertyCombo("解像度", &size, kSizeLabels, IM_ARRAYSIZE(kSizeLabels), 1,
                              "1 方向の画像の一辺。画面で大きく見える距離で使うなら上げる")) {
            impostor.settings.frameSize = kSizes[size];
            changed = true;
        }
        if (impostor.baked) {
            const auto& baked = impostor.bakedSettings;
            const uint32_t atlas = baked.frames * baked.frameSize;
            ui::PropertyValue("作成済み", "%s・%u×%u・%u px", baked.fullSphere ? "全球" : "半球",
                              baked.frames, baked.frames, baked.frameSize);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("作成したときの設定。画像は %u px 四方（色・法線・色むらの重みの 3 枚）", atlas);
            ImGui::BeginDisabled(m_impostors.Find(asset.id) == nullptr);
            ui::PropertyBool("インポスターで表示", &m_modelShowImpostor, false,
                             "プレビューを焼いた画像で描き、元のメッシュと見比べる（保存しない）");
            ImGui::EndDisabled();
        } else {
            ui::PropertyValue("作成済み", "なし");
        }
        ui::EndPropertyTable();
    }
    const uint32_t atlas = impostor.settings.frames * impostor.settings.frameSize;
    const bool tooLarge = atlas > 8192;
    if (tooLarge) ui::HintText("画像が大きすぎます。方向数 × 解像度は 8192 px までにしてください");
    else if (impostor.baked && !(impostor.settings == impostor.bakedSettings))
        ui::HintText("設定が作成時と違います。作り直すと反映されます");
    if (impostor.baked && !m_impostors.Find(asset.id))
        ui::HintText("焼いた画像を読み込めません。作り直してください");
    else if (impostor.baked && impostor.variationPath.empty())
        ui::HintText("色むらの重みがありません（幹にも色むらが掛かります）。作り直すと付きます");
    ImGui::BeginDisabled(tooLarge);
    if (ui::Button(impostor.baked ? "作り直す" : "作成", ui::kButtonWidth)) m_pendingImpostorBake = asset.id;
    ImGui::EndDisabled();
    if (impostor.baked) {
        ImGui::SameLine();
        if (ui::Button("削除", ui::kButtonWidth)) ImGui::OpenPopup("インポスターの削除");
    }
    ui::HintText("画像はモデルの横に「名前_Impostor_C.png / _N.png / _V.png」で保存します");
    if (ImGui::BeginPopupModal("インポスターの削除", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(asset.name.c_str());
        ui::HintText("焼いた画像（色・法線・色むらの重み）と .meta を、ルート内の退避フォルダへ移します。");
        if (ui::Button("削除", ui::kButtonWidth)) {
            m_pendingImpostorDelete = asset.id;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ui::Button("キャンセル", ui::kButtonWidth) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (changed) MarkDocumentChanged(false);
}
}  // namespace tg
