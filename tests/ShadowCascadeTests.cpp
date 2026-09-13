#include <algorithm>
#include <cmath>
#include <cstring>

#include "TestSupport.h"
#include "renderer/ShadowCascades.h"

void RunShadowCascadeTests() {
    using namespace tg;
    using namespace DirectX;
    tests::Section("カスケードシャドウ — 分割と投影");
    renderer::Camera camera;
    camera.SetSceneRadius(25);
    renderer::CameraState state;
    state.distance = 8;
    state.pitch = 0.2f;
    camera.SetState(state);
    for (const float aspect : {0.5f, 2.0f})
        for (const XMFLOAT3 light :
             {XMFLOAT3{0.4f, 0.8f, 0.3f}, XMFLOAT3{0, 1, 0}, XMFLOAT3{0, -1, 0}}) {
            const auto data = renderer::BuildShadowCascades(camera, light, 25, aspect, 2048);
            const auto inverseView = XMMatrixInverse(nullptr, camera.ViewMatrix());
            float previous = data.nearDistance;
            for (uint32_t i = 0; i < renderer::kShadowCascadeCount; ++i) {
                tests::Check(data.splits[i] > previous && std::isfinite(data.biases[i]) &&
                                 data.biases[i] > 0,
                             "分割距離が単調増加し、深度バイアスが有限である");
                float start = previous;
                if (i)
                    start -= (previous - (i == 1 ? data.nearDistance : data.splits[i - 2])) *
                             renderer::kShadowCascadeBlend;
                const auto matrix = XMLoadFloat4x4(&data.matrices[i]);
                for (float distance : {start, data.splits[i]})
                    for (float x : {-1.0f, 1.0f})
                        for (float y : {-1.0f, 1.0f}) {
                            const float tangent = std::tan(camera.FovY() * 0.5f);
                            const auto world = XMVector3TransformCoord(
                                XMVectorSet(x * tangent * distance * aspect, y * tangent * distance,
                                            -distance, 1),
                                inverseView);
                            const auto clip = XMVector3TransformCoord(world, matrix);
                            tests::Check(std::isfinite(XMVectorGetX(clip)) &&
                                             std::isfinite(XMVectorGetY(clip)) &&
                                             std::abs(XMVectorGetX(clip)) <= 1 &&
                                             std::abs(XMVectorGetY(clip)) <= 1,
                                         "重複範囲を含む視錐台の8隅が影のXY範囲内に収まる");
                        }
                const auto lightAxis = XMVector3Normalize(XMLoadFloat3(&light));
                for (float sign : {-1.0f, 1.0f}) {
                    const auto clip =
                        XMVector3TransformCoord(XMVectorScale(lightAxis, 25 * sign), matrix);
                    tests::Check(XMVectorGetZ(clip) >= 0 && XMVectorGetZ(clip) <= 1,
                                 "画面外の遮蔽物も含むシーン全体の深度を保持する");
                }
                previous = data.splits[i];
            }
            const auto fine = renderer::BuildShadowCascades(camera, light, 25, aspect, 4096);
            tests::Check(fine.splits == data.splits && fine.biases[0] < data.biases[0],
                         "解像度で分割位置は変えず、バイアスをテクセルの大きさへ合わせる");
        }
    const XMFLOAT3 light{0.4f, 0.8f, 0.3f};
    const auto before = renderer::BuildShadowCascades(camera, light, 25, 2, 2048);
    auto moved = camera.State();
    const auto right = camera.Basis().right;
    moved.target.x += right.x * 0.01f;
    moved.target.y += right.y * 0.01f;
    moved.target.z += right.z * 0.01f;
    camera.SetState(moved);
    const auto after = renderer::BuildShadowCascades(camera, light, 25, 2, 2048);
    const auto single = renderer::BuildShadowCascades(camera, light, 25, 2, 2048, 1);
    camera.SetState(state);
    const auto singleOtherCamera = renderer::BuildShadowCascades(camera, light, 25, 0.5f, 2048, 1);
    const auto singleMatrix = XMLoadFloat4x4(&single.matrices[0]);
    tests::Check(
        std::memcmp(&single.matrices[0], &singleOtherCamera.matrices[0], sizeof(XMFLOAT4X4)) == 0,
        "1枚方式の投影はカメラの移動・画角に依存しない");
    for (const XMFLOAT3 axis : {XMFLOAT3{25, 0, 0}, XMFLOAT3{0, 25, 0}, XMFLOAT3{0, 0, 25}}) {
        for (float sign : {-1.0f, 1.0f}) {
            const auto clip =
                XMVector3TransformCoord(XMVectorScale(XMLoadFloat3(&axis), sign), singleMatrix);
            tests::Check(std::abs(XMVectorGetX(clip)) <= 1 && std::abs(XMVectorGetY(clip)) <= 1 &&
                             XMVectorGetZ(clip) >= 0 && XMVectorGetZ(clip) <= 1,
                         "1枚方式がシーン全体を覆う");
        }
    }
    for (uint32_t i = 0; i < renderer::kShadowCascadeCount; ++i) {
        const float dx = (after.matrices[i]._41 - before.matrices[i]._41) * 1024;
        const float dy = (after.matrices[i]._42 - before.matrices[i]._42) * 1024;
        tests::Check(
            std::abs(dx - std::round(dx)) < 0.001f && std::abs(dy - std::round(dy)) < 0.001f,
            "微小なカメラ平行移動では投影がテクセル単位で動く");
    }
}
