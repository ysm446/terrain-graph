#pragma once
#include <array>

#include "renderer/Camera.h"

namespace tg::renderer {
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr float kShadowCascadeBlend = 0.1f;
// モデル描画と地形描画で共有する影の参照（GPU定数と同じ配置）。
struct SceneShadowData {
    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 matrices[4]{};
    uint32_t indices[4] = {0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu};
    float splits[4]{}, biases[4]{};
    float nearDistance = 0, texel = 1.0f/2048, blend = 0.1f;
    uint32_t count = 0;
};
static_assert(sizeof(SceneShadowData) == 384);
struct ShadowCascadeData {
    std::array<DirectX::XMFLOAT4X4, kShadowCascadeCount> matrices;
    std::array<float, kShadowCascadeCount> splits;
    std::array<float, kShadowCascadeCount> biases;
    float nearDistance = 0;
};
// 原点中心のシーン包囲球は、画面外の影を落とす物体も含む。
ShadowCascadeData BuildShadowCascades(const Camera& camera, const DirectX::XMFLOAT3& lightDirection,
                                      float sceneRadius, float aspect, uint32_t resolution,
                                      uint32_t count = kShadowCascadeCount);
}  // namespace tg::renderer
