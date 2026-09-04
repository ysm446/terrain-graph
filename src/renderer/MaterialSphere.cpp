#include "renderer/MaterialSphere.h"

#include "rhi/Common.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tg::renderer {
namespace {

using rhi::DispatchCount;

// 出力の一辺。窓の大きさに合わせて作り直すと GPU 待機が要るので、
// 十分に大きい正方形を 1 枚だけ持ち、表示側で縮めて使う。
constexpr uint32_t kOutputSize = 1024;

// 画角。**素材を見るための望遠寄り**にしてある。広角にすると球の縁が
// 引き伸ばされ、同じ素材でも中心と縁で見え方が変わってしまう。
constexpr float kFovYDegrees = 30.0f;

// 軌道の範囲。仰角は極を避ける（シェーダの上方向との外積が縮退する）。
constexpr float kMaxPitchDegrees = 85.0f;
// **既定は球がちょうど収まる距離。** 画角 30 度（半角 15 度）に対して
// 球（半径 1）の見かけの半径は asin(1 / 距離) なので、4.5 で 12.8 度。
// 縁に少し余白が残り、背景との境目が見える。
constexpr float kMinDistance = 2.2f;   // 寄ると球が画面からはみ出す
constexpr float kMaxDistance = 12.0f;
constexpr float kDefaultDistance = 4.5f;
constexpr float kDefaultPitchDegrees = 12.0f;

// 背景に使う環境キューブのミップ。**ビューポートの「背景をぼかす」と同じ段。**
// 球の輪郭が背景から分かれて見えるよう、ぼかしたものを出す。
constexpr float kBackgroundMip = 3.0f;

constexpr float kPi = 3.14159265358979f;

// GPU 側の SphereConstants と一致させること。
struct SphereConstants {
    uint32_t outputIndex;
    uint32_t size;
    uint32_t baseColorIndex;
    uint32_t normalIndex;

    uint32_t roughnessIndex;
    uint32_t metallicIndex;
    uint32_t aoIndex;
    uint32_t mapChannels;

    float baseColorTint[3];
    float roughnessValue;

    float metallicValue;
    float aoValue;
    float uvScale;
    uint32_t flipNormalGreen;

    float cameraPosition[3];
    float tanHalfFov;

    float lightDirection[3];
    float lightIlluminance;

    float lightColor[3];
    float iblIntensity;

    uint32_t irradianceIndex;
    uint32_t prefilteredIndex;
    uint32_t brdfLutIndex;
    uint32_t environmentIndex;

    uint32_t prefilteredMipCount;
    float backgroundMip;
    float exposure;
    uint32_t tonemapMode;

    // ベースカラーの調整。合成と同じ値を渡すこと。
    float colorAdjust[2];  // 色相（ラジアン）, 彩度
    float pad0[2];
};

}  // namespace

void MaterialSphere::Destroy(rhi::Device& device) {
    device.DeferRelease(m_output);
}

