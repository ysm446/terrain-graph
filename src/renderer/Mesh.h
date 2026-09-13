#pragma once

#include "rhi/Device.h"
#include "renderer/MeshData.h"

#include <DirectXMath.h>

#include <vector>

namespace tg::renderer {

// GPU 上のメッシュ。頂点・インデックスとも DEFAULT ヒープに置く。
class Mesh {
public:
    bool Create(rhi::Device& device, const MeshData& data, const wchar_t* debugName);
    void Release(rhi::Device& device);

    // asPatches が真なら 3 制御点のパッチとして描く（テセレーション用）。
    void Draw(ID3D12GraphicsCommandList* commandList, bool asPatches = false, uint32_t instanceCount = 1) const;

    bool IsValid() const { return m_indexCount > 0; }
    uint32_t IndexCount() const { return m_indexCount; }
    uint32_t VertexCount() const { return m_vertexCount; }

private:
    rhi::GpuBuffer m_vertexBuffer;
    rhi::GpuBuffer m_indexBuffer;
    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView = {};
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView = {};
    uint32_t m_indexCount = 0;
    uint32_t m_vertexCount = 0;
};

// プレビューのジオメトリ。**平面 1 種類だけ。**（このツールが扱うのは地形で、
// 球やキューブに素材を貼って眺める用途は持たない。）
// subdivisions はディスプレイスメントを効かせるための分割数。
MeshData MakePlane(float size, uint32_t subdivisions);

}  // namespace tg::renderer
