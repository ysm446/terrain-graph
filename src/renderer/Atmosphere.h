#pragma once
#include "renderer/Environment.h"
#include "renderer/CloudMotion.h"
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
    float cloudBottom = 100.0f;
    float cloudThickness = 400.0f;
    float cloudScale = 1200.0f;
    uint32_t seed = 1;
    uint32_t samples = 64;
    float fieldCenterX = 0.0f;
    float fieldCenterZ = 0.0f;
    float fieldRadius = 3200.0f;
    float fieldFalloff = 400.0f;
    float windSpeed = 10.0f;
    float windDirection = 0.0f;
    uint32_t animateClouds = 0;
    float windOffsetX = 0.0f; // 実行時のみ。保存しない。
    float windOffsetZ = 0.0f;
    uint32_t lowerHemisphere = 1; // 0: 空の延長、1: 地面反射。
    float noiseSpeedRatio = 0.75f;
    uint32_t distributionMask = UINT32_MAX; // 雲層の分布。未接続は全面。
    uint32_t localCloud = 0; // 実行時のみ。雲ノードが楕円体の密度を指定する。
    float radiusX = 600.0f, radiusZ = 400.0f, edgeSoftness = 0.2f;
    union { float shapeStrength = 0.65f; uint32_t opticalDepthIndex; }; // 雲層では照明キャッシュの SRV。
    float detailStrength = 0.3f;
    uint32_t cloudMotionMode = 0;
    uint32_t cloudSource = 0;
};
static_assert(sizeof(AtmosphereSettings) == 144);

class Atmosphere {
public:
    void SetDistributionMask(uint32_t index, uint64_t revision) {
        m_applied.distributionMask = index;
        if (revision != m_distributionRevision) m_opticalDirty = true;
        m_distributionRevision = revision;
    }
    void UpdateFrameLighting(rhi::Device& device, rhi::PipelineCache& pipelines, ID3D12GraphicsCommandList* commands);
    void InvalidateFrameLighting() { m_opticalDirty = true; }
    void ResetAnimation();
    void ResetCloudMotion();
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
    AtmosphereSettings m_environmentSettings;
    CloudMotion m_motion;
    std::chrono::steady_clock::time_point m_lastCloudEdit{};
    bool m_cloudEnvironmentDirty = false;
    std::chrono::steady_clock::time_point m_lastTick{};
    float m_cloudTime = 0.0f;
    float m_environmentTime = 0.0f;
    rhi::GpuTexture m_opticalDepth;
    AtmosphereSettings m_opticalSettings;
    uint64_t m_distributionRevision = 0;
    bool m_opticalDirty = true;
    bool m_initialized = false;
    bool m_ready = false;
};
} // namespace tg::renderer
