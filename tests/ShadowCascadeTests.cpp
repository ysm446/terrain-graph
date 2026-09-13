#include <algorithm>
#include <cmath>
#include <cstring>

#include "TestSupport.h"
#include "renderer/ShadowCascades.h"
#include "renderer/InstanceCulling.h"

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
    for (const uint32_t count : {2u,3u,4u})
    for (const float aspect : {0.5f, 2.0f})
        for (const XMFLOAT3 light :
             {XMFLOAT3{0.4f, 0.8f, 0.3f}, XMFLOAT3{0, 1, 0}, XMFLOAT3{0, -1, 0}}) {
            const auto data = renderer::BuildShadowCascades(camera, light, 25, aspect, 2048, count);
            const auto inverseView = XMMatrixInverse(nullptr, camera.ViewMatrix());
            float previous = data.nearDistance;
            for (uint32_t i = 0; i < count; ++i) {
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
            const auto fine = renderer::BuildShadowCascades(camera, light, 25, aspect, 4096, count);
            tests::Check(fine.splits == data.splits && fine.biases[0] < data.biases[0],
                         "解像度で分割位置は変えず、バイアスをテクセルの大きさへ合わせる");
        }
    tests::Section("インスタンスの視錐台 — 透視・平行投影と境界");
    for (auto projection : {XMMatrixPerspectiveFovRH(1.0f,1.5f,1.0f,100.0f),
                            XMMatrixOrthographicRH(20,10,1,100)}) {
        const auto view = XMMatrixLookAtRH(XMVectorSet(10,5,20,1),XMVectorSet(0,0,0,1),XMVectorSet(0,1,0,0));
        const auto matrix = view*projection;
        XMFLOAT4X4 stored; XMStoreFloat4x4(&stored,matrix);
        const auto planes = renderer::InstanceFrustumPlanes(stored);
        const auto inverse = XMMatrixInverse(nullptr,matrix);
        for (auto clip : {XMVectorSet(0,0,0.5f,1),XMVectorSet(1.2f,0,0.5f,1),
                          XMVectorSet(0,-1.2f,0.5f,1),XMVectorSet(0,0,-0.1f,1),XMVectorSet(0,0,1.1f,1)}) {
            const auto world = XMVector3TransformCoord(clip,inverse);
            bool inside = true;
            for (const auto& plane : planes) inside &= XMVectorGetX(XMPlaneDotCoord(XMLoadFloat4(&plane),world)) >= 0;
            const bool expected = std::abs(XMVectorGetX(clip))<=1 && std::abs(XMVectorGetY(clip))<=1 &&
                                  XMVectorGetZ(clip)>=0 && XMVectorGetZ(clip)<=1;
            tests::Check(inside==expected,"CPUの抽出平面がDirectXクリップ範囲と一致する");
        }
        for (const auto& plane : planes) {
            const float length = std::sqrt(plane.x*plane.x+plane.y*plane.y+plane.z*plane.z);
            tests::Check(std::abs(length-1)<0.0001f,"包囲球の半径をm単位で比較できる正規化平面");
        }
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
