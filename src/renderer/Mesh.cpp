#include "renderer/Mesh.h"

#include "core/Log.h"

#include <cmath>
#include <cstring>

using namespace DirectX;

namespace tg::renderer {
namespace {

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

bool Mesh::Create(rhi::Device& device, const MeshData& data, const wchar_t* debugName) {
    if (data.vertices.empty() || data.indices.empty()) {
        return false;
    }

    const uint64_t vertexBytes = data.vertices.size() * sizeof(MeshVertex);
    const uint64_t indexBytes = data.indices.size() * sizeof(uint32_t);

    rhi::ResourceAllocator& allocator = device.Allocator();
    if (!allocator.CreateDefaultBuffer(vertexBytes, D3D12_RESOURCE_STATE_COMMON, debugName,
                                       m_vertexBuffer)) {
        return false;
    }
    if (!allocator.CreateDefaultBuffer(indexBytes, D3D12_RESOURCE_STATE_COMMON, debugName,
                                       m_indexBuffer)) {
        return false;
    }

    // 初期化時の一度きりの転送なので、専用のステージングバッファを使って即実行する。
    rhi::GpuBuffer staging;
    if (!allocator.CreateUploadBuffer(vertexBytes + indexBytes, L"MeshStaging", staging)) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = {0, 0};
    if (!TG_CHECK_HR(staging.resource->Map(0, &readRange, &mapped))) {
        return false;
    }
    auto* bytes = static_cast<uint8_t*>(mapped);
    std::memcpy(bytes, data.vertices.data(), vertexBytes);
    std::memcpy(bytes + vertexBytes, data.indices.data(), indexBytes);
    staging.resource->Unmap(0, nullptr);

    const bool executed = device.ExecuteImmediate([&](ID3D12GraphicsCommandList* commandList) {
        commandList->CopyBufferRegion(m_vertexBuffer.resource.Get(), 0, staging.resource.Get(), 0,
                                      vertexBytes);
        commandList->CopyBufferRegion(m_indexBuffer.resource.Get(), 0, staging.resource.Get(),
                                      vertexBytes, indexBytes);

        const D3D12_RESOURCE_BARRIER barriers[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(m_vertexBuffer.resource.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER),
            CD3DX12_RESOURCE_BARRIER::Transition(m_indexBuffer.resource.Get(),
                                                 D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_INDEX_BUFFER),
        };
        commandList->ResourceBarrier(_countof(barriers), barriers);
    });
    if (!executed) {
        return false;
    }

    m_vertexBuffer.state = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    m_indexBuffer.state = D3D12_RESOURCE_STATE_INDEX_BUFFER;

    m_vertexBufferView.BufferLocation = m_vertexBuffer.GpuAddress();
    m_vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
    m_vertexBufferView.StrideInBytes = sizeof(MeshVertex);

    m_indexBufferView.BufferLocation = m_indexBuffer.GpuAddress();
    m_indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);
    m_indexBufferView.Format = DXGI_FORMAT_R32_UINT;

