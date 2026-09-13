#pragma once

#include "io/ProjectIo.h"
#include "io/ThumbnailStore.h"
#include "renderer/ModelPreview.h"
#include <unordered_map>

namespace tg {

// シーンに登録せずに作る一覧専用の画像。GPU資源は明示的に遅延解放する。
class AssetThumbnailCache {
public:
    void BeginRequests();
    D3D12_GPU_DESCRIPTOR_HANDLE Request(const std::filesystem::path& path);
    bool Failed(const std::filesystem::path& path) const;
    bool HasPendingWork() const;
    void Invalidate() { m_invalidate = true; }
    void Process(rhi::Device& device, rhi::PipelineCache& pipelines, io::ProjectWorkspace& workspace,
                 const std::filesystem::path& directory, renderer::PreviewRenderer& renderer);
    void Render(rhi::Device& device, rhi::PipelineCache& pipelines, ID3D12GraphicsCommandList* list,
                renderer::PreviewRenderer& renderer);
    void Destroy(rhi::Device& device);
    static bool Supports(const std::filesystem::path& path);

private:
    struct Entry {
        rhi::GpuTexture texture;
        uint64_t lastUsed = 0;
        bool failed = false;
    };
    void ClearScratch(rhi::Device& device);
    bool BuildImage(rhi::Device& device, const std::filesystem::path& path, rhi::GpuTexture& output);
    void Store(rhi::Device& device, const std::filesystem::path& path, rhi::GpuTexture texture, bool persist = true);
    std::unordered_map<std::filesystem::path, Entry> m_entries;
    std::vector<std::filesystem::path> m_requests;
    std::filesystem::path m_root;
    std::filesystem::path m_directory;
    io::ThumbnailRecord m_diskRecord;
    std::filesystem::path m_pendingModel;
    uint64_t m_frame = 0;
    bool m_invalidate = false;
    bool m_modelRendered = false;
    compositor::TextureLibrary m_textures;
    compositor::MaterialLibrary m_materials;
    renderer::SkyLibrary m_skies;
    std::vector<renderer::ModelAsset> m_models;
    renderer::ModelPreview m_modelPreview;
};

}  // namespace tg
