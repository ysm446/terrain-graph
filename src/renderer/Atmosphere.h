#pragma once
#include "renderer/Environment.h"
#include <cstdint>

namespace tg::renderer {
// HLSL の AtmosphericParameters と同じ配置。距離は m、太陽は大気圏外照度 lux。
struct AtmosphereSettings {
    float azimuth = 0.9f;
    float elevation = 0.9f;
    float illuminance = 120000.0f;
    float density = 1.0f;
    float mie = 0.3f;
    float eccentricity = 0.8f;
    float altitude = 0.0f;
    float groundAlbedo = 0.2f;
    uint32_t clouds = 0;
    float coverage = 0.55f;
    float extinction = 0.006f;
    float cloudBottom = 1500.0f;
    float cloudThickness = 1500.0f;
    float cloudScale = 12000.0f;
    uint32_t seed = 1;
    uint32_t samples = 64;
};
static_assert(sizeof(AtmosphereSettings) == 64);

class Atmosphere {
public:
    bool Update(rhi::Device& device, rhi::PipelineCache& pipelines, const AtmosphereSettings& settings);
    void Shutdown(rhi::Device& device);
    const Environment& GetEnvironment() const { return m_environment; }
    bool IsReady() const { return m_ready; }
    uint32_t NoiseIndex() const { return m_noise.SrvIndex(); }
    const AtmosphereSettings& AppliedSettings() const { return m_applied; }
    void Render(rhi::Device& device, rhi::PipelineCache& pipelines,
                ID3D12GraphicsCommandList* commands, rhi::GpuTexture& scene, rhi::GpuTexture& depth,
                const DirectX::XMFLOAT4X4& inverseViewProjection, DirectX::XMFLOAT3 camera, bool showSky);
private:
    Environment m_environment;
    rhi::GpuTexture m_multiScatter;
    rhi::GpuTexture m_noise;
    rhi::GpuTexture m_skyView;
    AtmosphereSettings m_applied;
    bool m_initialized = false;
    bool m_ready = false;
};
} // namespace tg::renderer
