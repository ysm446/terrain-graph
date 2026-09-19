#include "renderer/SnowPlume.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tg::renderer {
namespace {

// SnowPlume.hlsl の SnowPlumeConstants と同じ並び。
struct SnowPlumeConstants {
    DirectX::XMFLOAT4X4 viewProjection;
    float cameraPosition[3];
    float time;
    float lightDirection[3];
    float lightIlluminance;
    float lightColor[3];
    float iblIntensity;
    uint32_t maskIndex, heightIndex, depthIndex, irradianceIndex;
    float planeSize, heightScale, nearZ, farZ;
    float wind[2];
    float windSpeed;
    uint32_t seedsPerSide;
    float threshold, coverage, lengthMeters, widthStart;
    float widthEnd, lift, sink, opacity;
    float puffSize, turbulence, gust, loopSeconds;
    float anisotropy;
    uint32_t seed, sheets, useHeight;
    SceneShadowData shadows;
    AtmosphereSettings atmosphere;
    uint32_t cloudNoiseIndex, atmosphericMode;
    float upwind, slopeFollow;
    uint32_t cloudIndex, cloudDepthIndex;
    float cloudFarDistance;
    uint32_t clearIrradianceIndex;
    float ambientLow, ambientHigh, ambientOcclusion, pad;
};
static_assert(sizeof(SnowPlumeConstants) % 16 == 0);

}  // namespace

uint32_t DrawSnowPlumes(rhi::PipelineCache& pipelineCache, rhi::Device& device,
                        ID3D12GraphicsCommandList* commandList, DXGI_FORMAT rtvFormat,
                        const SnowPlumeFrame& frame, std::span<const SnowPlumeDraw> plumes) {
    if (plumes.empty() || frame.depthIndex == UINT32_MAX) return 0;
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"SnowPlume.hlsl";
    desc.vertexEntry = L"VsMain";
    desc.pixelEntry = L"PsMain";
    desc.rtvFormat = rtvFormat;
    desc.layout = rhi::VertexLayout::None;
    desc.cullMode = D3D12_CULL_MODE_NONE;
    // 深度はシェーダで読み比べる（ソフトな縁のため）。深度バッファは束ねない。
    desc.depthTest = false;
    desc.depthWrite = false;
    desc.alphaBlend = true;
    ID3D12PipelineState* pipeline = pipelineCache.GetGraphics(desc);
    if (pipeline == nullptr) return 0;

    PIXBeginEvent(commandList, PIX_COLOR(230, 240, 255), "SnowPlume");
    commandList->SetGraphicsRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 0, nullptr);
    commandList->IASetIndexBuffer(nullptr);

    uint32_t ribbons = 0;
    for (const SnowPlumeDraw& plume : plumes) {
        if (plume.maskIndex == UINT32_MAX || plume.maskIndex == UINT32_MAX - 1) continue;
        const auto allocation = device.Upload().Allocate(sizeof(SnowPlumeConstants), 256);
        if (!allocation.IsValid()) break;
        SnowPlumeConstants c = {};
        c.viewProjection = frame.viewProjection;
        std::memcpy(c.cameraPosition, &frame.cameraPosition, sizeof(c.cameraPosition));
        const float loop = std::max(plume.loopSeconds, 1.0f);
        c.loopSeconds = loop;
        c.time = static_cast<float>(std::fmod(frame.seconds, static_cast<double>(loop)));
        std::memcpy(c.lightDirection, &frame.lightDirection, sizeof(c.lightDirection));
        std::memcpy(c.lightColor, &frame.lightColor, sizeof(c.lightColor));
        c.lightIlluminance = frame.lightIlluminance;
        c.iblIntensity = frame.iblIntensity;
        c.maskIndex = plume.maskIndex;
        c.heightIndex = frame.heightIndex;
        c.useHeight = frame.heightIndex != UINT32_MAX && frame.heightScale != 0.0f ? 1u : 0u;
        c.depthIndex = frame.depthIndex;
        c.irradianceIndex = frame.irradianceIndex;
        c.planeSize = frame.planeSize;
        c.heightScale = frame.heightScale;
        c.nearZ = frame.nearZ;
        c.farZ = frame.farZ;
        // 風向は雲・Wind Field と同じ：0 が +Z、90 が +X。
        const float radians = plume.windDirectionDegrees * 3.14159265f / 180.0f;
        c.wind[0] = std::sin(radians);
        c.wind[1] = std::cos(radians);
        c.windSpeed = std::max(plume.windSpeed, 0.0f);
        c.seedsPerSide = static_cast<uint32_t>(std::clamp(plume.seedsPerSide, 4, 256));
        c.threshold = std::clamp(plume.threshold, 0.0f, 0.99f);
        c.coverage = std::clamp(plume.coverage, 0.0f, 1.0f);
        c.lengthMeters = std::max(plume.lengthMeters, 1.0f);
        c.widthStart = std::max(plume.widthStart, 0.1f);
        c.widthEnd = std::max(plume.widthEnd, 0.1f);
        c.lift = plume.lift;
        c.sink = plume.sink;
        c.upwind = std::max(plume.upwind, 0.0f);
        c.slopeFollow = std::clamp(plume.slopeFollow, 0.0f, 1.0f);
        c.opacity = std::clamp(plume.opacity, 0.0f, 1.0f);
        c.puffSize = std::max(plume.puffSize, 1.0f);
        c.turbulence = std::clamp(plume.turbulence, 0.0f, 1.0f);
        c.gust = std::clamp(plume.gust, 0.0f, 1.0f);
        c.anisotropy = std::clamp(plume.anisotropy, 0.0f, 0.95f);
        c.seed = static_cast<uint32_t>(plume.seed);
        c.sheets = static_cast<uint32_t>(std::clamp(plume.sheets, 1, 3));
        c.shadows = frame.shadows;
        c.atmosphere = frame.atmosphere;
        c.cloudNoiseIndex = frame.cloudNoiseIndex;
        c.atmosphericMode = frame.atmosphericMode;
        c.cloudIndex = frame.cloudIndex;
        c.cloudDepthIndex = frame.cloudDepthIndex;
        c.cloudFarDistance = frame.cloudFarDistance;
        c.clearIrradianceIndex = frame.ambient.clearIrradianceIndex;
        c.ambientLow = frame.ambient.low;
        c.ambientHigh = frame.ambient.high;
        c.ambientOcclusion = frame.ambient.occlusion;
        std::memcpy(allocation.cpu, &c, sizeof(c));

        const uint32_t instances = c.seedsPerSide * c.seedsPerSide * c.sheets;
        commandList->SetGraphicsRootConstantBufferView(1, allocation.gpuAddress);
        commandList->DrawInstanced(kSnowPlumeVerticesPerRibbon, instances, 0, 0);
        ribbons += instances;
    }
    PIXEndEvent(commandList);
    return ribbons;
}

}  // namespace tg::renderer
