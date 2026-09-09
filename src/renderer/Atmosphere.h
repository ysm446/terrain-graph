#pragma once
#include "renderer/Environment.h"
#include <cstdint>
#include <chrono>

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
    float cloudThickness = 2000.0f;
    float cloudScale = 4000.0f;
    uint32_t seed = 1;
    uint32_t samples = 64;
    float fieldCenterX = 0.0f;
    float fieldCenterZ = 0.0f;
    float fieldRadius = 6000.0f;
    float fieldFalloff = 2000.0f;
    float windSpeed = 10.0f;
    float windDirection = 0.0f;
    uint32_t animateClouds = 0;
    float windOffsetX = 0.0f; // 実行時のみ。保存しない。
    float windOffsetZ = 0.0f;
    uint32_t lowerHemisphere = 1; // 0: 空の延長、1: 地面反射。
    float padding[2]{};
};
static_assert(sizeof(AtmosphereSettings) == 112);

class Atmosphere {
public:
    void ResetAnimation() { m_cloudTime = m_environmentTime = 0.0f; m_windX = m_windZ = 0.0; m_lastTick = {}; m_ready = false; }
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
    rhi::GpuTexture m_cloudLighting;
    AtmosphereSettings m_applied;
    AtmosphereSettings m_requested;
    std::chrono::steady_clock::time_point m_lastTick{};
    double m_windX = 0.0;
    double m_windZ = 0.0;
    float m_cloudTime = 0.0f;
    float m_environmentTime = 0.0f;
    bool m_initialized = false;
    bool m_ready = false;
};
} // namespace tg::renderer
