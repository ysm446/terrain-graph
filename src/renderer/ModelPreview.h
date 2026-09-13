#pragma once
#include "renderer/MaterialSphere.h"
#include "renderer/ModelAsset.h"
namespace tg::renderer {
class ModelPreview {
   public:
    void Destroy(rhi::Device& device);
    bool Prepare(rhi::Device& device, const ModelAsset& asset, int lod);
    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                const compositor::MaterialLibrary& materials,
                const compositor::TextureLibrary& textures, const Environment& environment,
                float iblIntensity, const LightSettings& light, float exposure,
                TonemapMode tonemap);
    Camera& GetCamera() { return m_camera; }
    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }
    void ResetView();
    void FocusView();
    void FrameView();

   private:
    std::shared_ptr<const ModelGeometry> m_geometry;
    int m_lod = -1;
    std::vector<Mesh> m_meshes;
    rhi::GpuTexture m_output, m_depth;
    Camera m_camera;
};
}  // namespace tg::renderer
