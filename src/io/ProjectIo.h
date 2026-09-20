#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"
#include "graph/NodeGraph.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "renderer/ModelAsset.h"
#include "io/ProjectWorkspace.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <filesystem>
#include <map>

// プロジェクトとマテリアルのファイル入出力。
//
// 形式の仕様は docs/reference/file-format.md にある。変更したらそちらも直すこと。
namespace tg::io {

// 保存・読み込みの対象。Application が持っているものへの参照をまとめたもの。
// 合成の構造はグラフが唯一の持ち主（旧形式の layers[] は読み込み時にグラフへ移行する）。
struct ProjectRefs {
    compositor::TextureLibrary& textures;
    compositor::MaterialLibrary& materials;
    compositor::PaintMaskStore& paintMasks;
    renderer::SkyLibrary& skies;
    renderer::PreviewRenderer& renderer;
    graph::NodeGraph& graph;
    std::vector<renderer::ModelAsset>* models = nullptr;
    nlohmann::json* components = nullptr;
    // 0 以上なら、その部品だけを書く（0 地形 / 1 雲 / 2 大気散乱スカイ）。シーン本体は書かない。
    int componentOnly = -1;
    nlohmann::json* atmosphereAsset = nullptr;
    // シーン全体の保存で書き直す部品（kWriteTerrain などのビット）。
    // ビットの無い部品は、まだファイルが無いときだけ作る。既定は全部。
    int componentWrite = 7;
    bool saveSharedAssets = true;
};

// componentWrite のビット。
inline constexpr int kWriteTerrain = 1;
inline constexpr int kWriteCloud = 2;
inline constexpr int kWriteAtmosphere = 4;

// 保存済みかどうかの判定に使う、部品ごとの内容の指紋。
//
// 保存時と同じ分け方で地形 / 雲 / 大気散乱スカイ / シーン本体 / 共有アセット
// （マテリアル・モデル）へ振り分け、それぞれの JSON のハッシュを取る。
// 読み込みや保存の直後の値と比べれば、どの項目にまだ書いていない変更があるか分かる。
// ペイントの筆跡は GPU 上にあり、ここには映らない。
struct SceneFingerprint {
    size_t terrain = 0;
    size_t cloud = 0;
    size_t atmosphere = 0;
    size_t scene = 0;
    size_t shared = 0;
};
SceneFingerprint FingerprintScene(const ProjectRefs& refs);
std::map<std::filesystem::path, size_t> FingerprintAssets(const ProjectRefs& refs);

bool SaveWorkEnvironment(ProjectWorkspace& workspace, const ProjectRefs& refs, bool saveAsset = true);
bool LoadWorkEnvironment(ProjectWorkspace& workspace, rhi::Device& device,
                         rhi::PipelineCache& pipelineCache, const ProjectRefs& refs);
bool SaveAtmosphereAsset(ProjectWorkspace& workspace, const ProjectRefs& refs,
                         const std::filesystem::path& directory);
bool SaveSharedAssets(ProjectWorkspace& workspace, const ProjectRefs& refs,
                      const std::filesystem::path* only = nullptr);
// シーンが持つ天球は 1 つ。適用中の天球だけを残し、ほかは破棄する（フレームの外で呼ぶこと）。
void KeepOnlyActiveSky(rhi::Device& device, renderer::SkyLibrary& skies);
bool LoadSharedAsset(ProjectWorkspace& workspace, const std::filesystem::path& path,
                     rhi::Device& device, rhi::PipelineCache& pipelineCache, const ProjectRefs& refs);

// --- プロジェクト (.tgproj) -----------------------------------------------
//
// マテリアルの構造は丸ごと埋め込む。開くのに別のマテリアルファイルは要らない。
// テクスチャの画像だけは参照で持ち、パスはプロジェクトからの相対で書く。
// ペイントマスクは手続きで再現できないので、`<名前>.assets/` へ PNG で書き出す。
//
// どちらも GPU 待機を伴うため、**フレームの外で呼ぶこと。**

bool SaveProject(const std::filesystem::path& path, rhi::Device& device, const ProjectRefs& refs, ProjectWorkspace* workspace = nullptr);
bool LoadProject(const std::filesystem::path& path, rhi::Device& device,
                 rhi::PipelineCache& pipelineCache, const ProjectRefs& refs, ProjectWorkspace* workspace = nullptr);

// --- マテリアル単体 (.tgmat) ----------------------------------------------
//
// プロジェクト間でマテリアルを持ち回るための書き出し / 読み込み。
// テクスチャはこのファイルのある場所からの相対パスで参照する。
// 読み込みは既存のライブラリへ 1 つ追加する形で、他のマテリアルには触らない。

bool SaveMaterial(const std::filesystem::path& path, const compositor::MaterialAsset& asset,
                  const compositor::TextureLibrary& textures);
compositor::MaterialAssetId LoadMaterial(const std::filesystem::path& path, rhi::Device& device,
                                         rhi::PipelineCache& pipelineCache,
                                         compositor::TextureLibrary& textures,
                                         compositor::MaterialLibrary& materials);

}  // namespace tg::io
