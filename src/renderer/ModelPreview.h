#pragma once
#include "renderer/Atmosphere.h"
#include "renderer/ShadowCascades.h"
#include "renderer/MaterialSphere.h"
#include "renderer/ModelAsset.h"
#include <utility>
namespace tg::renderer {
// Prepare に渡すと全 LOD（先頭から kMaxInstanceLods 段まで）を用意し、距離で選んで描く。
inline constexpr int kAllLods = -1;
inline constexpr size_t kMaxInstanceLods = 4;
struct ModelInstanceDraw {
    uint32_t points = 0, rows = 0, count = 0, seed = 1;
    float weightStart = 0, weightEnd = 1;
    float scaleMin = 1, scaleMax = 1, align = 1, offset = 0;
    bool usePointSize = true, shadow = false;
    float maxDistance = 0;
    // 全 LOD を用意したときだけ使う。lodBias はモデルの切り替え距離に掛ける倍率。
    // fadeBand は切り替え距離に対する重ね合わせの幅（0.15 なら距離の 15% をかけて移る）。
    float lodBias = 1, fadeBand = 0.15f;
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
        ID3D12GraphicsCommandList* list, const ModelAsset& model, const ModelInstanceDraw& draw);
    size_t LodCount() const { return m_lodFirstPart.empty() ? 0 : m_lodFirstPart.size() - 1; }
    // 可視リストの区画。LOD ごとの通常の区画 [0, L) と、切り替え中の区画 [L, 2L)。
    // 1 段だけなら切り替えが無いので 1 区画。
    size_t SegmentCount() const { return LodCount() > 1 ? LodCount() * 2 : LodCount(); }
    rhi::GpuBuffer m_visibleInstances, m_indirectArguments;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> m_drawSignature;
    std::shared_ptr<const ModelGeometry> m_geometry;
    int m_requestedLod = 0;
    bool m_ready = false;
    std::vector<Mesh> m_meshes;
    // m_meshes と同じ並び。lod は用意した段の中での番号（0 が m_firstLod）。
    struct Part { uint32_t lod = 0, slot = 0; };
    std::vector<Part> m_parts;
    std::vector<size_t> m_lodFirstPart;  // 段ごとの先頭パーツ。末尾に総数
    size_t m_firstLod = 0;
    std::vector<uint32_t> m_segmentFirstArgument;  // 区画ごとの先頭の描画引数
    rhi::GpuTexture m_output, m_depth;
    Camera m_camera;
};
}  // namespace tg::renderer
