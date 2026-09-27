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

uint32_t RoadMeshStride(const RoadMeshSettings& settings) {
    const float width = std::clamp(settings.widthMeters, kRoadMinWidthMeters, kRoadMaxWidthMeters);
    return static_cast<uint32_t>(std::max(1, static_cast<int>(std::ceil(width)))) + 1;
}

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

bool BuildRoadShoulder(const renderer::MeshData& source, uint32_t sourceStride, uint32_t edgeColumn,
                       uint32_t innerColumn, const RoadShoulderSettings& settings, renderer::MeshData& out,
                       uint32_t& outStride, std::string* error) {
    const auto fail = [&](const char* message) { if (error) *error = message; return false; };
    out.vertices.clear();
    out.indices.clear();
    outStride = 0;
    const float width = std::clamp(settings.widthMeters, kShoulderMinWidthMeters, kShoulderMaxWidthMeters);
    if (sourceStride < 2 || source.vertices.size() < static_cast<size_t>(sourceStride) * 2 ||
        source.vertices.size() % sourceStride != 0)
        return fail("路肩の元になる面がありません");
    if (edgeColumn >= sourceStride || innerColumn >= sourceStride || edgeColumn == innerColumn)
        return fail("路肩の端の列が不正です");
    const size_t rows = source.vertices.size() / sourceStride;

    // 列の横位置。端 0、（段差があれば）面取りの列、以後は約 1 m ごとに外側の端まで。
    const bool stepped = settings.stepHeightMeters > 0.0f;
    const float stepLateral = std::min(std::max(settings.stepWidthMeters, 0.005f), width * 0.5f);
    std::vector<float> laterals{0.0f};
    if (stepped) laterals.push_back(stepLateral);
    const int cells = std::max(1, static_cast<int>(std::ceil(width)));
    for (int cell = 1; cell <= cells; ++cell) {
        const float lateral = width * static_cast<float>(cell) / static_cast<float>(cells);
        if (lateral > laterals.back() + 1e-4f) laterals.push_back(lateral);
    }
    const uint32_t stride = static_cast<uint32_t>(laterals.size());
    if (rows * stride > kMaxRoadVertices) return fail("路肩の頂点が多すぎます");
    const float drop = settings.crossSlopePercent * 0.01f;

    out.vertices.resize(rows * stride);
    for (size_t row = 0; row < rows; ++row) {
        const renderer::MeshVertex& edge = source.vertices[row * sourceStride + edgeColumn];
        const renderer::MeshVertex& inner = source.vertices[row * sourceStride + innerColumn];
        // 外向きは、端から隣の列を引いた水平成分。
        float ox = edge.position.x - inner.position.x;
        float oz = edge.position.z - inner.position.z;
        const float length = std::hypot(ox, oz);
        if (length < 1e-5f) return fail("端の幅が 0 の行があります");
        ox /= length;
        oz /= length;
        for (uint32_t column = 0; column < stride; ++column) {
            const float lateral = laterals[column];
            renderer::MeshVertex& v = out.vertices[row * stride + column];
            v.position = edge.position;
            if (column > 0) {
                v.position.x += ox * lateral;
                v.position.z += oz * lateral;
                v.position.y -= drop * lateral + (stepped ? settings.stepHeightMeters : 0.0f);
            }
            v.normal = {0.0f, 0.0f, 0.0f};
            v.tangent = {ox, 0.0f, oz, 1.0f};
            v.uv = {lateral, edge.uv.y};
        }
    }
    // 三角形。左右どちらの端かで列の並びの向きが変わるので、法線が上を向くように並べる。
    out.indices.reserve((rows - 1) * (stride - 1) * 6);
    for (size_t row = 0; row + 1 < rows; ++row) {
        for (uint32_t column = 0; column + 1 < stride; ++column) {
            const uint32_t a = static_cast<uint32_t>(row * stride + column);
            const uint32_t tris[2][3] = {{a, a + stride, a + 1}, {a + 1, a + stride, a + stride + 1}};
            for (const auto& tri : tris) {
                uint32_t x = tri[0], y = tri[1], z = tri[2];
                const XMVECTOR px = XMLoadFloat3(&out.vertices[x].position);
                XMVECTOR face = XMVector3Cross(XMVectorSubtract(XMLoadFloat3(&out.vertices[y].position), px),
                                               XMVectorSubtract(XMLoadFloat3(&out.vertices[z].position), px));
                if (XMVectorGetY(face) < 0.0f) {
                    std::swap(y, z);
                    face = XMVectorNegate(face);
                }
                if (XMVectorGetY(face) <= 1e-7f) return fail("幅に対してカーブが急すぎて路肩が裏返ります");
                for (const uint32_t index : {x, y, z}) {
                    XMFLOAT3& n = out.vertices[index].normal;
                    XMStoreFloat3(&n, XMVectorAdd(XMLoadFloat3(&n), face));
                }
                out.indices.insert(out.indices.end(), {x, y, z});
            }
        }
    }
    // 法線と接線。接線は U（内側 → 外側）の向き。従法線 cross(N, T) が道のりの増える向き（+V）に
    // なるよう w を決める（左の路肩では外向きが左なので w = -1 になる）。
    for (size_t row = 0; row < rows; ++row) {
        const size_t next = row + 1 < rows ? row + 1 : row;
        const size_t prev = row + 1 < rows ? row : row - 1;
        for (uint32_t column = 0; column < stride; ++column) {
            renderer::MeshVertex& v = out.vertices[row * stride + column];
            const XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&v.normal));
            XMStoreFloat3(&v.normal, n);
            XMVECTOR t = XMVectorSet(v.tangent.x, v.tangent.y, v.tangent.z, 0.0f);
            t = XMVector3Normalize(XMVectorSubtract(t, XMVectorScale(n, XMVectorGetX(XMVector3Dot(n, t)))));
            const XMVECTOR along = XMVectorSubtract(XMLoadFloat3(&out.vertices[next * stride + column].position),
                                                    XMLoadFloat3(&out.vertices[prev * stride + column].position));
            const float w = XMVectorGetX(XMVector3Dot(XMVector3Cross(n, t), along)) < 0.0f ? -1.0f : 1.0f;
            v.tangent = {XMVectorGetX(t), XMVectorGetY(t), XMVectorGetZ(t), w};
        }
    }
    outStride = stride;
    return true;
}

}  // namespace tg::graph
