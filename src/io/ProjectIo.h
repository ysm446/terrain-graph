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
#include <functional>
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
// reload が真なら、同じ uid の素材・モデル・天球が既にあっても、ID と参照を保ったまま
// 中身をファイルの内容で上書きする（未保存の編集を捨てて戻すときに使う）。
bool LoadSharedAsset(ProjectWorkspace& workspace, const std::filesystem::path& path,
                     rhi::Device& device, rhi::PipelineCache& pipelineCache, const ProjectRefs& refs,
                     bool reload = false);

// ノードの設定が持つ参照（テクスチャ・マテリアル・ペイントマスク・モデル）の置き換え。
// 空の関数の種類には触らない。「なし」（0）の参照は関数へ渡さない。
// 置き換えずに ID を集めたいときは、受け取った ID をそのまま返せばよい。
struct NodeReferenceRemap {
    std::function<compositor::TextureId(compositor::TextureId)> texture;
    std::function<compositor::MaterialAssetId(compositor::MaterialAssetId)> material;
    std::function<compositor::PaintMaskId(compositor::PaintMaskId)> paint;
    std::function<uint64_t(uint64_t)> model;
};
void RemapNodeReferences(graph::NodeSettings& settings, const NodeReferenceRemap& remap);

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

// グラフの読み込みで、ファイルに書いてあったのに読まなかった / 直した設定。
// 読んだ設定を書き戻してファイルの値と比べて求める（知らないキー・範囲外で丸めた値・
// 使えない列挙名・型違い）。node はファイルのノード ID、path は設定の中のキー（"." 区切り）。
struct GraphReadIssue {
    graph::GraphId node = 0;
    std::string path;
    std::string message;
};
// 設定している間、グラフの読み込みで GraphReadIssue を sink へ足す（評価のレポート用）。
// nullptr で止める。普段の読み込みでは比べない。
void SetGraphReadIssueSink(std::vector<GraphReadIssue>* sink);

// ノード 1 つの設定を、保存と同じ形の JSON で返す（id・kind・ピン・位置などは除く）。
// テクスチャなどの参照はライブラリの ID のまま書く。設定の行と保存のキーを対応づけるのに使う。
nlohmann::json WriteNodeJson(const graph::Node& node);

// WriteNodeJson の形の設定を読み、base の設定だけを差し替えたノードを out に作る。
// 設定の JSON を書き換えて別の状態のノードを作るのに使う。
bool ReadNodeJson(const graph::Node& base, const nlohmann::json& settings, graph::Node& out);
// ノードの設定のうち列挙のキー（"." 区切り）と、取れる値（保存に書く名前）。
nlohmann::json EnumFieldsOf(const graph::Node& node);

// ノードカタログ。種類ごとの保存名・表示名・ピン（名前と型）・既定の設定（保存と同じ形）と、
// 設定のうち列挙のキーが取る値。定義表と保存処理から作るので、コードとずれない。
// スクリプトや LLM がグラフを書くときの資料にする（--dump-catalog）。GPU は使わない。
nlohmann::json NodeCatalog();

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