    m_vertexCount = static_cast<uint32_t>(data.vertices.size());
    m_indexCount = static_cast<uint32_t>(data.indices.size());
    return true;
}

void Mesh::Release(rhi::Device& device) {
    if (m_vertexBuffer.IsValid()) {
        device.Defer(m_vertexBuffer.resource);
        device.Defer(m_vertexBuffer.allocation);
    }
    if (m_indexBuffer.IsValid()) {
        device.Defer(m_indexBuffer.resource);
        device.Defer(m_indexBuffer.allocation);
    }
    m_vertexBuffer = rhi::GpuBuffer{};
    m_indexBuffer = rhi::GpuBuffer{};
    m_indexCount = 0;
    m_vertexCount = 0;
}

void Mesh::Draw(ID3D12GraphicsCommandList* commandList, bool asPatches) const {
    if (m_indexCount == 0) {
        return;
    }
    commandList->IASetPrimitiveTopology(asPatches
                                            ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                                            : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    commandList->IASetIndexBuffer(&m_indexBufferView);
    commandList->DrawIndexedInstanced(m_indexCount, 1, 0, 0, 0);
}

MeshData MakeSphere(uint32_t segments, uint32_t rings, float radius) {
    MeshData data;
    segments = (segments < 3) ? 3 : segments;
    rings = (rings < 2) ? 2 : rings;

    data.vertices.reserve(static_cast<size_t>(segments + 1) * (rings + 1));
    for (uint32_t y = 0; y <= rings; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(rings);
        const float phi = v * kPi;
        for (uint32_t x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float theta = u * 2.0f * kPi;

            const float sinPhi = std::sin(phi);
            const XMFLOAT3 normal{sinPhi * std::cos(theta), std::cos(phi), sinPhi * std::sin(theta)};

            MeshVertex vertex;
            vertex.position = XMFLOAT3{normal.x * radius, normal.y * radius, normal.z * radius};
            vertex.normal = normal;
            // 経線方向を接線にする。
            vertex.tangent = XMFLOAT4{-std::sin(theta), 0.0f, std::cos(theta), 1.0f};
            vertex.uv = XMFLOAT2{u, v};
            data.vertices.push_back(vertex);
        }
    }

    const uint32_t stride = segments + 1;
    for (uint32_t y = 0; y < rings; ++y) {
        for (uint32_t x = 0; x < segments; ++x) {
            const uint32_t i0 = y * stride + x;
            const uint32_t i1 = i0 + stride;
            // 球は行方向が -Y、列方向が経度なので、平面とは巻き順が逆になる。
            // 外向き法線が表面になるようこの順で並べること。
            data.indices.insert(data.indices.end(), {i0, i0 + 1, i1, i0 + 1, i1 + 1, i1});
        }
    }
    return data;
}

MeshData MakePlane(float size, uint32_t subdivisions) {
    MeshData data;
    const uint32_t count = (subdivisions < 1) ? 1 : subdivisions;
    const float half = size * 0.5f;

    data.vertices.reserve(static_cast<size_t>(count + 1) * (count + 1));
    for (uint32_t z = 0; z <= count; ++z) {
        const float tz = static_cast<float>(z) / static_cast<float>(count);
        for (uint32_t x = 0; x <= count; ++x) {
            const float tx = static_cast<float>(x) / static_cast<float>(count);

            MeshVertex vertex;
            vertex.position = XMFLOAT3{-half + tx * size, 0.0f, -half + tz * size};
            vertex.normal = XMFLOAT3{0.0f, 1.0f, 0.0f};
            vertex.tangent = XMFLOAT4{1.0f, 0.0f, 0.0f, 1.0f};
            vertex.uv = XMFLOAT2{tx, tz};
            data.vertices.push_back(vertex);
        }
    }

    const uint32_t stride = count + 1;
    for (uint32_t z = 0; z < count; ++z) {
        for (uint32_t x = 0; x < count; ++x) {
            const uint32_t i0 = z * stride + x;
            const uint32_t i1 = i0 + stride;
            data.indices.insert(data.indices.end(), {i0, i1, i0 + 1, i0 + 1, i1, i1 + 1});
        }
    }
    return data;
}

MeshData MakeCube(float size, uint32_t subdivisions) {
    MeshData data;
    const float half = size * 0.5f;
    const uint32_t count = (subdivisions < 1) ? 1 : subdivisions;

    struct Face {
        XMFLOAT3 normal;
        XMFLOAT3 tangent;
    };
    const Face faces[] = {
        {{0.0f, 0.0f, -1.0f}, {-1.0f, 0.0f, 0.0f}},
        {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
        {{1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}},
        {{0.0f, -1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
        {{0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
    };

    for (const Face& face : faces) {
        const XMVECTOR normal = XMLoadFloat3(&face.normal);
        const XMVECTOR tangent = XMLoadFloat3(&face.tangent);
        const XMVECTOR bitangent = XMVector3Cross(normal, tangent);

        // 面を格子に割る。分割しないとディスプレイスメントが効かない。
        const uint32_t base = static_cast<uint32_t>(data.vertices.size());
        for (uint32_t y = 0; y <= count; ++y) {
            const float ty = static_cast<float>(y) / static_cast<float>(count);
            for (uint32_t x = 0; x <= count; ++x) {
                const float tx = static_cast<float>(x) / static_cast<float>(count);
                const float u = tx * 2.0f - 1.0f;
                const float v = ty * 2.0f - 1.0f;
                const XMVECTOR position = XMVectorScale(
                    XMVectorAdd(XMVectorAdd(XMVectorScale(tangent, u),
                                            XMVectorScale(bitangent, v)),
                                normal),
                    half);

                MeshVertex vertex;
                XMStoreFloat3(&vertex.position, position);
                vertex.normal = face.normal;
                vertex.tangent = XMFLOAT4{face.tangent.x, face.tangent.y, face.tangent.z, 1.0f};
                vertex.uv = XMFLOAT2{tx, ty};
                data.vertices.push_back(vertex);
            }
        }

        const uint32_t stride = count + 1;
        for (uint32_t y = 0; y < count; ++y) {
            for (uint32_t x = 0; x < count; ++x) {
                const uint32_t i0 = base + y * stride + x;
                const uint32_t i1 = i0 + 1;
                const uint32_t i2 = i0 + stride + 1;
                const uint32_t i3 = i0 + stride;
                data.indices.insert(data.indices.end(), {i0, i1, i2, i0, i2, i3});
            }
        }
    }
    return data;
}

}  // namespace tg::renderer
