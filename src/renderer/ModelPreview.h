#pragma once
#include "renderer/Atmosphere.h"
#include "renderer/ShadowCascades.h"
#include "renderer/MaterialSphere.h"
#include "renderer/ModelAsset.h"
#include <utility>
namespace tg::renderer {
struct ModelInstanceDraw {
    uint32_t points = 0, rows = 0, count = 0, seed = 1;
    float weightStart = 0, weightEnd = 1;
    float scaleMin = 1, scaleMax = 1, align = 1, offset = 0;
    bool usePointSize = true, shadow = false;
    float maxDistance = 0;
    SceneShadowData shadows;
    // 雲影。地形（MeshPbr）と同じ大気設定で CloudShadow を引く。atmosphericMode が 0 なら掛けない。
    AtmosphereSettings atmosphere;
    uint32_t cloudNoiseIndex = 0, atmosphericMode = 0;
    // 環境光を雲あり / 雲なしで混ぜる値（地形と同じ。Atmosphere::CurrentAmbientBlend）。
    Atmosphere::AmbientBlend ambient;
    DirectX::XMFLOAT4X4 viewProjection;
    DirectX::XMFLOAT3 cameraPosition;
};
class ModelPreview {
   public:
    void Destroy(rhi::Device& device);
    bool Prepare(rhi::Device& device, const ModelAsset& asset, int lod);
    void Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                const compositor::MaterialLibrary& materials,
                const compositor::TextureLibrary& textures, const Environment& environment,
                float iblIntensity, const LightSettings& light, float exposure,
                TonemapMode tonemap, const ModelInstanceDraw* instances = nullptr);
    Camera& GetCamera() { return m_camera; }
    bool HasOutput() const { return m_output.IsValid(); }
    D3D12_GPU_DESCRIPTOR_HANDLE OutputHandle() const { return m_output.srv.gpu; }
    rhi::GpuTexture TakeOutput() { return std::exchange(m_output, {}); }
    void ResetView();
    void FocusView();
    void FrameView();

   private:
    bool CullInstances(rhi::Device& device, rhi::PipelineCache& cache,
        ID3D12GraphicsCommandList* list, const ModelInstanceDraw& draw);
    rhi::GpuBuffer m_visibleInstances, m_indirectArguments;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_drawSignature;
    std::shared_ptr<const ModelGeometry> m_geometry;
    int m_lod = -1;
    std::vector<Mesh> m_meshes;
    rhi::GpuTexture m_output, m_depth;
    Camera m_camera;
};
}  // namespace tg::renderer
