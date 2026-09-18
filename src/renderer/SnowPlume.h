#pragma once
#include "renderer/Atmosphere.h"
#include "renderer/ShadowCascades.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <DirectXMath.h>
#include <span>

namespace tg::renderer {

// 雪煙 1 ノードぶん。Application が Snow Plume ノードから写して毎フレーム渡す
// （graph::SnowPlumeSettings と同じ項目。レンダラはグラフを知らないので写しを持つ）。
struct SnowPlumeDraw {
    uint32_t maskIndex = UINT32_MAX;  // Source を評価したマスク（R が 0〜1）
    float windDirectionDegrees = 90.0f, windSpeed = 15.0f;
    int seedsPerSide = 64, sheets = 2, seed = 1;
    float threshold = 0.2f, coverage = 1.0f;
    float lengthMeters = 250.0f, widthStart = 12.0f, widthEnd = 70.0f;
    float lift = 30.0f, sink = 40.0f, opacity = 0.6f, puffSize = 25.0f;
    float turbulence = 0.5f, gust = 0.5f, loopSeconds = 12.0f, anisotropy = 0.6f;
};

// 雪煙を描くのに要る、そのフレームの共通の入力。PreviewRenderer が組み立てる。
struct SnowPlumeFrame {
    DirectX::XMFLOAT4X4 viewProjection;
    DirectX::XMFLOAT3 cameraPosition;
    double seconds = 0;  // 起動からの秒。ノードごとにループ長で巻く
    DirectX::XMFLOAT3 lightDirection, lightColor;
    float lightIlluminance = 0, iblIntensity = 0;
    uint32_t irradianceIndex = UINT32_MAX;
    uint32_t heightIndex = UINT32_MAX;  // 地形のハイト（R32F）。無効なら平らとして扱う
    float planeSize = 1, heightScale = 0;
    uint32_t depthIndex = UINT32_MAX;
    float nearZ = 0.1f, farZ = 1000.0f;
    SceneShadowData shadows;
    AtmosphereSettings atmosphere;
    uint32_t cloudNoiseIndex = 0, atmosphericMode = 0;
};

// シーンカラー（RTV を束ねた状態）へ半透明で重ねる。深度は SRV として読める状態にしておくこと。
// 描いた帯の上限（種の数 × シート数）を返す。
uint32_t DrawSnowPlumes(rhi::PipelineCache& pipelineCache, rhi::Device& device,
                        ID3D12GraphicsCommandList* commandList, DXGI_FORMAT rtvFormat,
                        const SnowPlumeFrame& frame, std::span<const SnowPlumeDraw> plumes);

// 1 本の帯の頂点数（区間 20 × 三角形 2 枚）。シェーダの kSegments と揃える。
inline constexpr uint32_t kSnowPlumeVerticesPerRibbon = 20 * 6;

}  // namespace tg::renderer
