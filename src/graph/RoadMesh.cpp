#include "graph/RoadMesh.h"

#include <algorithm>
#include <cmath>

namespace tg::graph {
namespace {
using namespace DirectX;

// 隣り合う区間の向きの内積がこれ未満の角は断る（約 75° より急。断面が重なって裏返る）。
constexpr float kMinCornerDot = 0.25f;
// 頂点数の上限（1 本の道路）。
constexpr size_t kMaxRoadVertices = 4u * 1024u * 1024u;
}  // namespace

bool BuildRoadMesh(const RoadPathSettings& road, const RoadProfileCurve& centerline,
                   const RoadMeshSettings& settings, renderer::MeshData& out, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    out.vertices.clear();
    out.indices.clear();
    const auto& points = centerline.points;
    const size_t rows = points.size();
    if (rows < 2) return fail("中心線の点が足りません");
    const float width = std::clamp(settings.widthMeters, kRoadMinWidthMeters, kRoadMaxWidthMeters);
    // 幅方向は約 1 m ごと。
    const int columns = std::max(1, static_cast<int>(std::ceil(width)));
    const size_t stride = static_cast<size_t>(columns) + 1;
    if (rows * stride > kMaxRoadVertices) return fail("道路が長すぎます（頂点が多すぎる）");

    // 区間の水平な向き。
    std::vector<XMFLOAT2> directions(rows - 1);
    for (size_t i = 0; i + 1 < rows; ++i) {
        const float dx = points[i + 1].x - points[i].x;
        const float dz = points[i + 1].z - points[i].z;
        const float length = std::hypot(dx, dz);
        if (length < 1e-5f) return fail("中心線に長さ 0 の区間があります");
        directions[i] = {dx / length, dz / length};
    }

    out.vertices.resize(rows * stride);
    for (size_t i = 0; i < rows; ++i) {
        // 頂点の向きは前後の区間の平均。角では断面を 1/cos だけ広げて幅を保つ（マイター）。
        const XMFLOAT2 before = directions[i == 0 ? 0 : i - 1];
        const XMFLOAT2 after = directions[i + 1 == rows ? rows - 2 : i];
        const float turn = before.x * after.x + before.y * after.y;
        if (turn < kMinCornerDot) return fail("中心線の角が急すぎます（曲線にするか点を足してください）");
        XMFLOAT2 forward{before.x + after.x, before.y + after.y};
        const float length = std::hypot(forward.x, forward.y);
        forward = {forward.x / length, forward.y / length};
        const float miter = 1.0f / std::max(forward.x * after.x + forward.y * after.y, 0.5f);
        // 進行方向に向かって右（Road Path と同じ。右手系 Y-up で (-dz, 0, dx)）。
        const XMFLOAT3 right{-forward.y, 0.0f, forward.x};
        const float distance = centerline.arcLengths[i];
        const float bank = road.bankEnabled ? EvaluateBankAngleRadians(road, centerline, distance) : 0.0f;
        // 正のバンクで左（-right）が上がる。横位置 l（右が正）の点は right*cos - up*sin だけずれる。
        const float c = std::cos(bank);
        const float s = std::sin(bank);
        for (int k = 0; k <= columns; ++k) {
            const float lateral = -0.5f * width + width * static_cast<float>(k) / static_cast<float>(columns);
            const float horizontal = lateral * c * miter;
            renderer::MeshVertex& v = out.vertices[i * stride + static_cast<size_t>(k)];
            v.position = {points[i].x + right.x * horizontal,
                          points[i].y - lateral * s + settings.surfaceOffsetMeters,
                          points[i].z + right.z * horizontal};
            v.normal = {0.0f, 0.0f, 0.0f};
            // 接線は U（左 → 右）の向き。従法線 cross(N, T) が進行方向（+V）になるので w = +1。
            v.tangent = {right.x * c, -s, right.z * c, 1.0f};
            v.uv = {lateral + 0.5f * width, distance};
        }
    }

    // 三角形と、面の法線の積み上げ。表は上（+Y）。
    out.indices.reserve((rows - 1) * static_cast<size_t>(columns) * 6);
    for (size_t i = 0; i + 1 < rows; ++i) {
        for (int k = 0; k < columns; ++k) {
            const uint32_t a = static_cast<uint32_t>(i * stride + static_cast<size_t>(k));
            const uint32_t b = a + 1;                                 // 右隣
            const uint32_t d = static_cast<uint32_t>(a + stride);     // 前
            const uint32_t e = d + 1;
            const XMVECTOR pa = XMLoadFloat3(&out.vertices[a].position);
            const XMVECTOR across = XMVectorSubtract(XMLoadFloat3(&out.vertices[b].position), pa);
            const XMVECTOR along = XMVectorSubtract(XMLoadFloat3(&out.vertices[d].position), pa);
            const XMVECTOR face = XMVector3Cross(across, along);
            if (XMVectorGetY(face) <= 1e-7f) return fail("路面が裏返る所があります（バンク角が大きすぎるか、角が急すぎます）");
            for (const uint32_t index : {a, b, d, e}) {
                XMFLOAT3& n = out.vertices[index].normal;
                XMStoreFloat3(&n, XMVectorAdd(XMLoadFloat3(&n), face));
            }
            out.indices.insert(out.indices.end(), {a, d, b, b, d, e});
        }
    }
    for (renderer::MeshVertex& v : out.vertices) {
        XMStoreFloat3(&v.normal, XMVector3Normalize(XMLoadFloat3(&v.normal)));
        // 接線を法線と直交させる（バンクで傾いた面でも U の向きのまま）。
        const XMVECTOR n = XMLoadFloat3(&v.normal);
        XMVECTOR t = XMVectorSet(v.tangent.x, v.tangent.y, v.tangent.z, 0.0f);
        t = XMVector3Normalize(XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t)))));
        v.tangent = {XMVectorGetX(t), XMVectorGetY(t), XMVectorGetZ(t), 1.0f};
    }
    return true;
}

}  // namespace tg::graph
