#pragma once
#include <array>

#include "renderer/Camera.h"

namespace tg::renderer {
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr float kShadowCascadeBlend = 0.1f;
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
