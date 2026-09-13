#pragma once
#include <filesystem>
#include <memory>
#include <string>

#include "compositor/MaterialLayer.h"
#include "renderer/MeshData.h"

namespace tg::renderer {
struct ModelPart {
    MeshData mesh;
    uint32_t slot = 0;
};
struct ModelLod {
    std::vector<ModelPart> parts;
    uint32_t triangles = 0;
};
struct ModelGeometry {
    std::vector<ModelLod> lods;
    std::vector<std::string> slots;
    DirectX::XMFLOAT3 minimum{}, maximum{};
};
// CPU形状は不変・共有。履歴へ頂点配列を複製しない。
struct ModelAsset {
    uint64_t id = 0;
    std::string name;
    std::filesystem::path path;
    std::shared_ptr<const ModelGeometry> geometry;
    std::vector<compositor::MaterialAssetId> materials;
    std::string error;
};
bool LoadModel(const std::filesystem::path& path, ModelAsset& asset);
}  // namespace tg::renderer
