#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"
#include "graph/NodeGraph.h"
#include "renderer/PreviewRenderer.h"
#include "renderer/SkyLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <filesystem>

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
};

// --- プロジェクト (.tgproj) -----------------------------------------------
//
// マテリアルの構造は丸ごと埋め込む。開くのに別のマテリアルファイルは要らない。
// テクスチャの画像だけは参照で持ち、パスはプロジェクトからの相対で書く。
// ペイントマスクは手続きで再現できないので、`<名前>.assets/` へ PNG で書き出す。
//
// どちらも GPU 待機を伴うため、**フレームの外で呼ぶこと。**

bool SaveProject(const std::filesystem::path& path, rhi::Device& device, const ProjectRefs& refs);
bool LoadProject(const std::filesystem::path& path, rhi::Device& device,
                 rhi::PipelineCache& pipelineCache, const ProjectRefs& refs);

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
