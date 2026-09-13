#include "renderer/ShadowCascades.h"

#include <algorithm>
#include <cmath>

namespace tg::renderer {
ShadowCascadeData BuildShadowCascades(const Camera& camera, const DirectX::XMFLOAT3& lightDirection,
                                      float sceneRadius, float aspect, uint32_t resolution,
                                      uint32_t count) {
    using namespace DirectX;
    ShadowCascadeData result{};
    count = std::clamp(count, 1u, kShadowCascadeCount);
    const float radius = std::max(sceneRadius, 0.1f) * 1.05f;
    const auto view = camera.ViewMatrix();
    const auto inverseView = XMMatrixInverse(nullptr, view);
    const float sceneDepth = -XMVectorGetZ(XMVector3TransformCoord(XMVectorZero(), view));
    const float nearDistance = std::max(camera.NearZ(), sceneDepth - radius);
    const float farDistance =
        std::max(nearDistance + 0.01f, std::min(camera.FarZ(), sceneDepth + radius));
    result.nearDistance = nearDistance;
    // 対数分割を主体にし、遠景側にも一定の密度を残す。
    for (uint32_t i = 0; i < count; ++i) {
        const float t = float(i + 1) / float(count);
        result.splits[i] = std::lerp(std::lerp(nearDistance, farDistance, t),
                                     nearDistance * std::pow(farDistance / nearDistance, t), 0.7f);
    }
    result.splits[count-1] = farDistance;
    const auto direction = XMVector3Normalize(XMLoadFloat3(&lightDirection));
    const auto up = std::abs(XMVectorGetY(direction)) > 0.99f ? XMVectorSet(0, 0, 1, 0)
                                                              : XMVectorSet(0, 1, 0, 0);
    // 回転だけの光源座標を使い、カメラ移動が投影の端数へ入り込まないようにする。
    const auto lightView = XMMatrixLookAtRH(XMVectorZero(), XMVectorNegate(direction), up);
    if (count == 1) {
        // 1枚方式はカメラに依存せず、シーン全体を覆う。
        const float extent =
            radius * float(std::max(resolution, 8u)) / float(std::max(resolution, 8u) - 4);
        const auto projection = XMMatrixOrthographicRH(2 * extent, 2 * extent, -radius, radius);
        XMStoreFloat4x4(&result.matrices[0], XMMatrixMultiply(lightView, projection));
        result.biases[0] = (2 * extent / float(std::max(resolution, 8u))) * 1.5f / (2 * radius);
        return result;
    }
    const float tanY = std::tan(camera.FovY() * 0.5f);
    const float tanX = tanY * std::max(aspect, 0.01f);
    for (uint32_t cascade = 0; cascade < count; ++cascade) {
        float start = cascade == 0 ? nearDistance : result.splits[cascade - 1];
        if (cascade > 0) {
            const float previous = cascade == 1 ? nearDistance : result.splits[cascade - 2];
            start -= (start - previous) * kShadowCascadeBlend;
        }
        const float end = result.splits[cascade];
        std::array<XMVECTOR, 8> corners;
        XMVECTOR center = XMVectorZero();
        size_t index = 0;
        for (float distance : {start, end})
            for (float y : {-1.0f, 1.0f})
                for (float x : {-1.0f, 1.0f}) {
                    corners[index] = XMVector3TransformCoord(
                        XMVectorSet(x * tanX * distance, y * tanY * distance, -distance, 1),
                        inverseView);
                    center = XMVectorAdd(center, corners[index++]);
                }
        center = XMVectorScale(center, 1.0f / 8);
        float extent = 0;
        for (const auto corner : corners)
            extent =
                std::max(extent, XMVectorGetX(XMVector3Length(XMVectorSubtract(corner, center))));
        // 回転で投影幅が変わらない包囲球と、PCF・丸めのための余白。
        extent = std::ceil(extent * 16) / 16;
        extent *= float(std::max(resolution, 8u)) / float(std::max(resolution, 8u) - 4);
        const float texel = 2 * extent / float(std::max(resolution, 8u));
        XMFLOAT3 lightCenter;
        XMStoreFloat3(&lightCenter, XMVector3TransformCoord(center, lightView));
        lightCenter.x = std::round(lightCenter.x / texel) * texel;
        lightCenter.y = std::round(lightCenter.y / texel) * texel;
        // 全シーンのZ範囲を保ち、視錐台の外にある遮蔽物も取り込む。
        const auto projection = XMMatrixOrthographicOffCenterRH(
            lightCenter.x - extent, lightCenter.x + extent, lightCenter.y - extent,
            lightCenter.y + extent, -radius, radius);
        XMStoreFloat4x4(&result.matrices[cascade], XMMatrixMultiply(lightView, projection));
        result.biases[cascade] = texel * 1.5f / (2 * radius);
    }
    return result;
}
}  // namespace tg::renderer
