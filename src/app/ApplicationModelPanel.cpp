#include <imgui.h>

#include <algorithm>
#include <cstdio>

#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "ui/UiStyle.h"

namespace tg {
void Application::PrepareModelScatters() {
    m_modelScatters = m_graph.CompileModelScatters();
    std::vector<graph::GraphId> sources;
    std::vector<std::string> meshKeys;
    for (const auto& scatter : m_modelScatters) {
        for (const auto& choice : scatter.settings.models) {
            const auto model = std::find_if(m_models.begin(),m_models.end(),[&](const auto& m) { return m.id == choice.model; });
            if (model == m_models.end() || !model->geometry || choice.weight <= 0) continue;
            const auto key = std::to_string(choice.model)+":"+std::to_string(scatter.settings.lod);
            meshKeys.push_back(key);
            auto& mesh = m_instanceMeshes[key];
            if (!mesh) mesh = std::make_unique<renderer::ModelPreview>();
            mesh->Prepare(m_device,*model,scatter.settings.lod);
        }
        if (std::find(sources.begin(),sources.end(),scatter.source) != sources.end()) continue;
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
                    compiled.layers[i].maskOnly = true; compiled.layers[i].emitPoints = true;
                }
            slot->stack.Layers() = std::move(compiled.layers);
            slot->stack.MaskOps() = std::move(compiled.maskOps);
            slot->stack.MarkDirty();
            slot->evaluator.Invalidate();
            slot->graphRevision = m_graph.TerrainRevision();
            slot->documentRevision = m_graphStack.Revision();
            slot->paintRevision = m_paintMasks.Revision();
        }
        slot->evaluator.CaptureCrumblingPoints(true);
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
void Application::DrawModelScatters(ID3D12GraphicsCommandList* commandList,
                                   const DirectX::XMFLOAT4X4& viewProjection, bool shadow) {
    for (const auto& scatter : m_modelScatters) {
        const auto pointSlot = m_modelPoints.find(scatter.source);
        if (pointSlot == m_modelPoints.end()) continue;
        const auto& evaluator = pointSlot->second->evaluator;
        if (!evaluator.CrumblingPoints().IsValid() || !evaluator.CrumblingPointCount() || evaluator.HasPendingPostprocess()) continue;
        float total = 0;
        for (const auto& choice : scatter.settings.models)
            if (std::any_of(m_models.begin(),m_models.end(),[&](const auto& m){return m.id==choice.model && m.geometry;})) total += std::max(choice.weight,0.0f);
        if (total <= 0) continue;
        float cumulative = 0;
        for (const auto& choice : scatter.settings.models) {
            const auto model=std::find_if(m_models.begin(),m_models.end(),[&](const auto& m){return m.id==choice.model;});
            if (model==m_models.end() || !model->geometry || choice.weight<=0) continue;
            const auto key=std::to_string(choice.model)+":"+std::to_string(scatter.settings.lod);
            const auto mesh=m_instanceMeshes.find(key);
            if (mesh==m_instanceMeshes.end()) continue;
            renderer::ModelInstanceDraw draw;
            draw.points=evaluator.CrumblingPoints().SrvIndex();
            draw.rows=evaluator.CrumblingPoints().height/2; draw.count=evaluator.CrumblingPointCount();
            draw.seed=static_cast<uint32_t>(scatter.settings.seed);
            draw.weightStart=cumulative/total; cumulative+=choice.weight; draw.weightEnd=cumulative/total;
            draw.scaleMin=scatter.settings.scaleMin; draw.scaleMax=std::max(draw.scaleMin,scatter.settings.scaleMax);
            draw.align=scatter.settings.alignToNormal; draw.offset=scatter.settings.offset;
            draw.usePointSize=scatter.settings.usePointSize; draw.shadow=shadow;
            draw.viewProjection=viewProjection; draw.cameraPosition=m_renderer.GetCamera().Position();
            if (!shadow) draw.shadows=m_renderer.InstanceShadows();
            const auto& lod=model->geometry->lods[std::min(scatter.settings.lod,static_cast<int>(model->geometry->lods.size())-1)];
            for (const auto& part : lod.parts)
                m_renderer.RecordInstanceDraw(static_cast<uint32_t>(part.mesh.indices.size()),draw.count);
            mesh->second->Render(m_device,m_pipelineCache,commandList,*model,m_materialLibrary,m_textureLibrary,
                m_renderer.GetEnvironment(),m_renderer.EnvironmentIntensity(),m_renderer.EffectiveLight(),
                m_renderer.Exposure().Exposure(),m_renderer.Tonemap(),&draw);
        }
    }
}

void Application::ProcessModelWork() {
    for (const auto& asset : m_models) m_nextModelId = std::max(m_nextModelId, asset.id + 1);
    for (const auto& path : m_pendingModels) {
        renderer::ModelAsset asset;
        asset.id = m_nextModelId++;
        asset.name = ToUtf8Display(path.stem());
        asset.path = path;
        if (renderer::LoadModel(path, asset)) {
            m_selectedModel = asset.id;
            m_modelLod = 0;
            m_showModelPreview = true;
            m_focusModelLibrary = true;
            m_models.push_back(std::move(asset));
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
}
void Application::RenderModelPreviews(ID3D12GraphicsCommandList* commandList) {
    for (const auto& asset : m_models) {
        const auto found = m_modelPreviews.find(asset.id);
        if (found == m_modelPreviews.end()) continue;
        if (!(m_modelPreviewVisible && asset.id == m_selectedModel) &&
            m_renderedModelThumbnails.contains(asset.id))
            continue;
        found->second->Render(m_device, m_pipelineCache, commandList, asset, m_materialLibrary,
                              m_textureLibrary, m_renderer.GetEnvironment(),
                              m_renderer.EnvironmentIntensity(), m_renderer.EffectiveLight(),
                              m_renderer.Exposure().Exposure(), m_renderer.Tonemap());
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
        if (ui::PropertyTextInput("名前", name, sizeof(name))) {
            asset.name = name;
            changed = true;
        }
        ui::PropertyValue("ファイル", "%s", ToUtf8Display(asset.path.filename()).c_str());
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
    if (asset.geometry) {
        ui::SectionHeader("マテリアルスロット");
        std::vector<const char*> names = {"未割り当て"};
        for (const auto& mat : m_materialLibrary.Entries()) names.push_back(mat.name.c_str());
        if (ui::BeginPropertyTable("modelMaterials")) {
            for (size_t i = 0; i < asset.geometry->slots.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                int selected = 0;
                const auto& entries = m_materialLibrary.Entries();
                for (size_t j = 0; j < entries.size(); ++j)
                    if (entries[j].id == asset.materials[i]) selected = int(j) + 1;
                const std::string label = "スロット " + std::to_string(i + 1);
                if (ui::PropertyCombo(label.c_str(), &selected, names.data(), int(names.size()), 0,
                                      asset.geometry->slots[i].c_str())) {
                    asset.materials[i] =
                        selected ? entries[selected - 1].id : compositor::kNoMaterialAsset;
                    changed = true;
                }
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
}  // namespace tg
