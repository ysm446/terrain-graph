#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "compositor/MaterialLibrary.h"
#include "compositor/TextureLibrary.h"
#include "renderer/ModelAsset.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

namespace tg::renderer {
// 焼いたインポスターの GPU テクスチャ。どちらも RGBA8 UNORM で、ミップ付き。
//   color  : rgb = ベースカラー（sRGB で符号化）、a = 覆い
//   normal : rg = モデル空間の法線（8 面体）、b = 深度、a = ラフネス
// 方向とマスの対応は shaders/ImpostorCommon.hlsli。
struct ImpostorTextures {
    rhi::GpuTexture color, normal;
    ImpostorSettings settings;
    DirectX::XMFLOAT3 center{};
    float radius = 0;
    std::filesystem::path colorPath, normalPath;
};

// モデルごとのインポスター。テクスチャライブラリとは別に持つ（一覧や文書のテクスチャに混ぜない）。
// 焼き込み・読み込みは GPU 待機を伴うので、どれもフレームの外で呼ぶこと。
class ImpostorLibrary {
   public:
    void Destroy(rhi::Device& device);
    const ImpostorTextures* Find(uint64_t model) const;
    // 焼いた画像を GPU へ読み込む。読み込み済みで同じ画像なら何もしない。焼いていなければ破棄する。
    bool Sync(rhi::Device& device, rhi::PipelineCache& cache, const ModelAsset& model);
    // LOD0 を焼き、色と法線を colorPath / normalPath へ PNG で保存する。GPU にもそのまま残す。
    // 成功したら result に焼いた結果（パス、設定、中心と半径）を書く。
    bool Bake(rhi::Device& device, rhi::PipelineCache& cache, const ModelAsset& model,
              const compositor::MaterialLibrary& materials, const compositor::TextureLibrary& textures,
              const std::filesystem::path& colorPath, const std::filesystem::path& normalPath,
              ModelImpostor& result, std::string& error);
    void Remove(rhi::Device& device, uint64_t model);

   private:
    std::unordered_map<uint64_t, ImpostorTextures> m_entries;
    std::unordered_map<uint64_t, std::filesystem::path> m_failed;  // 読めなかった画像（色のパス）
};
// インポスターの画像の置き場所。モデルの .tgmodel（無ければ FBX）の横に「名前_Impostor_C/N.png」。
void ImpostorPaths(const ModelAsset& model, std::filesystem::path& colorPath, std::filesystem::path& normalPath);
}  // namespace tg::renderer
