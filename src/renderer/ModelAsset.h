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
// インポスター（遠景用に多方向から焼いた画像）の設定。
struct ImpostorSettings {
    uint32_t frames = 12;      // 1 辺あたりの方向数（frames × frames 方向を撮る）
    uint32_t frameSize = 256;  // 1 方向の画像の一辺（px）
    bool fullSphere = false;   // 偽なら上半球だけ（地面に置く物向け。上からの解像度が倍）
    bool operator==(const ImpostorSettings&) const = default;
};
struct ModelImpostor {
    ImpostorSettings settings;  // 次に作るときの設定
    // 焼いた結果。画像はモデルの横に PNG で置く（色 / 法線 / 色むらの重み）。
    // variationPath は色むらを入れる前に焼いたものでは空（全画素が色むらを受ける扱い）。
    bool baked = false;
    ImpostorSettings bakedSettings;
    DirectX::XMFLOAT3 center{};  // 撮った球の中心と半径（モデル空間、m）
    float radius = 0;
    std::filesystem::path colorPath, normalPath, variationPath;
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
    ModelImpostor impostor;
    std::string error;
};
bool LoadModel(const std::filesystem::path& path, ModelAsset& asset);
// LOD lod に替わるカメラ距離（m、等倍のとき）。lod 0 は 0。
float LodStartDistance(const ModelAsset& asset, size_t lod);
}  // namespace tg::renderer