void MaterialSphere::Orbit(float deltaXDegrees, float deltaYDegrees) {
    // **符号の付け方はビューポートのカメラ（Camera::Orbit）と同じ。**
    // 同じドラッグで違う向きに回ると、窓を行き来したときに手が迷う。
    m_yawDegrees = std::fmod(m_yawDegrees - deltaXDegrees, 360.0f);
    m_pitchDegrees =
        std::clamp(m_pitchDegrees + deltaYDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
}

void MaterialSphere::Zoom(float steps) {
    // 1 段で 10% 寄る。指数で効かせると、寄っても引いても手応えが同じになる。
    m_distance = std::clamp(m_distance * std::pow(0.9f, steps), kMinDistance, kMaxDistance);
}

void MaterialSphere::ResetView() {
    m_yawDegrees = 0.0f;
    m_pitchDegrees = kDefaultPitchDegrees;
    m_distance = kDefaultDistance;
}

void MaterialSphere::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                            ID3D12GraphicsCommandList* commandList,
                            const compositor::MaterialAsset& asset,
                            const compositor::TextureLibrary& textures,
                            const Environment& environment, float iblIntensity,
                            const LightSettings& light, float exposure, TonemapMode tonemap) {
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"MaterialSphere.hlsl", L"CsMain");
    if (pipeline == nullptr) {
        return;
    }

    if (!m_output.IsValid()) {
        rhi::TextureDesc desc;
        desc.width = kOutputSize;
        desc.height = kOutputSize;
        desc.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.allowUnorderedAccess = true;
        desc.createSrv = true;
        desc.initialState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        desc.debugName = L"MaterialSphere";
        if (!device.Allocator().CreateTexture2D(desc, m_output)) {
            return;
        }
    }

    SphereConstants constants = {};
    constants.outputIndex = m_output.UavIndex();
    constants.size = kOutputSize;
    // ベースカラーだけ sRGB として読む。それ以外はリニア（サムネイルと同じ）。
    constants.baseColorIndex = textures.SrvIndex(asset.baseColor, true);
    constants.normalIndex = textures.SrvIndex(asset.normal, false);
    constants.roughnessIndex = textures.SrvIndex(asset.roughness.texture, false);
    constants.metallicIndex = textures.SrvIndex(asset.metallic.texture, false);
    constants.aoIndex = textures.SrvIndex(asset.ambientOcclusion.texture, false);
    constants.mapChannels = compositor::PackMaterialChannels(asset);
    constants.baseColorTint[0] = asset.baseColorTint.x;
    constants.baseColorTint[1] = asset.baseColorTint.y;
    constants.baseColorTint[2] = asset.baseColorTint.z;
    constants.roughnessValue = asset.roughnessValue;
    constants.metallicValue = asset.metallicValue;
    constants.aoValue = asset.ambientOcclusionValue;
    constants.uvScale = m_uvScale;
    constants.colorAdjust[0] = asset.hueShiftDegrees * (kPi / 180.0f);
    constants.colorAdjust[1] = asset.saturation;
    constants.flipNormalGreen = asset.flipNormalGreen ? 1u : 0u;

    // 軌道カメラ。球は原点にあり半径 1。
    const float yaw = m_yawDegrees * (kPi / 180.0f);
    const float pitch = m_pitchDegrees * (kPi / 180.0f);
    const float cosPitch = std::cos(pitch);
    constants.cameraPosition[0] = m_distance * cosPitch * std::sin(yaw);
    constants.cameraPosition[1] = m_distance * std::sin(pitch);
    constants.cameraPosition[2] = m_distance * cosPitch * std::cos(yaw);
    constants.tanHalfFov = std::tan(kFovYDegrees * 0.5f * (kPi / 180.0f));

    const DirectX::XMFLOAT3 lightDirection = light.Direction();
    constants.lightDirection[0] = lightDirection.x;
    constants.lightDirection[1] = lightDirection.y;
    constants.lightDirection[2] = lightDirection.z;
    constants.lightIlluminance = light.illuminance;
    constants.lightColor[0] = light.color.x;
    constants.lightColor[1] = light.color.y;
    constants.lightColor[2] = light.color.z;

    const bool hasEnvironment = environment.IsReady();
    constants.iblIntensity = hasEnvironment ? iblIntensity : 0.0f;
    constants.irradianceIndex =
        hasEnvironment ? environment.IrradianceSrvIndex() : compositor::kInvalidTextureIndex;
    constants.prefilteredIndex = environment.PrefilteredSrvIndex();
    constants.brdfLutIndex = environment.BrdfLutSrvIndex();
    constants.environmentIndex =
        hasEnvironment ? environment.EnvironmentSrvIndex() : compositor::kInvalidTextureIndex;
    constants.prefilteredMipCount = environment.PrefilteredMipCount();
    constants.backgroundMip = kBackgroundMip;
    constants.exposure = exposure;
    constants.tonemapMode = static_cast<uint32_t>(tonemap);

    const rhi::UploadAllocation cb = device.Upload().Allocate(sizeof(SphereConstants), 256);
    if (!cb.IsValid()) {
        return;
    }
    std::memcpy(cb.cpu, &constants, sizeof(constants));

    PIXBeginEvent(commandList, PIX_COLOR(120, 200, 200), "MaterialSphere");

    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRootConstantBufferView(1, cb.gpuAddress);
    commandList->Dispatch(DispatchCount(kOutputSize), DispatchCount(kOutputSize), 1);

    // ImGui が SRV として読むので、ピクセルシェーダ可視の状態へ戻す。
    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    PIXEndEvent(commandList);
}

}  // namespace tg::renderer
