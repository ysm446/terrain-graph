#pragma once
#include "io/ProjectWorkspace.h"

namespace tg::io {
// 保存済みの参照形式の文書を扱う。GPUや編集状態には触れない。
// 出力ノードを持つ空のグラフを、重複しない名前で作成する。
std::filesystem::path CreateGraphAsset(ProjectWorkspace& workspace,
                                       const std::filesystem::path& directory, bool cloud);
nlohmann::json AtmosphereAssetBody(const nlohmann::json& preview, const std::string& name);
bool ExpandSceneAtmosphere(ProjectWorkspace& workspace, nlohmann::json& document);
bool AssignGraphComponents(nlohmann::json& graph);
bool SaveSceneComponents(ProjectWorkspace& workspace, const std::filesystem::path& scene,
                         nlohmann::json& document);
bool ExpandSceneComponents(ProjectWorkspace& workspace, nlohmann::json& document);
std::filesystem::path MigrateSceneComponents(ProjectWorkspace& workspace,
                                            const std::filesystem::path& source);
// シーンを部品ごと複製する。シーンのフォルダと同じ階層に name のフォルダを作り、シーンと、
// シーンと同じフォルダにある部品（地形・雲・空）とそのペイントをコピーする。ファイル名の
// 元のシーン名の部分は name に置き換え、ID を振り直してシーンの参照を付け替える。
// 別のフォルダにある部品と、マテリアル・モデル・テクスチャは参照のまま（共有）。
// 成功したら新しいシーンのパスを返す。失敗したら作りかけのフォルダを消し、error に理由を書く。
std::filesystem::path DuplicateScene(ProjectWorkspace& workspace, const std::filesystem::path& scene,
                                     const std::string& name, std::string& error);
}
