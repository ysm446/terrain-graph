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

// 画角と、球がちょうど枠に収まる距離。
// 球（半径 1）の見かけの半径は asin(1 / 距離) なので、画角 40 度（半角 20 度）に対して
// 3.2 で 18.2 度。サムネイル（半径の 92%）と同じくらいの余白が縁に残る。
constexpr float kFovYDegrees = 40.0f;
constexpr float kDefaultDistance = 3.2f;
constexpr float kMinDistance = 1.15f;  // 内壁に近づく。遠近が強く出る
constexpr float kMaxDistance = 8.0f;

constexpr float kPi = 3.14159265358979f;

// GPU 側の SkySphereConstants と一致させること。
struct SkySphereConstants {
    uint32_t outputIndex;
    uint32_t size;
    uint32_t environmentIndex;
    float intensity;

    float exposure;
    uint32_t tonemapMode;
    float distance;
    float tanHalfFov;

    float rotation[4];  // ヨーの sin / cos、ピッチの sin / cos
};

}  // namespace

void SkySphere::Destroy(rhi::Device& device) {
    device.DeferRelease(m_output);
}

void SkySphere::Orbit(float deltaXDegrees, float deltaYDegrees) {
    // **掴んだ面がドラッグに付いてくる向き。**
    //
    // 見えているのは球の**内壁**（向こう側の面）なので、球を右へ回すと
    // 内壁は左へ動く。素直に足すと「引いた向きと逆へ動く」感触になるため、
    // 上下左右とも符号を反転して、マウスの下の面がそのまま付いてくるようにする。
    m_yawDegrees = std::fmod(m_yawDegrees - deltaXDegrees, 360.0f);
    m_pitchDegrees =
        std::clamp(m_pitchDegrees - deltaYDegrees, -kMaxPitchDegrees, kMaxPitchDegrees);
}

void SkySphere::Zoom(float steps) {
    // 1 段で 10% 寄る。マテリアルの球と同じ効き方にする。
    m_distance = std::clamp(m_distance * std::pow(0.9f, steps), kMinDistance, kMaxDistance);
}

void SkySphere::ResetView() {
    m_yawDegrees = 0.0f;
    m_pitchDegrees = 0.0f;
    m_distance = kDefaultDistance;
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
    constants.distance = m_distance;
    constants.tanHalfFov = std::tan(kFovYDegrees * 0.5f * (kPi / 180.0f));

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
