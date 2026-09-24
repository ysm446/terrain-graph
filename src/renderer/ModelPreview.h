#pragma once
#include "renderer/Atmosphere.h"
#include "renderer/ShadowCascades.h"
#include "renderer/MaterialSphere.h"
#include "renderer/Impostor.h"
#include "renderer/ModelAsset.h"
#include <array>
#include <utility>
namespace tg::renderer {
// Prepare に渡すと全 LOD（先頭から kMaxInstanceLods 段まで）を用意し、距離で選んで描く。
inline constexpr int kAllLods = -1;
inline constexpr size_t kMaxInstanceLods = 4;
// LOD の色分け表示の色（リニア）。UE5 の LOD Coloration と同じ並び。固定 LOD は 8 段目以降も最後の色。
inline constexpr DirectX::XMFLOAT3 kLodDebugColors[] = {
    {1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
    {1.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.0f},
};
struct ModelInstanceDraw {
    uint32_t points = 0, rows = 0, count = 0, seed = 1;
    float weightStart = 0, weightEnd = 1;
    float scaleMin = 1, scaleMax = 1, align = 1, offset = 0;
    bool usePointSize = true, shadow = false;
    bool lodView = false;  // LOD の色分け表示（ベースカラーを段の色にする）
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
// インスタンス描画で GPU が実際に描いた量。カリングと LOD の選択の結果を読み戻して数える。
struct InstanceStats {
    // 本描画と影の合計（IA が読む量）。
    uint64_t vertices = 0, triangles = 0;
    // 本描画で描いた株の数。添字はモデルの LOD 番号。切り替え中の株は両方の段に数える。
    std::array<uint64_t, kMaxInstanceLods> instances{};
};
class ModelPreview {
   public:
    void Destroy(rhi::Device& device);
    bool Prepare(rhi::Device& device, const ModelAsset& asset, int lod);
    // 完了したフレームの集計を読み戻す。フレームの記録を始めた後、描画より前に呼ぶ。
    void CollectInstanceStats(rhi::Device& device);
    // 直近に完了したフレームの集計。1〜2 フレーム遅れる。
    const InstanceStats& LatestInstanceStats() const { return m_instanceStats; }
    // 発行した描画の回数を返す。
    uint32_t Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                ID3D12GraphicsCommandList* commandList, const ModelAsset& model,
                const compositor::MaterialLibrary& materials,
                const compositor::TextureLibrary& textures, const Environment& environment,
                float iblIntensity, const LightSettings& light, float exposure,
                TonemapMode tonemap, const ModelInstanceDraw* instances = nullptr,
                const ImpostorTextures* impostor = nullptr);
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
    // 区画ごとの件数を 1 フレームぶん足し込む（本描画 [0, 8)、影 [8, 16)）。
    // 同じメッシュを影の各段・本描画・複数の配置で使い回すので、描画引数は上書きされる。
    static constexpr uint32_t kStatSlots = kMaxInstanceLods * 2 * 2;
    rhi::GpuBuffer m_statCounters;
    rhi::GpuBuffer m_statReadback[rhi::kFrameCount];
    uint64_t m_statFence[rhi::kFrameCount]{};   // 読み戻しを記録したフレームのフェンス値。0 は空
    size_t m_statLodCount[rhi::kFrameCount]{};  // 記録したときの段数（作り直した後の読み違いを防ぐ）
    uint64_t m_statFrame = 0;                   // 足し込み中のフレームのフェンス値
    uint64_t m_statCollected = 0;
    InstanceStats m_instanceStats;
    rhi::GpuTexture m_output, m_depth;
    Camera m_camera;
};
}  // namespace tg::renderer
