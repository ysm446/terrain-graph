#pragma once
#include <DirectXMath.h>

#include <cstdint>
#include <vector>
namespace tg::renderer {
// PipelineCache の VertexLayout::MeshStandard と対応する頂点。
struct MeshVertex {
    DirectX::XMFLOAT3 position;
    DirectX::XMFLOAT3 normal;
    DirectX::XMFLOAT4 tangent;  // w は従法線の向き
    DirectX::XMFLOAT2 uv;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

}  // namespace tg::renderer
