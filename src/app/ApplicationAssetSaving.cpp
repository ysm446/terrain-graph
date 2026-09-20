// アセット欄の選択保存と、保存済みの内容との比較。
#include "app/Application.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "io/ProjectIo.h"
#include <utility>

namespace tg {
void Application::RememberAssetStates(bool saved) {
    io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
        m_skyLibrary, m_renderer, m_graph, &m_models, &m_sceneComponents, -1, &m_sceneAtmosphere};
    m_assetStates = io::FingerprintAssets(refs);
    for (const auto& [path, state] : m_assetStates) {
        if (saved) m_savedAssetStates[path] = state;
        else m_savedAssetStates.try_emplace(path, state);
    }
}

bool Application::IsAssetDirty(const std::filesystem::path& path) const {
    if (const auto found = m_assetStates.find(path); found != m_assetStates.end()) {
        const auto saved = m_savedAssetStates.find(path);
        if (saved != m_savedAssetStates.end() && saved->second != found->second) return true;
    }
    if (path == m_projectPath) return (m_sceneDirty & kDirtyScene) != 0;
    if (path == m_componentPreviewPath && m_componentPreview >= 0)
        return m_componentPreview == 1 ? m_currentFingerprint.cloud != m_previewSavedFingerprint.cloud :
            m_currentFingerprint.terrain != m_previewSavedFingerprint.terrain;
    if (m_sceneComponents.is_array()) for (const auto& component : m_sceneComponents) {
        if (!component.contains("asset") || m_workspace.Resolve(component["asset"]) != path) continue;
        const auto role = io::ProjectWorkspace::String(component, "role");
        return (m_sceneDirty & (role == "cloud" ? kDirtyCloud : kDirtyTerrain)) != 0;
    }
    return !m_sceneAtmosphere.is_null() && m_workspace.Resolve(m_sceneAtmosphere) == path &&
        (m_sceneDirty & kDirtyAtmosphere) != 0;
}

void Application::RequestSaveSelection() {
    CommitMaterialEdit();
    if (m_selectedAssets.empty()) { RequestSaveProject(false); return; }
    m_pendingSelectedAssetSave = m_selectedAssets;
}

void Application::ProcessSelectedAssetSave() {
    if (m_pendingSelectedAssetSave.empty()) return;
    const auto paths = std::exchange(m_pendingSelectedAssetSave, {});
    CommitMaterialEdit();
    RefreshSceneDirty();
    io::ProjectRefs refs{m_textureLibrary, m_materialLibrary, m_paintMasks,
        m_skyLibrary, m_renderer, m_graph, &m_models, &m_sceneComponents, -1, &m_sceneAtmosphere};
    refs.saveSharedAssets = false;
    size_t savedCount = 0, failedCount = 0;
    for (const auto& path : paths) {
        if (!m_workspace.Contains(path) || !IsAssetDirty(path)) continue;
        bool saved = false;
        if (m_assetStates.contains(path)) {
            saved = io::SaveSharedAssets(m_workspace, refs, &path);
            if (saved) {
                RememberAssetStates();
                m_savedAssetStates[path] = m_assetStates.at(path);
            }
        } else {
            int component = -1;
            if (path == m_componentPreviewPath && m_componentPreview >= 0) component = m_componentPreview;
            if (component < 0 && m_sceneComponents.is_array()) for (const auto& entry : m_sceneComponents)
                if (entry.contains("asset") && m_workspace.Resolve(entry["asset"]) == path)
                    component = io::ProjectWorkspace::String(entry, "role") == "cloud" ? 1 : 0;
            if (!m_sceneAtmosphere.is_null() && m_workspace.Resolve(m_sceneAtmosphere) == path) component = 2;
            if (component >= 0) {
                refs.componentOnly = component;
                saved = io::SaveProject(m_workspace.Root() / L".terrain-graph/editor/Selection.tgscene", m_device, refs, &m_workspace);
                if (saved) {
                    if (path == m_componentPreviewPath) m_previewSavedFingerprint = io::FingerprintScene(refs);
                    else MarkSceneSaved(1u << component);
                }
            } else if (path == m_projectPath) {
                refs.componentOnly = -1; refs.componentWrite = 0;
                saved = io::SaveProject(path, m_device, refs, &m_workspace);
                if (saved) { MarkSceneSaved(kDirtyScene); SaveSceneThumbnail(path); }
            }
        }
        if (saved) ++savedCount;
        else { ++failedCount; TG_LOG_ERROR("アセットを保存できませんでした: %s", ToUtf8Display(path).c_str()); }
    }
    RefreshSceneDirty();
    m_assetRefresh = true;
    if (savedCount) {
        m_assetThumbnails.Invalidate();
        m_toasts.Push(std::to_string(savedCount) + " 件のアセットを保存しました");
    } else if (!failedCount) m_toasts.Push("選択したアセットに保存する変更はありません");
    if (failedCount) m_toasts.Push("保存に失敗したアセットがあります", "変更は未保存のまま保持しています");
}
} // namespace tg
