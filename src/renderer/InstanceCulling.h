#pragma once
#include <array>
#include <DirectXMath.h>

namespace tg::renderer {
// row-vector規約のViewProjection。DirectXのクリップ範囲は -w<=x,y<=w, 0<=z<=w。
inline std::array<DirectX::XMFLOAT4, 6> InstanceFrustumPlanes(const DirectX::XMFLOAT4X4& matrix) {
    using namespace DirectX;
    const auto columns = XMMatrixTranspose(XMLoadFloat4x4(&matrix));
    const XMVECTOR planes[] = {columns.r[3]+columns.r[0], columns.r[3]-columns.r[0],
        columns.r[3]+columns.r[1], columns.r[3]-columns.r[1], columns.r[2], columns.r[3]-columns.r[2]};
    std::array<XMFLOAT4,6> result;
    for (size_t i=0;i<result.size();++i) XMStoreFloat4(&result[i],XMPlaneNormalize(planes[i]));
    return result;
}
}
