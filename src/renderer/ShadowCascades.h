#pragma once
#include <array>

#include "renderer/Camera.h"

namespace tg::renderer {
inline constexpr uint32_t kShadowCascadeCount = 4;
inline constexpr float kShadowCascadeBlend = 0.1f;
// モデル描画と地形描画で共有する影の参照（GPU定数と同じ配置）。
struct SceneShadowData {
    DirectX::XMFLOAT4X4 view{};
    DirectX::XMFLOAT4X4 matrices[kShadowCascadeCount]{};
    uint32_t indices[kShadowCascadeCount] = {0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu};
    float splits[kShadowCascadeCount]{}, biases[kShadowCascadeCount]{};
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
// カスケードの分割の偏りの既定値（0 = 均等割り、1 = 対数割り）。1 に近いほど近景を細かく描く。
// 最後の段はいつもシーン全体の奥まで覆うので、上げても遠景の影は消えない（2〜3 段目が粗くなる）。
inline constexpr float kDefaultShadowSplitLambda = 0.97f;
// 原点中心のシーン包囲球は、画面外の影を落とす物体も含む。
ShadowCascadeData BuildShadowCascades(const Camera& camera, const DirectX::XMFLOAT3& lightDirection,
                                      float sceneRadius, float aspect, uint32_t resolution,
                                      uint32_t count = kShadowCascadeCount,
                                      float splitLambda = kDefaultShadowSplitLambda);
}  // namespace tg::renderer
