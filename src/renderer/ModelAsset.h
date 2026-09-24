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
    std::filesystem::path assetPath;
    std::string assetUid;
    std::string name;
    std::filesystem::path path;
    std::shared_ptr<const ModelGeometry> geometry;
    std::vector<compositor::MaterialAssetId> materials;
    // LOD の切り替え距離（m、等倍のとき）。[i] が LOD i+1 に替わる距離。
    // 足りない段は LodStartDistance の既定値を使う。空なら全段が既定値。
    std::vector<float> lodDistances;
    std::string error;
};
bool LoadModel(const std::filesystem::path& path, ModelAsset& asset);
// LOD lod に替わるカメラ距離（m、等倍のとき）。lod 0 は 0。
float LodStartDistance(const ModelAsset& asset, size_t lod);
}  // namespace tg::renderer
