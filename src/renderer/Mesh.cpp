#include "renderer/Mesh.h"

#include "core/Log.h"

#include <cstring>

using namespace DirectX;

namespace tg::renderer {

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
            // **w は -1。** シェーダは従法線を cross(normal, tangent) * w で作るが、
            // cross((0,1,0), (1,0,0)) = (0,0,-1) で、この平面の V の向き
            // （dP/dv = +Z。uv.v = tz で z が増える）と逆になる。
            // +1 のままだと接空間法線の Y が世界の -Z へ載り、**Z 方向だけ
            // 陰影が反転する**（球とキューブは +1 で dP/dv と一致している）。
            vertex.tangent = XMFLOAT4{1.0f, 0.0f, 0.0f, -1.0f};
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

}  // namespace tg::renderer
