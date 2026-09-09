#include "renderer/Atmosphere.h"
#include "core/Log.h"
#include <pix3.h>
#include <cstring>

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
bool Atmosphere::Update(rhi::Device& device, rhi::PipelineCache& pipelines, const AtmosphereSettings& settings) {
    if (m_ready && std::memcmp(&settings, &m_applied, sizeof(settings)) == 0) return true;
    if (!m_initialized) {
        if (!m_environment.Initialize(device, pipelines, false) ||
            !CreateTarget(device, m_multiScatter, 32, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !CreateTarget(device, m_noise, 64, DXGI_FORMAT_R16_FLOAT, 64) ||
            !CreateTarget(device, m_skyView, 512, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 256)) {
            Shutdown(device);
            return false;
        }
        m_initialized = true;
    }
    const bool updateLut = !m_ready || settings.density != m_applied.density || settings.mie != m_applied.mie;
    const bool updateNoise = !m_ready || settings.seed != m_applied.seed;
    auto* lutPipeline = pipelines.GetCompute(L"AtmosphereMultiScatter.hlsl", L"CSGenerate");
    auto* noisePipeline = pipelines.GetCompute(L"AtmosphereCloudDensity.hlsl", L"CSGenerate");
    if (!lutPipeline || !noisePipeline) return false;
    if ((updateLut || updateNoise) && !device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        PIXBeginEvent(commands, PIX_COLOR(120, 180, 255), "AtmosphereCaches");
        commands->SetComputeRootSignature(pipelines.GlobalRootSignature());
        if (updateLut) {
            TransitionIfNeeded(commands, m_multiScatter, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            struct Constants { float density, mie, g; uint32_t output; };
            const Constants constants{settings.density, settings.mie, settings.eccentricity, m_multiScatter.UavIndex()};
            commands->SetPipelineState(lutPipeline);
            commands->SetComputeRoot32BitConstants(0, 4, &constants, 0);
            commands->Dispatch(4, 4, 1);
            TransitionIfNeeded(commands, m_multiScatter, ReadState);
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
    if (!device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        TransitionIfNeeded(commands, m_skyView, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    })) return false;
    if (!m_environment.BuildFromAtmosphere(device, pipelines, settings, m_multiScatter.SrvIndex(), m_noise.SrvIndex(), m_skyView.UavIndex())) {
        m_ready = false;
        TG_LOG_WARN("大気散乱の環境マップを生成できませんでした");
        return false;
    }
    if (!device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commands) {
        TransitionIfNeeded(commands, m_skyView, ReadState);
    })) return false;
    m_applied = settings;
    m_ready = true;
    return true;
}
void Atmosphere::Shutdown(rhi::Device& device) {
    m_environment.Shutdown(device);
    device.DeferRelease(m_multiScatter);
    device.DeferRelease(m_noise);
    device.DeferRelease(m_skyView);
    m_initialized = m_ready = false;
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
    };
    const auto allocation = device.Upload().Allocate(sizeof(Constants), 256);
    if (!pipeline || !allocation.IsValid()) return;
    const Constants constants{inverseViewProjection, camera, showSky ? 1u : 0u, m_applied,
        depth.SrvIndex(), m_skyView.SrvIndex(), m_noise.SrvIndex(), m_environment.EnvironmentSrvIndex()};
    std::memcpy(allocation.cpu, &constants, sizeof(constants));
    PIXBeginEvent(commands, PIX_COLOR(120, 180, 255), "AtmosphereComposite");
    // DSV を外してから深度を SRV として読む。
    commands->OMSetRenderTargets(1, &scene.rtv.cpu, FALSE, nullptr);
    TransitionIfNeeded(commands, depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commands->SetGraphicsRootSignature(pipelines.GlobalRootSignature());
    commands->SetGraphicsRootConstantBufferView(1, allocation.gpuAddress);
    commands->SetPipelineState(pipeline);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    PIXEndEvent(commands);
}
} // namespace tg::renderer
