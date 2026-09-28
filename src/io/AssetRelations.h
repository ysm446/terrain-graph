#pragma once
#include "io/ProjectWorkspace.h"
#include <unordered_map>
#include <vector>
namespace tg::io {
// 参照の付け替えで「同じ種類」とみなす区分。
enum class AssetKind { Image, Material, Sky, Model, Other, LayerMaterial };
// ルート内の通常の空フォルダだけを削除する。再帰削除は行わない。
bool RemoveEmptyAssetFolder(ProjectWorkspace& workspace, const std::filesystem::path& target);
AssetKind KindOfAsset(const std::filesystem::path& path);

struct AssetRelations {
    std::filesystem::path target;
    std::string uid;  // 対象のID（.meta または文書の uid）。空なら参照はパスでしか辿れない
    std::vector<std::filesystem::path> referencers;
    std::vector<std::filesystem::path> related;
    std::vector<std::filesystem::path> companions;
    std::vector<std::string> companionVersions;
    std::filesystem::file_time_type modified{};
    uintmax_t size = 0;
    bool complete = false;
};
AssetRelations InspectAssetRelations(ProjectWorkspace& workspace, const std::filesystem::path& target);
// ワークスペース全体の参照の対応表（参照ビューア用）。文書を 1 回ずつ読み、参照先と参照元の両向きを持つ。
// 添字は assets の位置。参照しているだけのファイル（画像など）も参照先として載る。
struct AssetReferenceIndex {
    struct Asset {
        std::filesystem::path path;
        std::vector<size_t> references;   // このアセットが参照しているもの
        std::vector<size_t> referencers;  // このアセットを参照しているもの
    };
    std::vector<Asset> assets;
    std::unordered_map<std::wstring, size_t> lookup;  // 小文字にした正規化パス → 添字
    bool complete = false;  // 読めない文書やリンクがあれば false
    const Asset* Find(const std::filesystem::path& path) const;
    size_t IndexOf(const Asset& asset) const { return static_cast<size_t>(&asset - assets.data()); }
};
AssetReferenceIndex BuildAssetReferenceIndex(ProjectWorkspace& workspace);
// 確認時から変わっていない場合だけ、元ファイルと.metaをルート内へ退避する。
bool RetireAsset(ProjectWorkspace& workspace, const AssetRelations& approved);
// 確認時から変わっていない場合だけ、直接の参照元の文書を replacement（同じ種類）への参照に書き換える。
// 書き換え後は参照関係が変わるので、退避の前にもう一度 InspectAssetRelations を取り直すこと。
bool ReplaceAssetReferences(ProjectWorkspace& workspace, const AssetRelations& approved,
                            const std::filesystem::path& replacement);
// ファイルと付随物（.meta、シーンなら <名前>.assets）をルート内の別フォルダへ移す。
// 参照はIDで解決されるので、移動後に再スキャンすれば切れない。
// 成功したら移動先のパスを返し、失敗したら空を返す（途中で失敗した分は戻す）。
std::filesystem::path MoveAsset(ProjectWorkspace& workspace, const std::filesystem::path& target,
                                const std::filesystem::path& directory);
// 同じフォルダ内で名前を変える。ファイルなら付随物も新しい名前へ揃え、フォルダはそのまま改名する。
// 成功したら新しいパスを返し、失敗したら空を返す。
std::filesystem::path RenameAsset(ProjectWorkspace& workspace, const std::filesystem::path& target,
                                  const std::string& newName);
}
