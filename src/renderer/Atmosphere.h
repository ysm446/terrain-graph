#pragma once
#include "../../shaders/CloudLimits.hlsli"
#include "renderer/Environment.h"
#include "renderer/CloudMotion.h"
#include <cstdint>
#include <chrono>
#include <span>
#include <vector>

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
    float flatCloudBottom = 0.0f; // 0: 丸い底、1: 平らな底。
    float cloudSkylightIntensity = 1.0f; // 実行時に地形と共通のスカイライト強度を受け取る。
    float cloudBodyOffsetX = 0.0f, cloudBodyOffsetZ = 0.0f; // 雲層の塊と表面ノイズの移流を分離。
    float indirectLight = 1.0f; // 雲の太陽光の多重散乱の倍率。
    float ambientLight = 1.0f; // 雲が受ける天空照明の倍率。
    uint32_t cloudCellIndex = UINT32_MAX; // 実行時のみ。周期セルの事前計算。
    uint32_t cloudNoiseType = 0; // 0: Perlin fBM、1: Perlin-Worley。旧 padding を利用する。
    uint32_t cloudCellCount = 10;
    float proceduralBottomHeight=0, proceduralBottomFeather=20;
    uint32_t typeMask = UINT32_MAX; // 天候層の雲種マップ。未接続は一様。
    uint32_t primitiveCount = 0;
    float primitiveSmoothness = 0;
    float primitiveDisplacement = 0, primitiveDetail = 0;
    struct Primitive { float center[4] = {}; float radius[4] = {}; };
    uint32_t primitiveBufferIndex=UINT32_MAX, primitiveBvhIndex=UINT32_MAX;
    uint32_t primitiveBvhCount=0, primitiveRevision=0;
    struct PrimitiveBvhNode {
        float lower[4]={}; // 中心群のAABB最小座標、最小rMin/rMax。
        float upper[4]={}; // 中心群のAABB最大座標、最大半径。
        uint32_t start=0, count=0, escape=0, padding=0;
    };
    float loopCenterX=0, loopCenterZ=0, loopWidth=10000, loopDepth=10000;
    uint32_t shapeCacheIndex = UINT32_MAX;
    uint32_t shapeCacheSize[3] = {};
    float weatherType = 0.3f; // 天候層の雲種。0: 層雲、1: 積乱雲。
    float weatherAnvil = 0.5f; // 積乱雲上部の横への広がり。
    float weatherWisp = 0.5f; // 雲底付近の削りの強さ。
    uint32_t opticalCacheSize = 64 | (32u << 16); // 照明キャッシュの格子数。下位16bit: XZ、上位16bit: Y。実行時のみ。
    float weatherStreets = 0.5f; // 風向に沿った帯状の伸び。
    float weatherVariation = 0.5f; // 塊ごとの密度差。
    float weatherDetailScale = 800.0f; // 細部ノイズの周期（m）。
    float weatherPadding = 0;
};
static_assert(sizeof(AtmosphereSettings) == 288);

struct CloudGeometry {
    std::vector<AtmosphereSettings::Primitive> primitives;
    std::vector<AtmosphereSettings::PrimitiveBvhNode> primitiveBvh;
};

struct GodRaySettings {
    bool enabled = false;
    float density = 0.00004f;
    float distance = 5000.0f;
};

class Atmosphere {
public:
    void SetCloudPrimitives(std::span<const AtmosphereSettings::Primitive> primitives);
    void SetDistributionMask(uint32_t index, uint64_t revision) {
        m_applied.distributionMask = index;
        if (revision != m_distributionRevision) m_opticalDirty = true;
        m_distributionRevision = revision;
    }
    void SetTypeMask(uint32_t index, uint64_t revision) {
        m_applied.typeMask = index;
        if (revision != m_typeRevision) m_opticalDirty = true;
        m_typeRevision = revision;
    }
    void UpdateFrameShape(rhi::Device& device, rhi::PipelineCache& pipelines, ID3D12GraphicsCommandList* commands);
    void UpdateFrameLighting(rhi::Device& device, rhi::PipelineCache& pipelines, ID3D12GraphicsCommandList* commands);
    void InvalidateFrameLighting() { m_opticalDirty = true; m_cellsDirty = true; m_shapeDirty = true; }
    GodRaySettings& GodRays() { return m_godRays; }
    bool& FullResolutionClouds() { return m_fullResolutionClouds; }
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
                const DirectX::XMFLOAT4X4& inverseViewProjection, DirectX::XMFLOAT3 camera, bool showSky,
                const DirectX::XMFLOAT4X4& lightViewProjection, uint32_t shadowIndex,
                float shadowTexelSize, float shadowBias);
private:
    bool UploadCloudGeometry(rhi::Device& device);
    CloudGeometry m_geometry;
    std::vector<AtmosphereSettings::Primitive> m_sourcePrimitives;
    rhi::GpuBuffer m_primitiveBuffer, m_primitiveBvhBuffer;
    uint32_t m_geometryRevision=0;
    bool m_geometryDirty=false;
    Environment m_environment;
    rhi::GpuTexture m_multiScatter;
    rhi::GpuTexture m_noise;
    rhi::GpuTexture m_skyView;
    rhi::GpuTexture m_cloudLighting;
    rhi::GpuTexture m_cloudCells;
    rhi::GpuTexture m_halfCloud;
    rhi::GpuTexture m_halfDepth;
    GodRaySettings m_godRays;
    bool m_cellsDirty = true;
    bool m_fullResolutionClouds = false;
    AtmosphereSettings m_applied;
    AtmosphereSettings m_requested;
    AtmosphereSettings m_environmentSettings;
    CloudMotion m_motion;
    std::chrono::steady_clock::time_point m_lastCloudEdit{};
    bool m_cloudEnvironmentDirty = false;
    std::chrono::steady_clock::time_point m_lastTick{};
    float m_cloudTime = 0.0f;
    float m_environmentTime = 0.0f;
    rhi::GpuTexture m_shapeCache;
    AtmosphereSettings m_shapeSettings;
    bool m_shapeDirty = true;
    rhi::GpuTexture m_opticalDepth;
    AtmosphereSettings m_opticalSettings;
    uint64_t m_distributionRevision = 0; uint64_t m_typeRevision = 0;
    uint32_t m_opticalCacheSize = 64;
    bool m_opticalDirty = true;
    bool m_initialized = false;
    bool m_ready = false;
};
} // namespace tg::renderer
