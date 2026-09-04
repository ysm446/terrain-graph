#include "renderer/SkySphere.h"

#include "rhi/Common.h"

#include <pix3.h>

#include <algorithm>
#include <cmath>

namespace tg::renderer {
namespace {

using rhi::DispatchCount;

// 出力の一辺。窓の大きさに合わせて作り直すと GPU 待機が要るので、
// 十分に大きい正方形を 1 枚だけ持ち、表示側で縮めて使う（マテリアルの球と同じ）。
constexpr uint32_t kOutputSize = 1024;

// 「シェーダから触れない」を表す値。HLSL 側の kInvalidTextureIndex と揃える。
constexpr uint32_t kInvalidTextureIndex = 0xFFFFFFFFu;

// 回せる範囲。ピッチは真上・真下でヨーの意味が無くなるので手前で止める。
constexpr float kMaxPitchDegrees = 89.0f;
constexpr float kMinZoom = 1.0f;   // 球がちょうど枠に収まる
constexpr float kMaxZoom = 6.0f;   // 中央を大きく見る

constexpr float kPi = 3.14159265358979f;

// GPU 側の SkySphereConstants と一致させること。
struct SkySphereConstants {
    uint32_t outputIndex;
    uint32_t size;
    uint32_t environmentIndex;
    float intensity;

    float exposure;
    uint32_t tonemapMode;
    float zoom;
    float pad0;

    float rotation[4];  // ヨーの sin / cos、ピッチの sin / cos
};

}  // namespace

void SkySphere::Destroy(rhi::Device& device) {
    device.DeferRelease(m_output);
}

void SkySphere::Orbit(float deltaXDegrees, float deltaYDegrees) {
    // **絵がドラッグに付いてくる向き。** 視線を回す側なので、マテリアルの球
    // （カメラを回す側）とは符号が逆になるが、手触りは同じになる。
    m_yawDegrees = std::fmod(m_yawDegrees + deltaXDegrees, 360.0f);
    m_pitchDegrees =
        std::clamp(m_pitchDegrees + deltaYDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
}

void SkySphere::Zoom(float steps) {
    // 1 段で 10%。マテリアルの球と同じ効き方にする。
    m_zoom = std::clamp(m_zoom * std::pow(1.1f, steps), kMinZoom, kMaxZoom);
}

void SkySphere::ResetView() {
    m_yawDegrees = 0.0f;
    m_pitchDegrees = 0.0f;
    m_zoom = 1.0f;
}

void SkySphere::Render(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                       ID3D12GraphicsCommandList* commandList, const Environment& environment,
                       float iblIntensity, float exposure, TonemapMode tonemap) {
    ID3D12PipelineState* pipeline = pipelineCache.GetCompute(L"SkySphere.hlsl", L"CsMain");
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
        desc.debugName = L"SkySphere";
        if (!device.Allocator().CreateTexture2D(desc, m_output)) {
            return;
        }
    }

    const bool ready = environment.IsReady();

    SkySphereConstants constants = {};
    constants.outputIndex = m_output.UavIndex();
    constants.size = kOutputSize;
    // 環境がまだ出来ていなければ、シェーダ側で透明にして抜ける。
    constants.environmentIndex = ready ? environment.EnvironmentSrvIndex() : kInvalidTextureIndex;
    constants.intensity = iblIntensity;
    constants.exposure = exposure;
    constants.tonemapMode = static_cast<uint32_t>(tonemap);
    constants.zoom = m_zoom;

    const float yaw = m_yawDegrees * (kPi / 180.0f);
    const float pitch = m_pitchDegrees * (kPi / 180.0f);
    constants.rotation[0] = std::sin(yaw);
    constants.rotation[1] = std::cos(yaw);
    constants.rotation[2] = std::sin(pitch);
    constants.rotation[3] = std::cos(pitch);

    PIXBeginEvent(commandList, PIX_COLOR(120, 160, 220), "SkySphere");

    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->SetComputeRootSignature(pipelineCache.GlobalRootSignature());
    commandList->SetPipelineState(pipeline);
    commandList->SetComputeRoot32BitConstants(0, sizeof(constants) / sizeof(uint32_t), &constants,
                                              0);
    commandList->Dispatch(DispatchCount(kOutputSize), DispatchCount(kOutputSize), 1);

    // ImGui が SRV として読むので、ピクセルシェーダ可視の状態へ戻す。
    rhi::TransitionIfNeeded(commandList, m_output, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    PIXEndEvent(commandList);
}

}  // namespace tg::renderer
