#include "renderer/Atmosphere.h"
#include "core/Log.h"
#include <pix3.h>
#include <cstring>
#include <algorithm>
#include <cmath>

namespace tg::renderer {
namespace {
constexpr auto ReadState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
bool CreateTarget(rhi::Device& device, rhi::GpuTexture& texture, uint32_t size, DXGI_FORMAT format, uint32_t arraySize = 1, uint32_t height = 0) {
    rhi::TextureDesc desc;
    desc.width = size;
    desc.height = height != 0 ? height : size;
    desc.format = format;
    desc.arraySize = arraySize;
    desc.allowUnorderedAccess = true;
    desc.createSrv = true;
    desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.debugName = L"AtmosphereCache";
    return device.Allocator().CreateTexture2D(desc, texture);
}
}
void Atmosphere::ResetCloudMotion() {
    m_motion.Reset();
    m_lastTick = {};
    m_lastCloudEdit = std::chrono::steady_clock::now();
    m_cloudEnvironmentDirty = true;
}
void Atmosphere::ResetAnimation() {
    ResetCloudMotion();
    m_cloudTime = m_environmentTime = 0.0f;
    m_ready = false;
}
bool Atmosphere::Update(rhi::Device& device, rhi::PipelineCache& pipelines, const AtmosphereSettings& requested) {
    if (requested.localCloud == 2 && !m_opticalDepth.IsValid()) {
        if (!CreateTarget(device, m_opticalDepth, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 32)) return false;
        m_opticalDirty = true;
    }
    const auto now = std::chrono::steady_clock::now();
    const float delta = !m_ready || m_lastTick.time_since_epoch().count() == 0 ? 0.0f :
        std::clamp(std::chrono::duration<float>(now-m_lastTick).count(), 0.0f, 0.1f);
    m_lastTick = now;
    if (requested.localCloud != m_requested.localCloud ||
        requested.cloudSource != m_requested.cloudSource ||
        requested.cloudMotionMode != m_requested.cloudMotionMode) m_motion.Reset();
    const bool playing = requested.clouds && requested.animateClouds && requested.windSpeed > 0;
    m_motion.Advance(delta, playing, requested.windSpeed, requested.windDirection, requested.noiseSpeedRatio);
    if (playing) m_cloudTime += delta;
    AtmosphereSettings settings = requested;
    settings.cloudCellIndex = m_cloudCells.SrvIndex();
    if (requested.localCloud && requested.cloudMotionMode != 1) {
        settings.fieldCenterX += static_cast<float>(m_motion.x);
        settings.fieldCenterZ += static_cast<float>(m_motion.z);
        // 範囲より模様を遅く進める。追加サンプルなしで異なる場所の密度を読む。
        settings.windOffsetX = m_motion.LocalNoiseOffset(m_motion.driftX, requested.cloudScale, requested.cloudMotionMode);
        settings.windOffsetZ = m_motion.LocalNoiseOffset(m_motion.driftZ, requested.cloudScale, requested.cloudMotionMode);
    } else {
        // ローカル雲の細部は周波数 3.1 倍なので、共通周期は基準周期の 10 倍。
        const double period = requested.cloudScale * (requested.localCloud ? 10.0 : 1.0);
        settings.windOffsetX = static_cast<float>(std::fmod(m_motion.x, period));
        settings.windOffsetZ = static_cast<float>(std::fmod(m_motion.z, period));
        if (requested.localCloud == 2) {
            const double bodyPeriod = requested.cloudScale * std::clamp(requested.cloudCellCount, 1u, 32u);
            settings.cloudBodyOffsetX = static_cast<float>(std::fmod(m_motion.x, bodyPeriod));
            settings.cloudBodyOffsetZ = static_cast<float>(std::fmod(m_motion.z, bodyPeriod));
            // 積算済みの相対移動を使い、速度比の編集時に表面を飛ばさない。
            settings.windOffsetX = static_cast<float>(std::fmod(m_motion.x-m_motion.driftX, period));
            settings.windOffsetZ = static_cast<float>(std::fmod(m_motion.z-m_motion.driftZ, period));
        }
    }
    const bool changed = !m_ready || std::memcmp(&requested, &m_requested, sizeof(requested)) != 0;
    const auto& baked = m_environmentSettings;
    const bool skyChanged = !m_ready || settings.azimuth != baked.azimuth || settings.elevation != baked.elevation ||
        settings.illuminance != baked.illuminance || settings.density != baked.density || settings.mie != baked.mie ||
        settings.eccentricity != baked.eccentricity || settings.altitude != baked.altitude ||
        settings.groundAlbedo != baked.groundAlbedo || settings.lowerHemisphere != baked.lowerHemisphere ||
        settings.cloudSkylightIntensity != baked.cloudSkylightIntensity;
    const bool updateLut = !m_ready || settings.density != baked.density || settings.mie != baked.mie || settings.groundAlbedo != baked.groundAlbedo;
    const bool updateNoise = !m_ready || settings.seed != baked.seed;
    const bool updateCells = updateNoise || m_cellsDirty;
    if (settings.localCloud && m_ready) {
        if (changed) m_lastCloudEdit = now;
        if (changed || playing) m_cloudEnvironmentDirty = true;
        const bool settled = !requested.animateClouds &&
            std::chrono::duration<float>(now-m_lastCloudEdit).count() >= 0.3f;
        // 本体・影は即時反映。再生・ドラッグ中は環境の GPU 待機を挟まない。
        // 空・シード・ノイズ種類の変更は再生中でも環境へ即時に反映する。
        const bool sourceChanged = settings.localCloud != baked.localCloud || settings.cloudSource != baked.cloudSource ||
            settings.cloudNoiseType != baked.cloudNoiseType ||
            settings.cloudCellCount != baked.cloudCellCount ||
            (baked.distributionMask == UINT32_MAX-1 && settings.distributionMask != UINT32_MAX-1);
        if (!skyChanged && !updateCells && !sourceChanged && !(m_cloudEnvironmentDirty && settled)) {
            m_applied = settings;
            m_requested = requested;
            return true;
        }
    } else if (!changed && !updateCells && m_cloudTime-m_environmentTime < 1.0f) {
        m_applied = settings;
        return true;
    }
    if (!m_initialized) {
        if (!m_environment.Initialize(device, pipelines, false) ||
            !CreateTarget(device, m_multiScatter, 32, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !CreateTarget(device, m_noise, 64, DXGI_FORMAT_R16G16B16A16_FLOAT, 64) ||
            !CreateTarget(device, m_skyView, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 256) ||
            !CreateTarget(device, m_cloudLighting, 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1) ||
            !CreateTarget(device, m_cloudCells, 32, DXGI_FORMAT_R32G32B32A32_FLOAT, 3)) {
            Shutdown(device);
            return false;
        }
        m_initialized = true;
    }
    auto* lutPipeline = pipelines.GetCompute(L"AtmosphereMultiScatter.hlsl", L"CSGenerate");
    auto* noisePipeline = pipelines.GetCompute(L"AtmosphereCloudDensity.hlsl", L"CSGenerate");
    auto* cellPipeline = pipelines.GetCompute(L"AtmosphereCloudCells.hlsl", L"CsMain");
    if (!lutPipeline || !noisePipeline || !cellPipeline) return false;
    settings.cloudCellIndex = m_cloudCells.SrvIndex();
    if ((updateLut || updateNoise || updateCells) && !device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        PIXBeginEvent(commands, PIX_COLOR(120, 180, 255), "AtmosphereCaches");
        commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
        if (updateLut) {
            TransitionIfNeeded(commands, m_multiScatter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            struct Constants { float density, mie, groundAlbedo; uint32_t output; };
            const Constants constants{settings.density, settings.mie, settings.groundAlbedo, m_multiScatter.UavIndex()};
            commands->SetPipelineState(lutPipeline);
            commands->SetComputeRoot32BitConstants(0, 4, &constants, 0);
            commands->Dispatch(4, 4, 1);
            TransitionIfNeeded(commands, m_multiScatter, ReadState);
        }
        if (updateCells) {
            PIXBeginEvent(commands, PIX_COLOR(120,180,255), "CloudCellCache");
            TransitionIfNeeded(commands, m_cloudCells, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const uint32_t constants[]{settings.seed, m_cloudCells.UavIndex(), 0, 0};
            commands->SetPipelineState(cellPipeline);
            commands->SetComputeRoot32BitConstants(0, 4, constants, 0);
            commands->Dispatch(4, 4, 1);
            TransitionIfNeeded(commands, m_cloudCells, ReadState);
            PIXEndEvent(commands);
        }
        if (updateNoise) {
            TransitionIfNeeded(commands, m_noise, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const uint32_t constants[]{64, settings.seed, m_noise.UavIndex(), 0};
            commands->SetPipelineState(noisePipeline);
            commands->SetComputeRoot32BitConstants(0, 4, constants, 0);
            commands->Dispatch(8, 8, 64);
            TransitionIfNeeded(commands, m_noise, ReadState);
        }
        PIXEndEvent(commands);
    })) return false;
    m_cellsDirty = false;
    auto* lightingPipeline=pipelines.GetCompute(L"AtmosphereCloudLighting.hlsl",L"CsMain");
    if (!lightingPipeline) return false;
    struct LightingConstants { AtmosphereSettings settings; uint32_t output, lut, pad[2]; };
    const LightingConstants lightingConstants{settings,m_cloudLighting.UavIndex(),m_multiScatter.SrvIndex(),{0,0}};
    const auto lightingAllocation=device.Upload().Allocate(sizeof(LightingConstants),256);
    if (!lightingAllocation.IsValid()) return false;
    std::memcpy(lightingAllocation.cpu,&lightingConstants,sizeof(lightingConstants));
    if (!device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        PIXBeginEvent(commands,PIX_COLOR(120,180,255),"AtmosphereCloudLighting");
        TransitionIfNeeded(commands,m_cloudLighting,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
        commands->SetComputeRootConstantBufferView(1,lightingAllocation.gpuAddress);
        commands->SetPipelineState(lightingPipeline);
        commands->Dispatch(1,1,1);
        TransitionIfNeeded(commands,m_cloudLighting,ReadState);
        PIXEndEvent(commands);
    })) return false;
    if (!device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        TransitionIfNeeded(commands, m_skyView, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    })) return false;
    if (!m_environment.BuildFromAtmosphere(device, pipelines, settings, m_multiScatter.SrvIndex(), m_noise.SrvIndex(), m_skyView.UavIndex(), m_cloudLighting.SrvIndex())) {
        m_ready = false;
        TG_LOG_WARN("大気散乱の環境マップを生成できませんでした");
        return false;
    }
    if (!device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        TransitionIfNeeded(commands, m_skyView, ReadState);
    })) return false;
    m_applied = settings;
    m_requested = requested;
    m_environmentTime = m_cloudTime;
    m_environmentSettings = settings;
    m_cloudEnvironmentDirty = false;
    m_ready = true;
    return true;
}
void Atmosphere::Shutdown(rhi::Device& device) {
    m_environment.Shutdown(device);
    device.DeferRelease(m_multiScatter);
    device.DeferRelease(m_noise);
    device.DeferRelease(m_skyView);
    device.DeferRelease(m_cloudLighting);
    device.DeferRelease(m_cloudCells);
    device.DeferRelease(m_halfCloud);
    device.DeferRelease(m_halfDepth);
    m_cellsDirty = true;
    m_initialized = m_ready = false;
    m_lastTick = {};
    m_cloudTime = m_environmentTime = 0.0f;
    m_motion.Reset();
    device.DeferRelease(m_opticalDepth);
    m_opticalDirty = true;
    m_cloudEnvironmentDirty = false;
}
// 雲の形は元の密度で描き、低周波な光学的厚さだけを共有する。
void Atmosphere::UpdateFrameLighting(rhi::Device& device, rhi::PipelineCache& pipelines,
                                   ID3D12GraphicsCommandList* commands) {
    if (!m_ready || !m_applied.clouds || m_applied.localCloud != 2 || !m_opticalDepth.IsValid()) return;
    auto settings = m_applied;
    settings.opticalDepthIndex = UINT32_MAX;
    settings.ambientLight = 1.0f; // 天空照明の倍率は光学的厚さを変えない。
    settings.indirectLight = 1.0f; // 散乱の倍率は光学的厚さを変えない。
    settings.cloudSkylightIntensity = 1.0f; // 照明倍率は光学的厚さを変えない。
    if (m_opticalDirty || std::memcmp(&settings, &m_opticalSettings, sizeof(settings)) != 0) {
        auto* pipeline = pipelines.GetCompute(L"AtmosphereOpticalDepth.hlsl", L"CsMain");
        struct Constants { AtmosphereSettings settings; uint32_t noise, output, pad[2]; };
        const auto allocation = device.Upload().Allocate(sizeof(Constants), 256);
        if (!pipeline || !allocation.IsValid()) return;
        const Constants constants{settings, m_noise.SrvIndex(), m_opticalDepth.UavIndex(), {0,0}};
        std::memcpy(allocation.cpu, &constants, sizeof(constants));
        PIXBeginEvent(commands, PIX_COLOR(120,180,255), "CloudOpticalDepthCache");
        TransitionIfNeeded(commands, m_opticalDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
        commands->SetComputeRootConstantBufferView(1, allocation.gpuAddress);
        commands->SetPipelineState(pipeline);
        commands->Dispatch(8, 8, 16);
        TransitionIfNeeded(commands, m_opticalDepth, ReadState);
        PIXEndEvent(commands);
        m_opticalSettings = settings;
        m_opticalDirty = false;
    }
    m_applied.opticalDepthIndex = m_opticalDepth.SrvIndex() | 0x80000000u;
}
void Atmosphere::Render(rhi::Device& device, rhi::PipelineCache& pipelines,
                        ID3D12GraphicsCommandList* commands, rhi::GpuTexture& scene, rhi::GpuTexture& depth,
                        const DirectX::XMFLOAT4X4& inverseViewProjection, DirectX::XMFLOAT3 camera, bool showSky) {
    if (!m_ready) return;
    rhi::GraphicsPipelineDesc desc;
    desc.shaderPath = L"AtmosphereComposite.hlsl";
    desc.vertexEntry = L"VsMain";
    desc.pixelEntry = L"PsMain";
    desc.rtvFormat = scene.format;
    desc.cullMode = D3D12_CULL_MODE_NONE;
    desc.depthTest = desc.depthWrite = false;
    desc.alphaBlend = true;
    auto* pipeline = pipelines.GetGraphics(desc);
    struct Constants {
        DirectX::XMFLOAT4X4 inverseViewProjection;
        DirectX::XMFLOAT3 camera; uint32_t showSky;
        AtmosphereSettings settings;
        uint32_t depth, lut, noise, environment;
        uint32_t halfCloud, halfDepth, width, height;
        uint32_t halfCloudOutput, halfDepthOutput, padding[2];
    };
    const auto allocation = device.Upload().Allocate(sizeof(Constants), 256);
    if (!pipeline || !allocation.IsValid()) return;
    Constants constants{inverseViewProjection, camera, showSky ? 1u : 0u, m_applied,
        depth.SrvIndex(), m_skyView.SrvIndex(), m_noise.SrvIndex(), m_cloudLighting.SrvIndex(),
        UINT32_MAX, UINT32_MAX, scene.width, scene.height, UINT32_MAX, UINT32_MAX, {0,0}};
    // 深度は compute と pixel の両方から読む。DSV を先に外す。
    commands->OMSetRenderTargets(1, &scene.rtv.cpu, FALSE, nullptr);
    TransitionIfNeeded(commands, depth, ReadState);
    if (m_applied.clouds && !m_fullResolutionClouds) {
        const uint32_t width = (scene.width + 1) / 2, height = (scene.height + 1) / 2;
        if (m_halfCloud.width != width || m_halfCloud.height != height || !m_halfDepth.IsValid()) {
            device.DeferRelease(m_halfCloud);
            device.DeferRelease(m_halfDepth);
            // HDR 散乱光をクリップせず保存する。深度は交差判定用の距離。
            if (!CreateTarget(device, m_halfCloud, width, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, height) ||
                !CreateTarget(device, m_halfDepth, width, DXGI_FORMAT_R32_FLOAT, 1, height)) {
                device.DeferRelease(m_halfCloud);
                device.DeferRelease(m_halfDepth);
            }
        }
        auto* halfPipeline = pipelines.GetCompute(L"AtmosphereComposite.hlsl", L"CsCloudHalf");
        const auto halfAllocation = device.Upload().Allocate(sizeof(Constants), 256);
        if (m_halfCloud.IsValid() && m_halfDepth.IsValid() && halfPipeline && halfAllocation.IsValid()) {
            constants.halfCloudOutput = m_halfCloud.UavIndex();
            constants.halfDepthOutput = m_halfDepth.UavIndex();
            std::memcpy(halfAllocation.cpu, &constants, sizeof(constants));
            PIXBeginEvent(commands, PIX_COLOR(120,180,255), "CloudHalfResolution");
            TransitionIfNeeded(commands, m_halfCloud, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            TransitionIfNeeded(commands, m_halfDepth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
            commands->SetComputeRootConstantBufferView(1, halfAllocation.gpuAddress);
            commands->SetPipelineState(halfPipeline);
            commands->Dispatch(rhi::DispatchCount(width), rhi::DispatchCount(height), 1);
            TransitionIfNeeded(commands, m_halfCloud, ReadState);
            TransitionIfNeeded(commands, m_halfDepth, ReadState);
            PIXEndEvent(commands);
            constants.halfCloud = m_halfCloud.SrvIndex();
            constants.halfDepth = m_halfDepth.SrvIndex();
        }
    }
    std::memcpy(allocation.cpu, &constants, sizeof(constants));
    PIXBeginEvent(commands, PIX_COLOR(120, 180, 255), "AtmosphereComposite");
    // DSV を外してから深度を SRV として読む。
    commands->OMSetRenderTargets(1, &scene.rtv.cpu, FALSE, nullptr);
    TransitionIfNeeded(commands, depth, ReadState);
    commands->SetGraphicsRootSignature(pipelines.GlobalRootSignature());
    commands->SetGraphicsRootConstantBufferView(1, allocation.gpuAddress);
    commands->SetPipelineState(pipeline);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    PIXEndEvent(commands);
}
} // namespace tg::renderer
