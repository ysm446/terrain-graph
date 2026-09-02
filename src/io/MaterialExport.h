#pragma once

#include "compositor/MaterialLibrary.h"
#include "compositor/MaterialStack.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"
#include "rhi/Device.h"
#include "rhi/PipelineCache.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace tg::io {

// チャンネルの詰め方。
enum class ExportPacking : uint32_t {
    Separate = 0,  // ラフネス / メタルネス / AO を 1 枚ずつ
    Ord = 1,       // AO=R / ラフネス=G / ハイト=B（Megascans の並び）
    Orm = 2,       // AO=R / ラフネス=G / メタルネス=B（glTF / Unreal の並び）
};

// 書き出す解像度の選択肢。プレビューの合成解像度とは独立に選べる。
// **編集はプレビュー解像度のまま、書き出しだけを高くできる**のがこの機能の要点。
inline constexpr uint32_t kExportResolutions[] = {1024, 2048, 4096, 8192};

struct ExportSettings {
    std::filesystem::path directory;
    // ファイル名の頭。`<baseName>_BaseColor.png` のように後ろへ種類が付く。
    std::string baseName = "Material";
    uint32_t resolution = 2048;
    ExportPacking packing = ExportPacking::Separate;
    bool baseColor = true;
    bool normal = true;
    // ラフネス / メタルネス / AO。packing に従って 1 枚ずつか 1 枚へまとめるかが決まる。
    bool surface = true;
    bool height = true;
    // **ハイトは既定で EXR。** 8bit の PNG に落とすと段差が見える。
    bool heightAsExr = true;
};

// 書き出しの対象。Application が持っているものへの参照をまとめたもの。
struct ExportRefs {
    const compositor::MaterialStack& stack;
    const compositor::TextureLibrary& textures;
    const compositor::MaterialLibrary& materials;
    const compositor::PaintMaskStore& paintMasks;
};

// 合成結果を画像として書き出す。書き出せた枚数を返す（0 なら失敗）。
//
// **プレビュー用の評価器には触らない。** 書き出し専用の評価器を指定解像度で作り、
// タイルに分けて評価してから読み戻す。編集中の解像度を上げ下げしないので、
// 書き出しの前後で画面の見え方が変わらない。
//
// GPU 待機とファイル入出力を伴うため、**フレームの外で呼ぶこと。**
uint32_t ExportMaterialTextures(rhi::Device& device, rhi::PipelineCache& pipelineCache,
                                const ExportRefs& refs, const ExportSettings& settings);

}  // namespace tg::io
