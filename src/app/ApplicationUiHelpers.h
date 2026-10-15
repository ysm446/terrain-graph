#pragma once

// Application の分割された実装ファイル（Application*.cpp）で共有する
// UI ヘルパと定数。Application の実装専用で、他のモジュールからは使わない。
//
// もとは Application.cpp の匿名名前空間にあったもの。複数の翻訳単位から
// 使うため、関数と変数は inline にしてある。

#include "compositor/MaterialLayer.h"
#include "core/PathUtf8.h"
#include "compositor/MaterialLibrary.h"
#include "compositor/PaintMask.h"
#include "compositor/TextureLibrary.h"
#include "renderer/Camera.h"
#include "renderer/PreviewRenderer.h"
#include "ui/UiStyle.h"
#include "app/AssetSelectionContext.h"

#include <imgui.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace tg {

inline float RadiansToDegrees(float radians) {
    return radians * (180.0f / 3.14159265358979323846f);
}

inline float DegreesToRadians(float degrees) {
    return degrees * (3.14159265358979323846f / 180.0f);
}

// -pi .. pi へ折り返す。方位角を一周させるときに使う（UI のスライダーもこの範囲）。
inline float WrapAngle(float radians) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    radians = std::fmod(radians + kPi, kTwoPi);
    if (radians < 0.0f) {
        radians += kTwoPi;
    }
    return radians - kPi;
}



// 既定値マーカーが参照する値。数値リテラルではなく設定構造体の初期値を使う。
inline const compositor::MaterialLayer kDefaultLayer;

// Surface ノードの既定値。地形へ素材を塗るのが主な使い方なので、「下地に沿わせる」を入れておく
// （切のままだと、勝った所の高さが基準の高さ 0.5 の平面へ引かれて地形が潰れる）。
// 構造体の既定は切のままにして、この値を持たない古いファイルの読み方を変えない。
inline const compositor::MaterialLayer kDefaultSurfaceLayer = [] {
    compositor::MaterialLayer layer;
    layer.wrapToUnderlying = true;
    layer.nearTileMeters = 2.0f;  // 近景は人間のスケール（草・土・砂礫の粒が見える大きさ）
    return layer;
}();

// シェイプレイヤーの既定値。追加時の初期値と既定値マーカーの参照先を兼ねる。
// 地形スケールの起伏が役割なので、ノイズは低周波にする。
inline const compositor::MaterialLayer kDefaultShapeLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Shape;
    layer.name = "Shape";
    layer.heightSource = compositor::ValueSource::Noise;
    layer.heightBase = 0.5f;  // 0.5 で持ち上げなし
    layer.heightGain = 0.6f;
    layer.heightNoise = compositor::NoiseParams{compositor::NoiseType::Fbm, 3.0f, 1.0f, 5, 0.0f};
    return layer;
}();

// 水面レイヤーの既定値。値の根拠は docs/design/compositing.md の「水面レイヤー」。
// 水の拡散反射はほぼゼロで、見える色は水中の散乱・吸収の色（赤が最も吸収される）。
inline const compositor::MaterialLayer kDefaultLiquidLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Liquid;
    layer.name = "Sea";  // 表示名（種類の識別子は Liquid のまま）
    layer.roughness = 0.07f;
    layer.metallic = 0.0f;
    layer.heightSource = compositor::ValueSource::Constant;
    layer.heightBase = 0.35f;   // 水位
    layer.blendRange = 0.01f;   // 汀線のフェザー幅
    // 海の見た目。沖は深い青（ベースカラー）、岸へ向かって明るい青になり、浅瀬では底が透ける。
    // 既定は「澄んだ外洋」（ユーザー指定）。水は赤を強く吸収し青を一番よく通すので、澄んだ水ほど
    // 深い紺になる（濁りやプランクトンが増えると緑へ寄る）。参考画像
    // （docs/references/sea/blender-nation-hearder.jpg）の海もこの系統。
    // 浅瀬の色は水そのものの色として控えめに置く（底の色は「透ける深さ」が見せる）。
    layer.baseColor = {0.003f, 0.012f, 0.04f};
    layer.liquid.shallowColor = {0.03f, 0.09f, 0.15f};
    layer.liquid.colorDepthMeters = 8.0f;
    layer.liquid.clarityMeters = 2.0f;
    layer.liquid.waveStrength = 0.5f;
    layer.liquid.waveFacetSizeMeters = 0.02f;  // さざ波の波面（1〜2 cm）
    layer.liquid.shoreFoam = 0.6f;
    layer.liquid.shoreRunupMeters = 0.4f;
    return layer;
}();

// ハイトマップノードの既定値。**ソースとして単体で成立する形**にしておく。
// 画像の 0〜1 がそのままハイトの全幅（起伏の強さは 1.0 固定で UI にも出さない）。
// その全幅を実寸へ変換するのはノードの「標高差」（m）。
inline const compositor::MaterialLayer kDefaultHeightmapLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Shape;
    layer.name = "Heightmap";
    layer.heightSource = compositor::ValueSource::Texture;
    layer.heightBase = 0.5f;  // 0.5 で持ち上げなし
    layer.heightGain = 1.0f;
    return layer;
}();

// ブラーノードの既定値。**合成しない加工**なので、色もマスクも使わない。
// 半径は terrain-editor の既定（3 セル）にならい、1024m / 1024px を想定して 3m。
inline const compositor::MaterialLayer kDefaultBlurLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Blur;
    layer.name = "Blur";
    return layer;
}();

// 崩落ノードの既定値。値は terrain-editor の Crumbling に合わせてある。
inline const compositor::MaterialLayer kDefaultCrumblingLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Crumbling;
    layer.name = "Crumbling";
    return layer;
}();

// 堆積ノードの既定値。値は terrain-editor の Sediment に合わせてある。
inline const compositor::MaterialLayer kDefaultSedimentLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Sediment;
    layer.name = "Sediment";
    return layer;
}();

inline const compositor::MaterialLayer kDefaultMeanderingRiversLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::MeanderingRivers;
    layer.name = "Meandering Rivers";
    return layer;
}();
// Lake の既定値。
inline const compositor::MaterialLayer kDefaultLakeLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Lake;
    layer.name = "Lake";
    // 新しく置く Lake は水を描く（色は湖らしくやや緑に寄せた淡水、波は穏やか）。
    // 波の大きさ・波打ち際は、Liquid が無いシーンではこの値がシーン全体に使われる。
    layer.baseColor = {0.006f, 0.022f, 0.03f};
    layer.roughness = 0.07f;
    layer.liquid.paintWater = true;
    layer.liquid.shallowColor = {0.04f, 0.12f, 0.12f};
    layer.liquid.colorDepthMeters = 4.0f;
    layer.liquid.clarityMeters = 1.5f;
    layer.liquid.waveStrength = 0.2f;
    layer.liquid.waveScaleMeters = 12.0f;
    layer.liquid.shoreFoam = 0.25f;
    layer.liquid.shoreRunupMeters = 0.08f;
    layer.liquid.shoreSpacingMeters = 8.0f;
    layer.liquid.shoreWidthMeters = 15.0f;
    return layer;
}();
// Snow Cover の既定値。
inline const compositor::MaterialLayer kDefaultSnowCoverLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::SnowCover;
    layer.name = "Snow Cover";
    return layer;
}();
// 積雪ノードの既定値。値は terrain-editor の Snow に合わせてある。
inline const compositor::MaterialLayer kDefaultSnowLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Snow;
    layer.name = "Snow";
    return layer;
}();

// 河川ノードの既定値。設計は docs/reference/river-node.md。
inline const compositor::MaterialLayer kDefaultRiverLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::River;
    layer.name = "River";
    // 新しく置く River は流れの波と早瀬の白波を出す（古いファイルは 0 で読む）。
    layer.river.flowWaveStrength = 0.5f;
    layer.river.flowFoam = 0.5f;
    // 新しく置く River は水の色も自分で付ける（後ろに水の色の Surface を置かなくてよい）。
    layer.baseColor = {0.006f, 0.022f, 0.03f};
    layer.roughness = 0.07f;
    layer.liquid.paintWater = true;
    layer.liquid.shallowColor = {0.04f, 0.12f, 0.12f};
    layer.liquid.colorDepthMeters = 3.0f;
    layer.liquid.clarityMeters = 1.0f;
    return layer;
}();

// 散布ノードの既定値。値は terrain-editor の Scatter に合わせてある。
inline const compositor::MaterialLayer kDefaultScatterLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Scatter;
    layer.name = "Scatter";
    // 既定は**高さ 0**。まず分布（Mask）を見ながら密度と大きさを決め、
    // 地形へ盛るかどうかは後から選べるようにする（terrain-editor と同じ既定）。
    layer.scatter = {};
    return layer;
}();

// 水滴侵食ノードの既定値。値は terrain-editor の Droplet Erosion に合わせてある。
inline const compositor::MaterialLayer kDefaultDropletLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::Droplet;
    layer.name = "Droplet Erosion";
    return layer;
}();

inline const compositor::MaterialLayer kDefaultMultiScaleErosionLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::MultiScaleErosion;
    layer.name = "Multi-Scale Erosion";
    return layer;
}();

inline const compositor::MaterialLayer kDefaultFluvialErosionLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::FluvialErosion;
    layer.name = "Fluvial Erosion";
    return layer;
}();

inline const compositor::MaterialLayer kDefaultFlattenBordersLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::FlattenBorders;
    layer.name = "Flatten Borders";
    return layer;
}();

inline const compositor::MaterialLayer kDefaultHeightLevelsLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::HeightLevels;
    layer.name = "Height Levels";
    return layer;
}();

inline const compositor::MaterialLayer kDefaultRoadGradingLayer = [] {
    compositor::MaterialLayer layer;
    layer.kind = compositor::LayerKind::RoadGrading;
    layer.name = "Grading";
    return layer;
}();

inline const compositor::MaterialLayer& DefaultLayerFor(compositor::LayerKind kind) {
    switch (kind) {
        case compositor::LayerKind::Surface:
            return kDefaultSurfaceLayer;
        case compositor::LayerKind::RoadGrading:
            return kDefaultRoadGradingLayer;
        case compositor::LayerKind::Shape:
            return kDefaultShapeLayer;
        case compositor::LayerKind::Liquid:
            return kDefaultLiquidLayer;
        case compositor::LayerKind::Blur:
            return kDefaultBlurLayer;
        case compositor::LayerKind::Sediment:
            return kDefaultSedimentLayer;
        case compositor::LayerKind::Crumbling:
            return kDefaultCrumblingLayer;
        case compositor::LayerKind::MeanderingRivers:
            return kDefaultMeanderingRiversLayer;
        case compositor::LayerKind::Lake:
            return kDefaultLakeLayer;
        case compositor::LayerKind::SnowCover:
            return kDefaultSnowCoverLayer;
        case compositor::LayerKind::Snow:
            return kDefaultSnowLayer;
        case compositor::LayerKind::River:
            return kDefaultRiverLayer;
        case compositor::LayerKind::FluvialErosion:
            return kDefaultFluvialErosionLayer;
        case compositor::LayerKind::FlattenBorders:
            return kDefaultFlattenBordersLayer;
        case compositor::LayerKind::HeightLevels:
            return kDefaultHeightLevelsLayer;
        case compositor::LayerKind::MultiScaleErosion:
            return kDefaultMultiScaleErosionLayer;
        case compositor::LayerKind::Droplet:
            return kDefaultDropletLayer;
        case compositor::LayerKind::Scatter:
            return kDefaultScatterLayer;
        default:
            return kDefaultLayer;
    }
}

// 曲率マスクの向き。compositor::CurvatureMode の並びと一致させること。
inline const ui::LocalizedText kCurvatureModeLabels[] = {
    {"尾根", "Ridges"}, {"谷", "Valleys"}, {"両方", "Both"}};
// 岩片の形。compositor::RockStyle の並びと一致させること。
inline const ui::LocalizedText kRockStyleLabels[] = {
    {"丸い", "Rounded"}, {"多面体", "Faceted"}, {"尖った破片", "Shards"}};
// 堆積 / 積雪の計算グリッド。合成解像度とは別に持つ。
inline const char* const kSedimentResolutionLabels[] = {"256", "512", "1024"};
inline constexpr uint32_t kSedimentResolutionValues[] = {256, 512, 1024};
// 積雪は 2048 まで選べる（評価器側のクランプ上限に合わせる）。
inline const char* const kSnowResolutionLabels[] = {"256", "512", "1024", "2048"};
inline constexpr uint32_t kSnowResolutionValues[] = {256, 512, 1024, 2048};
inline const compositor::BrushSettings kDefaultBrush;
inline const renderer::LightSettings kDefaultLight;
inline const renderer::ExposureSettings kDefaultExposure;
inline const renderer::MaterialSettings kDefaultMaterial;
inline const renderer::CameraState kDefaultCamera;
inline const renderer::SkySettings kDefaultSky;

inline const ui::LocalizedText kNoiseTypeLabels[] = {
    {"fBm", "fBm"},       {"尾根状", "Ridged"}, {"セル状", "Cellular"},
    {"Perlin", "Perlin"}, {"雲状", "Billow"},   {"割れ目", "Cracks"}};
inline const ui::LocalizedText kValueSourceLabels[] = {
    {"定数", "Constant"}, {"ノイズ", "Noise"}, {"テクスチャ", "Texture"}};
// **「ノード」は選ばせない。** グラフで Mask 入力へ繋ぐと自動でそれになる。
inline const ui::LocalizedText kMaskSourceLabels[] = {
    {"定数", "Constant"},
    {"ノイズ", "Noise"},
    {"テクスチャ", "Texture"},
    {"下地の高さ", "Base Height"},
    {"下地の傾斜", "Base Slope"},
    {"下地の曲率", "Base Curvature"},
    {"下地の窪み", "Base Cavity"},
    {"ペイント", "Paint"},
};
// マスクの合成。compositor::MaskBlendMode の並びと一致させること。
inline const ui::LocalizedText kMaskBlendModeLabels[] = {
    {"加算", "Add"}, {"乗算", "Multiply"}, {"小さいほう", "Min"}, {"大きいほう", "Max"},
    {"減算", "Subtract"}};
// 川筋マスクの出力カーブ。compositor::FluvialCurve の並びと一致させること。
inline const ui::LocalizedText kFluvialCurveLabels[] = {
    {"対数", "Logarithmic"}, {"しきい値", "Threshold"}, {"線形", "Linear"}};
// 川筋マスクの計算グリッド。合成解像度とは別に持つ。
inline const char* const kFluvialResolutionLabels[] = {"256", "512", "1024"};
inline constexpr uint32_t kFluvialResolutionValues[] = {256, 512, 1024};
inline const char* const kChannelLabels[] = {"BaseColor", "Normal", "Surface", "Height"};

// ビューポートの表示モード。renderer::DebugView と並びを合わせること。
inline const ui::LocalizedText kDebugViewLabels[] = {
    {"シェーディング", "Shaded"},
    {"ベースカラー", "Base Color"},
    {"法線（カメラ）", "Normal (Camera)"},
    {"法線（ワールド）", "Normal (World)"},
    {"ラフネス", "Roughness"},
    {"メタルネス", "Metalness"},
    {"AO", "AO"},
    {"ハイト", "Height"},
    {"ハイト（ローカル）", "Height (Local)"},
    {"ワイヤーフレーム", "Wireframe"},
    {"クレイ", "Clay"},
    {"LOD", "LOD"},
};
// ペイントマスクの解像度。
inline const char* const kResolutionLabels[] = {"512", "1024", "2048", "4096"};
inline constexpr uint32_t kResolutionValues[] = {512, 1024, 2048, 4096};
// 合成解像度（プレビューの評価解像度）。書き出しと同じ 8192 まで選べる。
inline const char* const kMaterialResolutionLabels[] = {"512", "1024", "2048", "4096", "8192"};
inline constexpr uint32_t kMaterialResolutionValues[] = {512, 1024, 2048, 4096, 8192};
// 平面メッシュの分割数。形の細かさの上限を決める。
inline const char* const kMeshSubdivisionLabels[] = {"256", "512", "1024"};
inline constexpr uint32_t kMeshSubdivisionValues[] = {256, 512, 1024};

// プレビューの窓（マテリアル / テクスチャ / 天球）の、上の区画に使う正方形の一辺。
//
// **幅に合わせる。** 窓を広げれば絵も大きくなり、区画に余白が残らない。
// ただし窓が横長のときに幅ぶんの高さを取ると下のプロパティが消えるので、
// 残りの高さ（プロパティ 1 画面ぶんを残した値）で頭打ちにする。
// **この関数は上の区画を置く直前に呼ぶこと**（`GetContentRegionAvail` を見るため）。
inline float PreviewPaneSize() {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float maxHeight = available.y - ui::Scaled(140.0f);
    return std::max(ui::Scaled(120.0f), std::min(available.x, maxHeight));
}

// テクスチャ一覧からマップ欄へのドラッグ＆ドロップで使うペイロードの種別。
inline constexpr const char* kTextureDragDropType = "TG_TEXTURE";
// マテリアル一覧から Surface のマテリアル欄（プロパティの行 / ノードのサムネイル）へ
// ドラッグ＆ドロップで割り当てるときのペイロードの種別。中身は MaterialAssetId。
inline constexpr const char* kMaterialDragDropType = "TG_MATERIAL";
// アセット一覧のサムネイルをフォルダ階層へドラッグ＆ドロップして移動するときの
// ペイロードの種別。中身は UTF-8 のパス文字列（終端なし）。
inline constexpr const char* kAssetPathDragDropType = "TG_ASSET_PATH";

// テクスチャの一覧に出すフォーマット名。DXGI の名前は長いので短く言い換える。
inline const char* TextureFormatLabel(const compositor::LibraryTexture& entry) {
    return entry.isFloat ? ui::Tr("RGBA16F (リニア)", "RGBA16F (Linear)")
                         : ui::Tr("RGBA8 (sRGB / リニア)", "RGBA8 (sRGB / Linear)");
}

// ステータスバーの通知を残す時間（秒）。情報だけが時間で消える。
inline constexpr float kStatusHoldSeconds = 6.0f;

// ビューポートの背景色の既定値（設定の行の「既定へ戻す」用）。io::AppSettings の clearColor の初期値と揃えること。
inline constexpr float kDefaultClearColor[3] = {0.09f, 0.09f, 0.11f};

// 解像度コンボの選択位置。一致するものが無ければ 1（1024）に寄せる。
inline int ResolutionIndex(uint32_t resolution) {
    for (int i = 0; i < IM_ARRAYSIZE(kResolutionValues); ++i) {
        if (kResolutionValues[i] == resolution) {
            return i;
        }
    }
    return 1;
}
inline int MaterialResolutionIndex(uint32_t resolution) {
    for (int i = 0; i < IM_ARRAYSIZE(kMaterialResolutionValues); ++i) {
        if (kMaterialResolutionValues[i] == resolution) {
            return i;
        }
    }
    return 1;
}

// ノイズの種類を選ぶ行。
inline bool DrawNoiseTypeRow(const char* label, compositor::NoiseType& type,
                      compositor::NoiseType defaultType) {
    int selected = static_cast<int>(type);
    if (ui::PropertyCombo(label, &selected, ui::Tr(kNoiseTypeLabels).data(), IM_ARRAYSIZE(kNoiseTypeLabels),
                          static_cast<int>(defaultType),
                          ui::Tr("fBm: 一般的な起伏 / 尾根状: 稜線や割れ目 / セル状: 石畳や砂利",
                                 "fBm: general relief / Ridged: ridgelines and cracks / Cellular: paving and gravel"))) {
        type = static_cast<compositor::NoiseType>(selected);
        return true;
    }
    return false;
}

// ノイズのパラメータをまとめて並べる。ハイトとマスクで共通。
// ハイトでは寄与の量を heightGain が担うので、showAmount を false にして「量」を出さない。
inline bool DrawNoiseRows(compositor::NoiseParams& noise, const compositor::NoiseParams& defaults,
                   bool showAmount = true) {
    bool changed = DrawNoiseTypeRow(ui::Tr("種類", "Type"), noise.type, defaults.type);
    // 周波数はタイルするよう整数へ丸めて評価される（Common.hlsli の SampleNoise）。
    // スライダーの刻みも 1 にして、丸めと表示がずれないようにする。
    changed |= ui::PropertyFloat(ui::Tr("周波数", "Frequency"), &noise.scale, 1.0f, 64.0f, defaults.scale,
                                 ui::Tr("大きいほど細かい模様になる。タイルさせるため整数で使う", "Higher gives a finer pattern. Used as an integer so it tiles"),
                                 "%.0f", 0, 1.0f);
    if (showAmount) {
        changed |= ui::PropertyFloat(ui::Tr("量", "Amount"), &noise.amount, 0.0f, 3.0f, defaults.amount,
                                     ui::Tr("ノイズの寄与。0 で効かなくなる", "Noise contribution. 0 disables it"), "%.2f");
    }
    changed |= ui::PropertyInt(ui::Tr("オクターブ", "Octaves"), &noise.octaves, 1, 8, defaults.octaves,
                               ui::Tr("重ねる段数。多いほど細部が増え、計算も増える", "Number of layers. More adds detail and cost"));
    changed |= ui::PropertyFloat(ui::Tr("オフセット", "Offset"), &noise.offset, 0.0f, 64.0f, defaults.offset,
                                 ui::Tr("同じ設定で別の模様がほしいときにずらす", "Shift for a different pattern with the same settings"), "%.1f", 0, 0.5f);
    return changed;
}

inline void DrawAssetSourceButton(const std::filesystem::path& path, std::filesystem::path& request) {
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    const auto tooltip = path.empty() ? std::string(ui::Tr("参照元がありません（未指定または未保存）", "No source (unassigned or unsaved)"))
        : std::string(ui::Tr("参照元をアセットブラウザで表示\n", "Show the source in the Asset Browser\n")) + ToUtf8Display(path);
    if (ui::RevealSourceButton(!path.empty(), tooltip.c_str())) request = path;
}
inline float AssetReferenceWidth(float width) {
    return std::max(ui::Scaled(24.0f), width - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
}
inline void DrawAssetPathRow(const char* label, const std::filesystem::path& path, std::filesystem::path& request) {
    ui::PropertyLabel(label);
    // アイコンを先に置き、長いファイル名でも画面外へ押し出されないようにする。
    const auto tooltip = path.empty() ? std::string(ui::Tr("参照元がありません", "No source")) : std::string(ui::Tr("参照元をアセットブラウザで表示\n", "Show the source in the Asset Browser\n")) + ToUtf8Display(path);
    if (ui::RevealSourceButton(!path.empty(), tooltip.c_str())) request = path;
    ImGui::SameLine();
    ImGui::TextWrapped("%s", path.empty() ? ui::Tr("（未指定）", "(None)") : ToUtf8Display(path.filename()).c_str());
    ui::PropertyEnd();
}

// 未読み込みの候補をルートから表示。GPUロードは次のフレームの外で行う。
inline bool AssetSlotPathMatches(const std::filesystem::path& a, const std::filesystem::path& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}
// thumbnailSide が 0 より大きければ、読み込み済みの行と同じくサムネイルと名前で描く（材質の一覧）。
inline void DrawUnloadedAssetChoices(uint32_t widget, uint32_t previous, bool texture, bool allowLayers,
                                    const std::function<bool(const std::filesystem::path&)>& loaded,
                                    float thumbnailSide = 0.0f) {
    auto* context = g_assetSelectionContext;
    if (!context) return;
    if (ImGui::IsWindowAppearing()) context->Scan();
    for (const auto& path : context->candidates) {
        const auto kind = io::KindOfAsset(path);
        const bool accepted = texture ? kind == io::AssetKind::Image :
            kind == io::AssetKind::Material || (allowLayers && kind == io::AssetKind::LayerMaterial);
        if (!accepted || loaded(path)) continue;
        const auto relative = ToUtf8Display(path.lexically_relative(context->root));
        if (!ui::ComboFilterPass(relative)) continue;
        ImGui::PushID(relative.c_str());
        if (thumbnailSide > 0.0f && context->thumbnail) {
            // 読み込み済みの行と同じ形: 行全体をサムネイルの高さの Selectable にし、上へ画像と名前を描く。
            // 名前はファイル名（拡張子なし）、フォルダは薄い文字で添える。選ぶと読み込んで割り当てる。
            const ImVec2 rowPos = ImGui::GetCursorPos();
            const bool picked = ImGui::Selectable("##unloaded", false, 0, ImVec2(0, thumbnailSide));
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 nextPos = ImGui::GetCursorPos();
            ImGui::SetCursorPos(rowPos);
            ui::ThumbnailImage(static_cast<ImTextureID>(context->thumbnail(path)), thumbnailSide);
            ImGui::SameLine();
            const float textY = rowPos.y + (thumbnailSide - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPosY(textY);
            ImGui::TextUnformatted(ToUtf8Display(path.stem()).c_str());
            const auto folder = ToUtf8Display(path.parent_path().lexically_relative(context->root));
            if (!folder.empty() && folder != ".") {
                ImGui::SameLine();
                ImGui::SetCursorPosY(textY);  // SameLine は行の先頭の高さへ戻すので、名前と揃え直す
                ImGui::TextDisabled("%s", folder.c_str());
            }
            ImGui::SetCursorPos(nextPos);
            if (picked) context->Queue(path, widget, previous);
            if (hovered) ImGui::SetTooltip(ui::Tr("%s\n選ぶと読み込んで割り当てる", "%s\nSelect to load and assign it"), relative.c_str());
        } else {
            const float side = ImGui::GetFrameHeight();
            // ラベルをSelectable自身に持たせ、ポップアップの幅も名前に合わせる。
            if (ImGui::Selectable(relative.c_str(), false, 0, ImVec2(0, side))) context->Queue(path, widget, previous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", relative.c_str());
        }
        ImGui::PopID();
    }
}
inline bool AcceptAssetSlotDrop(uint32_t widget, uint32_t& value, bool texture, bool allowLayers) {
    auto* context = g_assetSelectionContext;
    if (!context || !ImGui::BeginDragDropTarget()) return false;
    if (const auto* payload = ImGui::AcceptDragDropPayload(kAssetPathDragDropType)) {
        const std::string utf8(static_cast<const char*>(payload->Data), static_cast<size_t>(payload->DataSize));
        // 複数選択のペイロードは割り当て対象を一意に決められないので受けない。
        if (utf8.find('\n') == std::string::npos && utf8.find('\0') == std::string::npos) {
            const auto path = FromUtf8(utf8);
            const auto kind = io::KindOfAsset(path);
            if (texture ? kind == io::AssetKind::Image : kind == io::AssetKind::Material || (allowLayers && kind == io::AssetKind::LayerMaterial))
                context->Queue(path, widget, value);
        }
    }
    ImGui::EndDragDropTarget();
    return false;
}

// マテリアルを選ぶ行。サムネイル付きの一覧から選ぶ。
inline bool DrawMaterialSlotRow(const char* label, compositor::MaterialAssetId& slot,
                         const compositor::MaterialLibrary& library, std::filesystem::path& revealRequest, bool allowLayerMaterials = true, bool showThumbnail = false) {
    ui::PropertyLabel(label, ui::Tr("「なし」ならレイヤーの定数値だけで塗る", "If None, paints with the layer's constant values only"));
    const uint32_t widget = ImGui::GetID("##value");

    std::string preview = ui::Tr("なし", "None");
    if (const compositor::MaterialAsset* current = library.Find(slot); current != nullptr) {
        preview = current->name;
    }

    const float thumbnailSize = showThumbnail ? ui::Scaled(40) : ImGui::GetFrameHeight();
    bool changed = g_assetSelectionContext && g_assetSelectionContext->Consume(widget, slot);
    const auto acceptDrop = [&]() {
        bool accepted = false;
        if (ImGui::BeginDragDropTarget()) {
            if (const auto* payload = ImGui::AcceptDragDropPayload(kMaterialDragDropType);
                payload && payload->DataSize == sizeof(compositor::MaterialAssetId)) {
                const auto id = *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                const auto* asset = library.Find(id);
                if (asset && (allowLayerMaterials || !asset->layerMaterial)) { slot = id; accepted = true; }
            }
            ImGui::EndDragDropTarget();
        }
        return accepted;
    };
    if (showThumbnail) {
        const float rowY = ImGui::GetCursorPosY();
        ui::ThumbnailButton("##assignedMaterial", static_cast<ImTextureID>(library.ThumbnailHandle(slot).ptr), thumbnailSize, false);
        changed |= acceptDrop();
        AcceptAssetSlotDrop(widget, slot, false, allowLayerMaterials);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ui::Tr("マテリアルをここへドロップして割り当て", "Drop a material here to assign it"));
        ImGui::SameLine();
        ImGui::SetCursorPosY(rowY + (thumbnailSize - ImGui::GetFrameHeight()) * 0.5f);
    }
    ImGui::SetNextItemWidth(
        AssetReferenceWidth(std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x)));
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        ui::ComboFilterInput();
        if (!ui::ComboFilterActive() && ImGui::Selectable(ui::Tr("なし", "None"), slot == compositor::kNoMaterialAsset)) {
            slot = compositor::kNoMaterialAsset;
            changed = true;
        }
        for (const compositor::MaterialAsset& asset : library.Entries()) {
            if (!allowLayerMaterials && asset.layerMaterial) continue;
            if (!ui::ComboFilterPass(asset.name)) continue;
            ImGui::PushID(static_cast<int>(asset.id));
            if (asset.thumbnail.IsValid()) {
                // サムネイルと名前を行の中央で揃えるため、行全体をサムネイルの高さの
                // Selectable にして、その上へ画像と名前を描く。
                const ImVec2 rowPos = ImGui::GetCursorPos();
                const bool picked = ImGui::Selectable("##item", slot == asset.id,
                                                      ImGuiSelectableFlags_None,
                                                      ImVec2(0.0f, thumbnailSize));
                const ImVec2 nextPos = ImGui::GetCursorPos();
                ImGui::SetCursorPos(rowPos);
                ImGui::Image(static_cast<ImTextureID>(asset.thumbnail.srv.gpu.ptr),
                             ImVec2(thumbnailSize, thumbnailSize));
                ImGui::SameLine();
                ImGui::SetCursorPosY(rowPos.y + (thumbnailSize - ImGui::GetTextLineHeight()) * 0.5f);
                ImGui::TextUnformatted(asset.name.c_str());
                ImGui::SetCursorPos(nextPos);
                if (picked) {
                    slot = asset.id;
                    changed = true;
                }
            } else if (ImGui::Selectable(asset.name.c_str(), slot == asset.id)) {
                slot = asset.id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 0)); // SetCursorPosで戻した最終行の領域を確定。
        DrawUnloadedAssetChoices(widget, slot, false, allowLayerMaterials, [&](const auto& path) {
            for (const auto& asset : library.Entries()) if (AssetSlotPathMatches(asset.assetPath, path)) return true;
            return false;
        }, thumbnailSize);
        ImGui::Dummy(ImVec2(0, 0)); // SetCursorPosで戻した最終行の領域を確定。
        ImGui::EndCombo();
    }
    // マテリアル一覧からドラッグしてきたものを受ける。テクスチャのコンボと同じ作りで、
    // ドラッグ中にコンボは開けないので、直前のアイテムは必ずコンボ本体になる。
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kMaterialDragDropType);
            payload != nullptr) {
            if (payload->DataSize == sizeof(compositor::MaterialAssetId)) {
                const auto id = *static_cast<const compositor::MaterialAssetId*>(payload->Data);
                const auto* asset = library.Find(id);
                if (asset && (allowLayerMaterials || !asset->layerMaterial)) { slot = id; changed = true; }
            }
        }
        ImGui::EndDragDropTarget();
    }
    AcceptAssetSlotDrop(widget, slot, false, allowLayerMaterials);
    const auto* source = library.Find(slot);
    DrawAssetSourceButton(source ? source->assetPath : std::filesystem::path{}, revealRequest);
    ui::PropertyEnd();
    return changed;
}

// テクスチャを選ぶコンボ。行の中に置く部品。
inline bool DrawTextureCombo(const char* id, compositor::TextureId& slot,
                      const compositor::TextureLibrary& library, float width, std::filesystem::path& revealRequest) {
    const uint32_t widget = ImGui::GetID(id);
    std::string preview = ui::Tr("なし", "None");
    bool missing = false;
    if (const compositor::LibraryTexture* current = library.Find(slot); current != nullptr) {
        preview = current->name;
        missing = current->missing;
    }

    bool changed = g_assetSelectionContext && g_assetSelectionContext->Consume(widget, slot);
    ImGui::SetNextItemWidth(AssetReferenceWidth(width));
    // リンク切れの画像を指しているときは、名前を警告色で出す。
    // 割り当ては保ってあるので「なし」とは違い、繋ぎ直せば戻る。
    if (missing) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
    }
    const bool open = ImGui::BeginCombo(id, preview.c_str());
    if (missing) {
        ImGui::PopStyleColor();
    }
    if (open) {
        ui::ComboFilterInput();
        if (!ui::ComboFilterActive() && ImGui::Selectable(ui::Tr("なし", "None"), slot == compositor::kNoTexture)) {
            slot = compositor::kNoTexture;
            changed = true;
        }
        for (const compositor::LibraryTexture& entry : library.Entries()) {
            if (!ui::ComboFilterPass(entry.name)) continue;
            ImGui::PushID(static_cast<int>(entry.id));
            if (entry.missing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
            }
            if (ImGui::Selectable(entry.name.c_str(), slot == entry.id)) {
                slot = entry.id;
                changed = true;
            }
            if (entry.missing) {
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        DrawUnloadedAssetChoices(widget, slot, true, false, [&](const auto& path) {
            for (const auto& entry : library.Entries()) if (AssetSlotPathMatches(entry.path, path)) return true;
            return false;
        });
        ImGui::EndCombo();
    }

    // テクスチャ一覧からドラッグしてきた画像を受ける。
    // ドラッグ中にコンボは開けないので、直前のアイテムは必ずコンボ本体になる。
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kTextureDragDropType);
            payload != nullptr) {
            slot = *static_cast<const compositor::TextureId*>(payload->Data);
            changed = true;
        }
        ImGui::EndDragDropTarget();
    }

    // 割り当ててあるものをホバーで出す。コンボの幅では名前が入りきらず、
    // `T_Dusty_Gravel_Grou...` のように切れて見分けられないため。
    //
    // **ドロップの受け口より後に置くこと。** SetTooltip は内部でウィンドウを作るので、
    // 先に呼ぶと BeginDragDropTarget が見る「直前のアイテム」が変わってしまう。
    // ドラッグ中は ImGui 自身がプレビューを出すので、重ねない。
    // 遅延はプロパティ行のツールチップと揃える（即座には出さない）。
    AcceptAssetSlotDrop(widget, slot, true, false);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
        ImGui::GetDragDropPayload() == nullptr) {
        if (const compositor::LibraryTexture* current = library.Find(slot); current != nullptr) {
            if (current->missing) {
                ImGui::SetTooltip(ui::Tr("%s\nリンク切れ: ファイルが見つかりません。"
                                         "テクスチャ一覧の右クリックから繋ぎ直す",
                                         "%s\nBroken link: file not found. "
                                         "Relink it from the texture list's right-click menu"),
                                  current->name.c_str());
            } else {
                ImGui::SetTooltip("%s\n%u x %u", current->name.c_str(), current->texture.width,
                                  current->texture.height);
            }
        } else {
            ImGui::SetTooltip("%s", ui::Tr("なし\nテクスチャ一覧からドラッグしても割り当てられる", "None\nYou can also drag one from the texture list to assign it"));
        }
    }
    const auto* source = library.Find(slot);
    ImGui::PushID(id);
    DrawAssetSourceButton(source ? source->path : std::filesystem::path{}, revealRequest);
    ImGui::PopID();
    return changed;
}

// 川筋（フロー累積）マスクの設定行。レイヤーのマスクとマスクノードの両方から使う。
// **プロパティテーブルの中で呼ぶこと。**
// Wind Field の行。風・格子・粉雪の判定。
inline bool DrawWindRows(compositor::WindParams& wind) {
    const compositor::WindParams defaults;
    bool changed = false;
    changed |= ui::PropertyFloat(ui::Tr("風向", "Wind Direction"), &wind.directionDegrees, 0.0f, 360.0f, defaults.directionDegrees,
                                 ui::Tr("風が吹いていく向き。0 で +Z、90 で +X（雲の風向と同じ）", "Direction the wind blows toward. 0 is +Z, 90 is +X (same as the cloud wind)"), "%.0f °");
    changed |= ui::PropertyFloat(ui::Tr("風速", "Wind Speed"), &wind.speedMetersPerSecond, 0.0f, 60.0f, defaults.speedMetersPerSecond,
                                 ui::Tr("地形にぶつける一様な風の速さ（m/s）", "Speed of the uniform wind blown at the terrain (m/s)"), "%.1f m/s");
    changed |= ui::PropertyFloat(ui::Tr("計算する高さ", "Domain Height"), &wind.heightMeters, 10.0f, 3000.0f, defaults.heightMeters,
                                 ui::Tr("地形の最高点からどこまで上を計算するか（m）。低いと稜線の上で流れが詰まる", "How far above the highest terrain point to simulate (m). Too low and the flow chokes over ridges"),
                                 "%.0f m", ImGuiSliderFlags_Logarithmic);
    int resolution = static_cast<int>(wind.resolution);
    if (ui::PropertyInt(ui::Tr("格子（水平）", "Grid (Horizontal)"), &resolution, 32, 512, static_cast<int>(defaults.resolution),
                        ui::Tr("水平の格子の一辺（セル数）。上げるほど細かい地形に沿うが、反復のコストが増える", "Cells per side of the horizontal grid. Higher follows finer terrain but each iteration costs more"))) {
        wind.resolution = static_cast<uint32_t>(resolution); changed = true;
    }
    int layers = static_cast<int>(wind.layers);
    if (ui::PropertyInt(ui::Tr("格子（鉛直）", "Grid (Vertical)"), &layers, 4, 128, static_cast<int>(defaults.layers),
                        ui::Tr("鉛直の層数。標高差と計算する高さの合計をこの数で割った厚さが 1 層。矢印は地表から半層上の風", "Number of vertical layers. Each is (elevation range + domain height) / this count thick. Arrows show the wind half a layer above ground"))) {
        wind.layers = static_cast<uint32_t>(layers); changed = true;
    }
    changed |= ui::PropertyInt(ui::Tr("反復", "Iterations"), &wind.iterations, 2, 512, defaults.iterations,
                               ui::Tr("圧力の反復回数。少ないと地形を避けきらず、稜線の吹き上げが弱い", "Pressure iterations. Too few and the flow does not fully avoid the terrain, weakening updrafts over ridges"));
    changed |= ui::PropertyFloat(ui::Tr("粉雪のしきい値", "Drift Threshold"), &wind.spindriftThreshold, 0.0f, 60.0f, defaults.spindriftThreshold,
                                 ui::Tr("Spindrift が出始める地表の風速（m/s）", "Surface wind speed at which Spindrift starts (m/s)"), "%.1f m/s");
    changed |= ui::PropertyFloat(ui::Tr("粉雪の幅", "Drift Range"), &wind.spindriftRange, 0.1f, 60.0f, defaults.spindriftRange,
                                 ui::Tr("しきい値からこれだけ超えたら Spindrift が 1（m/s）", "Spindrift reaches 1 this far above the threshold (m/s)"), "%.1f m/s");
    changed |= ui::PropertyFloat(ui::Tr("風下の傾き", "Lee Slope"), &wind.leeSlope, 0.01f, 2.0f, defaults.leeSlope,
                                 ui::Tr("風の向きに地形がこの傾き（高さ / 距離）で下る所を風下とみなし、Spindrift を 1 にする", "Where the terrain drops downwind at this slope (height / distance), it counts as lee and Spindrift is 1"),
                                 "%.2f", ImGuiSliderFlags_Logarithmic);
    return changed;
}

inline bool DrawFluvialRows(compositor::FluvialParams& fluvial) {
    const compositor::FluvialParams defaults;
    bool changed = false;

    int curve = static_cast<int>(fluvial.curve);
    if (ui::PropertyCombo(ui::Tr("カーブ", "Curve"), &curve, ui::Tr(kFluvialCurveLabels).data(),
                          IM_ARRAYSIZE(kFluvialCurveLabels),
                          static_cast<int>(defaults.curve),
                          ui::Tr("対数は細い支流まで見える連続的な川筋、"
                                 "しきい値は川とみなす所だけを抜く、線形は主流が強く出る",
                                 "Logarithmic shows continuous channels down to thin tributaries, "
                                 "Threshold keeps only what counts as river, Linear emphasizes main streams"))) {
        fluvial.curve = static_cast<compositor::FluvialCurve>(curve);
        changed = true;
    }

    const bool isThreshold = (fluvial.curve == compositor::FluvialCurve::Threshold);
    changed |= ui::PropertyFloat(
        ui::Tr("しきい値", "Threshold"), &fluvial.threshold, 0.0f, 0.05f, defaults.threshold,
        isThreshold ? ui::Tr("これより多く水が集まる所を川とみなす（全セル数に対する割合）", "Where more water than this collects counts as river (fraction of all cells)")
                    : ui::Tr("これ未満の流量を切り捨てる（全セル数に対する割合）", "Flow below this is discarded (fraction of all cells)"),
        "%.4f", 0, 0.0005f);
    if (isThreshold) {
        changed |= ui::PropertyFloat(ui::Tr("やわらかさ", "Softness"), &fluvial.softness, 0.001f, 2.0f,
                                     defaults.softness,
                                     ui::Tr("しきい値の前後をどれだけなだらかに繋ぐか", "How smoothly to transition around the threshold"), "%.3f");
        changed |= ui::PropertyFloat(ui::Tr("川縁", "Edge Power"), &fluvial.edgePower, 0.1f, 8.0f,
                                     defaults.edgePower,
                                     ui::Tr("1 より大きいと川が細く、小さいと太くなる", "Above 1 makes rivers thinner, below 1 wider"), "%.2f");
    } else {
        changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &fluvial.gamma, 0.05f, 4.0f, defaults.gamma,
                                     ui::Tr("下げると細い支流が明るくなり、上げると主流だけが残る", "Lower brightens thin tributaries; higher keeps only the main streams"),
                                     "%.2f");
    }

    changed |= ui::PropertyFloat(
        ui::Tr("最大ディテール", "Max Detail"), &fluvial.detailMeters, 1.0f, 512.0f, defaults.detailMeters,
        ui::Tr("流向を読む前にならす大きさ（m）。大きいほど小さな凹凸を無視して大きな谷筋を追う", "Smoothing size before reading flow direction (m). Larger ignores small bumps and follows larger valleys"),
        "%.1f m", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(ui::Tr("集中度", "Concentration"), &fluvial.concentration, 0.1f, 16.0f,
                                 defaults.concentration,
                                 ui::Tr("下流への配分の集中度。大きいほど主流へ集まり、"
                                        "小さいほど面的に広がる",
                                        "How concentrated the downstream flow is. Higher gathers into main streams, "
                                        "lower spreads over the surface"),
                                 "%.2f");

    int resolutionIndex = 1;
    for (int i = 0; i < IM_ARRAYSIZE(kFluvialResolutionValues); ++i) {
        if (kFluvialResolutionValues[i] == fluvial.resolution) {
            resolutionIndex = i;
        }
    }
    if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &resolutionIndex, kFluvialResolutionLabels,
                          IM_ARRAYSIZE(kFluvialResolutionLabels), 1,
                          ui::Tr("川筋を計算するグリッド。合成解像度とは別。"
                                 "上げるほど細かい支流が出るが、反復回数も比例して増える",
                                 "Grid for computing channels, separate from the composite resolution. "
                                 "Higher shows finer tributaries, but iterations grow proportionally"))) {
        fluvial.resolution = kFluvialResolutionValues[resolutionIndex];
        changed = true;
    }
    return changed;
}

// 曲率マスクの設定行。プロパティテーブルの中で呼ぶこと。
inline bool DrawCurvatureRows(compositor::CurvatureParams& curvature) {
    const compositor::CurvatureParams defaults;
    bool changed = false;
    int mode = static_cast<int>(curvature.mode);
    if (ui::PropertyCombo(ui::Tr("向き", "Mode"), &mode, ui::Tr(kCurvatureModeLabels).data(),
                          IM_ARRAYSIZE(kCurvatureModeLabels), static_cast<int>(defaults.mode),
                          ui::Tr("周りより高い所（尾根）、低い所（谷）、その両方のどれを拾うか",
                                 "Whether to pick spots higher (ridges) or lower (valleys) than their surroundings, or both"))) {
        curvature.mode = static_cast<compositor::CurvatureMode>(mode);
        changed = true;
    }
    changed |= ui::PropertyFloat(
        ui::Tr("最大ディテール", "Max Detail"), &curvature.detailMeters, 1.0f, 512.0f, defaults.detailMeters,
        ui::Tr("比べる周りの広さ（m）。大きいほど小さな凹凸を無視して広い尾根や谷を拾う", "Size of the neighborhood to compare (m). Larger ignores small bumps and picks broad ridges and valleys"),
        "%.1f m", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(
        ui::Tr("感度", "Sensitivity"), &curvature.sensitivityMeters, 0.01f, 100.0f, defaults.sensitivityMeters,
        ui::Tr("この高さの差で 1 になる。小さいほど弱い曲率まで明るくなる", "Height difference that maps to 1. Smaller brightens weaker curvature too"), "%.2f m",
        ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(ui::Tr("しきい値", "Threshold"), &curvature.threshold, 0.0f, 0.99f,
                                 defaults.threshold, ui::Tr("これ以下の弱い曲率を捨てる", "Discards curvature weaker than this"), "%.2f");
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &curvature.gamma, 0.05f, 4.0f, defaults.gamma,
                                 ui::Tr("1 未満で弱い曲率を明るく、1 より大きいと強い所だけを残す", "Below 1 brightens weak curvature; above 1 keeps only strong areas"),
                                 "%.2f");
    return changed;
}

// 標高マスクの設定行。**プロパティテーブルの中で呼ぶこと。**
// Mask Path の行。形（幅 / フェザー / 強さ）はパスの点が持つので、ここは調整だけ。
inline bool DrawPathMaskRows(compositor::PathMaskParams& params) {
    const compositor::PathMaskParams defaults;
    bool changed = false;
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &params.gamma, 0.05f, 8.0f, defaults.gamma,
                                 ui::Tr("フェザーのカーブ。1 未満で外側まで明るく、1 より大きいと中心へ締まる", "Feather curve. Below 1 stays bright farther out; above 1 tightens toward the center"),
                                 "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &params.invert, defaults.invert, ui::Tr("白黒を入れ替える", "Swaps black and white"));
    return changed;
}

// Mask Area の行。閉じた鎖の内側を塗るので、縁の扱いだけを持つ。
inline bool DrawAreaMaskRows(compositor::AreaMaskParams& params) {
    const compositor::AreaMaskParams defaults;
    bool changed = false;
    changed |= ui::PropertyFloat(ui::Tr("縁のぼかし", "Edge Blur"), &params.featherMeters, 0.0f, 200.0f,
                                 defaults.featherMeters,
                                 ui::Tr("多角形の縁の外側を 0 へ落とす幅（m）。0 で二値", "Width outside the polygon edge that falls off to 0 (m). 0 is binary"), "%.1f m",
                                 ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(ui::Tr("縁のずれ", "Edge Offset"), &params.offsetMeters, -200.0f, 200.0f,
                                 defaults.offsetMeters,
                                 ui::Tr("縁を外へ（正）/ 内へ（負）ずらす距離（m）。"
                                        "線を引いた所より少し広く / 狭く取りたいときに",
                                        "Distance to move the edge outward (positive) / inward (negative) (m). "
                                        "Use it to take slightly more / less than the drawn line"),
                                 "%.1f m");
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &params.gamma, 0.05f, 8.0f, defaults.gamma,
                                 ui::Tr("ぼかしのカーブ。1 未満で外側まで明るく、1 より大きいと内側へ締まる", "Blur curve. Below 1 stays bright farther out; above 1 tightens inward"),
                                 "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &params.invert, defaults.invert, ui::Tr("白黒を入れ替える", "Swaps black and white"));
    return changed;
}

// Mask Mesh の行。形はメッシュの三角形が持つので、外側への余白と縁の扱いだけ。
inline bool DrawMeshMaskRows(compositor::MeshMaskParams& params) {
    const compositor::MeshMaskParams defaults;
    bool changed = false;
    changed |= ui::PropertyFloat(ui::Tr("余白", "Margin"), &params.marginMeters, 0.0f, 50.0f, defaults.marginMeters,
                                 ui::Tr("メッシュの外側へ広げる幅（m）。路肩の外まで木を生やさないときなどに", "Width to expand outside the mesh (m). E.g. to keep trees off beyond the shoulder"),
                                 "%.1f m");
    changed |= ui::PropertyFloat(ui::Tr("フェザー", "Feather"), &params.featherMeters, 0.0f, 50.0f, defaults.featherMeters,
                                 ui::Tr("余白の外側を 0 へ落とす幅（m）。0 で二値", "Width beyond the margin that falls off to 0 (m). 0 is binary"), "%.1f m",
                                 ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &params.gamma, 0.05f, 8.0f, defaults.gamma,
                                 ui::Tr("フェザーのカーブ。1 未満で外側まで明るく、1 より大きいと内側へ締まる", "Feather curve. Below 1 stays bright farther out; above 1 tightens inward"),
                                 "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &params.invert, defaults.invert,
                                ui::Tr("白黒を入れ替える。植生の除外に使うときはオン", "Swaps black and white. Turn on when excluding vegetation"));
    return changed;
}

inline bool DrawFlowlineRows(compositor::FlowlineParams& p) {
    const compositor::FlowlineParams d;
    bool changed = false;
    const char* modes[] = {ui::Tr("本数", "Count"), ui::Tr("密度", "Density")};
    changed |= ui::PropertyCombo(ui::Tr("発生方法", "Spawn Mode"), &p.scatteringMode, modes, 2, d.scatteringMode);
    if (p.scatteringMode == 0)
        changed |= ui::PropertyInt(ui::Tr("本数", "Count"), &p.numberOfFlows, 1, 1000000, d.numberOfFlows);
    else
        changed |= ui::PropertyFloat(ui::Tr("密度", "Density"), &p.density, 0.0f, 1.0f, d.density, ui::Tr("解像度と地形の幅に応じた粒子密度", "Particle density scaled by resolution and terrain width"), "%.4f");
    changed |= ui::PropertyFloat(ui::Tr("長さ", "Length"), &p.lengthMeters, 0.0f, 1024.0f, d.lengthMeters, ui::Tr("粒子が移動する最大距離", "Max distance a particle travels"), "%.2f m");
    changed |= ui::PropertyFloat(ui::Tr("摩擦", "Friction"), &p.friction, 0.01f, 1.0f, d.friction, ui::Tr("大きいほど慣性が減り、局所的な傾斜に沿う", "Higher reduces inertia so particles follow local slopes"));
    changed |= ui::PropertyBool(ui::Tr("障害物の回避", "Avoid Obstacles"), &p.reflectVelocity, d.reflectVelocity);
    if (p.reflectVelocity)
        changed |= ui::PropertyFloat(ui::Tr("回避量", "Avoidance"), &p.reflectionAmount, 0.0f, 1.0f, d.reflectionAmount);
    changed |= ui::PropertyBool(ui::Tr("窪みに滞留", "Allow Pooling"), &p.allowPooling, d.allowPooling, ui::Tr("無効なら周囲より低い窪みで粒子を止める", "When off, particles stop in pits lower than their surroundings"));
    changed |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &p.strength, 0.0f, 1.0f, d.strength, ui::Tr("流跡へ加算する強さ", "Amount added to the flow trail"), "%.4f");
    changed |= ui::PropertyFloat(ui::Tr("流れの分散", "Flow Dispersion"), &p.volume, 0.0f, 1.0f, d.volume, ui::Tr("既に流れた場所を避けて流跡を分散する", "Spreads trails by avoiding places already flowed through"), "%.4f");
    changed |= ui::PropertyFloat(ui::Tr("最小傾斜", "Min Slope"), &p.talusAngle, 0.0f, 90.0f, d.talusAngle, ui::Tr("これより緩い斜面では流跡を弱める", "Weakens trails on slopes gentler than this"), ui::Tr("%.1f 度", "%.1f°"));
    changed |= ui::PropertyFloat(ui::Tr("傾斜の遷移幅", "Slope Falloff"), &p.talusFalloff, 0.0f, 90.0f, d.talusFalloff, ui::Tr("最小傾斜の手前で流跡を徐々に弱める角度幅", "Angle range below Min Slope over which trails fade"), ui::Tr("%.1f 度", "%.1f°"));
    changed |= ui::PropertyBool(ui::Tr("発生値を制限", "Clamp Source"), &p.clampSource, d.clampSource, ui::Tr("Source 入力を 0〜1 に制限する", "Clamps the Source input to 0-1"));
    changed |= ui::PropertyBool(ui::Tr("流出域を加算", "Add Outflow"), &p.showOutflow, d.showOutflow, ui::Tr("Outflow 入力を最終マスクに加える", "Adds the Outflow input to the final mask"));
    changed |= ui::PropertyBool(ui::Tr("出力を圧縮", "Compress Output"), &p.normalize, d.normalize, ui::Tr("値 / (1 + 値) で 0〜1 に圧縮する。流出域の加算はその後に行う", "Compresses to 0-1 with v / (1 + v). Outflow is added afterward"));
    return changed;
}

inline bool DrawHeightMaskRows(compositor::HeightParams& height) {
    const compositor::HeightParams defaults;
    bool changed = false;
    changed |= ui::PropertyBool(ui::Tr("全範囲", "Full Range"), &height.useFullRange, defaults.useFullRange,
                                ui::Tr("地形の一番低い所を 0、一番高い所を 1 にして、"
                                       "標高そのものをグラデーションにする。"
                                       "入れると下の 3 行は使わない",
                                       "Maps the lowest terrain to 0 and the highest to 1, "
                                       "making the elevation itself a gradient. "
                                       "When on, the 3 rows below are unused"));
    if (!height.useFullRange) {
        changed |= ui::PropertyFloat(ui::Tr("最低標高", "Min Elevation"), &height.minMeters, 0.0f, 8000.0f,
                                     defaults.minMeters, ui::Tr("これより低い所を 0 にする", "Areas below this become 0"),
                                     "%.1f m");
        changed |= ui::PropertyFloat(ui::Tr("最高標高", "Max Elevation"), &height.maxMeters, 0.0f, 8000.0f,
                                     defaults.maxMeters, ui::Tr("これより高い所を 0 にする", "Areas above this become 0"),
                                     "%.1f m");
        changed |= ui::PropertyFloat(ui::Tr("フェザー", "Feather"), &height.featherMeters, 0.0f, 1000.0f,
                                     defaults.featherMeters,
                                     ui::Tr("標高帯の上下の境目をぼかす幅。0 で硬い帯になる", "Width that softens the band's upper and lower edges. 0 gives a hard band"),
                                     "%.1f m", ImGuiSliderFlags_Logarithmic);
    }
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &height.gamma, 0.05f, 4.0f, defaults.gamma,
                                 ui::Tr("1 未満で境目の弱い所を明るく、"
                                        "1 より大きいと帯の中心だけを残す",
                                        "Below 1 brightens the weak edges; "
                                        "above 1 keeps only the band's center"),
                                 "%.2f");
    changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &height.invert, defaults.invert,
                                ui::Tr("指定した標高帯の外側のマスクを作るときに使う", "Use to make a mask outside the given elevation band"));
    return changed;
}

// 傾斜マスクの設定行。**プロパティテーブルの中で呼ぶこと。**
inline bool DrawSlopeRows(compositor::SlopeParams& slope) {
    const compositor::SlopeParams defaults;
    bool changed = false;
    changed |= ui::PropertyFloat(
        ui::Tr("最大ディテール", "Max Detail"), &slope.detailMeters, 0.0f, 512.0f, defaults.detailMeters,
        ui::Tr("傾斜を測る距離（m）。大きいほど小さな凹凸を無視して大きな斜面を拾う", "Distance over which slope is measured (m). Larger ignores small bumps and picks broad slopes"),
        "%.1f m", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(ui::Tr("最小角", "Min Angle"), &slope.minDegrees, 0.0f, 89.0f, defaults.minDegrees,
                                 ui::Tr("これ以下の傾斜を 0 にする", "Slopes at or below this become 0"), ui::Tr("%.1f 度", "%.1f°"));
    changed |= ui::PropertyFloat(ui::Tr("最大角", "Max Angle"), &slope.maxDegrees, 1.0f, 90.0f, defaults.maxDegrees,
                                 ui::Tr("これ以上の傾斜を 1 にする", "Slopes at or above this become 1"), ui::Tr("%.1f 度", "%.1f°"));
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &slope.gamma, 0.05f, 4.0f, defaults.gamma,
                                 ui::Tr("1 未満で弱い斜面を明るく、1 より大きいと急斜面だけを残す", "Below 1 brightens gentle slopes; above 1 keeps only steep ones"),
                                 "%.2f");
    changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &slope.invert, defaults.invert,
                                ui::Tr("平地のマスクを作るときに使う", "Use to make a mask of flat ground"));
    return changed;
}

// レベル調整の設定行。**プロパティテーブルの中で呼ぶこと。**
inline bool DrawLevelsRows(compositor::LevelsParams& levels) {
    const compositor::LevelsParams defaults;
    bool changed = false;
    changed |= ui::PropertyFloat(ui::Tr("黒点", "Black Point"), &levels.blackPoint, 0.0f, 1.0f, defaults.blackPoint,
                                 ui::Tr("これ以下を 0 にする。弱い成分を切り落とす", "Values at or below this become 0. Cuts weak components"), "%.3f");
    changed |= ui::PropertyFloat(ui::Tr("白点", "White Point"), &levels.whitePoint, 0.0f, 1.0f, defaults.whitePoint,
                                 ui::Tr("これ以上を 1 にする。下げるほど早く飽和する", "Values at or above this become 1. Lower saturates sooner"), "%.3f");
    changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &levels.gamma, 0.05f, 4.0f, defaults.gamma,
                                 ui::Tr("1 未満で暗部を持ち上げ、1 より大きいと強い部分だけを残す", "Below 1 lifts dark areas; above 1 keeps only strong areas"),
                                 "%.2f");
    changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &levels.invert, defaults.invert, nullptr);
    return changed;
}

// マスクのぼかしの設定行。**プロパティテーブルの中で呼ぶこと。**
// つまみはハイトのぼかし（Heightmap Blur）と同じ形に揃えてある。
inline bool DrawMaskBlurRows(compositor::MaskBlurParams& blur) {
    const compositor::MaskBlurParams defaults;
    bool changed = false;
    // 半径は実寸（m）。合成解像度を変えても効きが変わらない。
    changed |= ui::PropertyFloat(ui::Tr("半径", "Radius"), &blur.radiusMeters, 0.0f, 512.0f, defaults.radiusMeters,
                                 ui::Tr("ぼかす範囲（m）。大きいほど境界がなだらかになる", "Blur extent (m). Larger gives smoother boundaries"), "%.2f m",
                                 ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &blur.strength, 0.0f, 1.0f, defaults.strength,
                                 ui::Tr("元のマスクとぼかしたマスクを混ぜる量。1 で完全なぼかし", "Mix between the original and blurred mask. 1 is fully blurred"),
                                 "%.2f");
    changed |= ui::PropertyInt(ui::Tr("反復", "Iterations"), &blur.iterations, 1, 16, defaults.iterations,
                               ui::Tr("ぼかしを重ねる回数。多いほど広く均される"
                                      "（実効半径はおよそ 半径 x sqrt(反復)）",
                                      "Number of blur passes. More smooths a wider area "
                                      "(effective radius is about Radius x sqrt(Iterations))"));
    return changed;
}

// マスクの合成の設定行。**プロパティテーブルの中で呼ぶこと。**
inline bool DrawBlendRows(compositor::BlendParams& blend) {
    const compositor::BlendParams defaults;
    bool changed = false;
    int mode = static_cast<int>(blend.mode);
    if (ui::PropertyCombo(ui::Tr("種類", "Type"), &mode, ui::Tr(kMaskBlendModeLabels).data(),
                          IM_ARRAYSIZE(kMaskBlendModeLabels), static_cast<int>(defaults.mode),
                          ui::Tr("乗算は絞り込み（両方が立つ所だけ）、加算は合算、"
                                 "小さいほう / 大きいほうは飽和させずに寄せる。"
                                 "減算は前景から背景を引く（背景が立つ所を除く。0 で切る）",
                                 "Multiply narrows (only where both are set), Add sums, "
                                 "Min / Max combine without saturating. "
                                 "Subtract takes the background from the foreground (removes where the background is set, clamped at 0)"))) {
        blend.mode = static_cast<compositor::MaskBlendMode>(mode);
        changed = true;
    }
    changed |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &blend.intensity, 0.0f, 1.0f, defaults.intensity,
                                 ui::Tr("前景そのままと合成結果の間の補間。0 で前景のまま", "Interpolates between the plain foreground and the blend result. 0 keeps the foreground"), "%.2f");
    return changed;
}

// テクスチャスロットを選ぶ行。RGB をそのまま使うマップ（ベースカラー / 法線）用。
inline bool DrawTextureSlotRow(const char* label, compositor::TextureId& slot,
                        const compositor::TextureLibrary& library, std::filesystem::path& revealRequest) {
    ui::PropertyLabel(label, ui::Tr("「なし」なら定数値を使う", "If None, the constant value is used"));
    const float width =
        std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x);
    const bool changed = DrawTextureCombo("##value", slot, library, width, revealRequest);
    ui::PropertyEnd();
    return changed;
}

// スカラーのマップを選ぶ行。テクスチャに加えて、どのチャンネルを読むかも選ぶ。
// Megascans の _ORD のように 1 枚へ複数のマップを詰めたテクスチャがあるため。
inline bool DrawMapSlotRow(const char* label, compositor::MapSlot& slot,
                    const compositor::TextureLibrary& library, std::filesystem::path& revealRequest) {
    ui::PropertyLabel(label, ui::Tr("「なし」なら定数値を使う。右は読むチャンネル", "If None, the constant value is used. The right box picks the channel to read"));

    const float channelWidth = ui::Scaled(52.0f);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    const float comboWidth =
        std::max(ui::Scaled(60.0f),
                 std::min(ui::Scaled(ui::kComboMaxWidth), available) - channelWidth - spacing);

    bool changed = DrawTextureCombo("##texture", slot.texture, library, comboWidth, revealRequest);

    if (slot.texture != compositor::kNoTexture) {
        ImGui::SameLine(0.0f, spacing);
        static const char* const kTextureChannelLabels[] = {"R", "G", "B", "A"};
        int channel = static_cast<int>(slot.channel);
        ImGui::SetNextItemWidth(channelWidth);
        if (ImGui::Combo("##channel", &channel, kTextureChannelLabels,
                         IM_ARRAYSIZE(kTextureChannelLabels))) {
            slot.channel = static_cast<compositor::TextureChannel>(channel);
            changed = true;
        }
    }

    ui::PropertyEnd();
    return changed;
}

// ライトのギズモが残る時間（秒）。掴むのをやめてから薄くなって消える。
inline constexpr double kLightGizmoFadeSeconds = 0.35;
// ライトを掴んだときの感度。参考にした terrain-editor と同じ 0.25 度 / ピクセル。
inline constexpr float kLightDegreesPerPixel = 0.25f;
// 「緯度経度と日時」のときの L + 左ドラッグ。横 1 ピクセルで進める時刻（分）。
// 0.25 度/px の方位角と同じ感覚になるよう、1 日（360 度相当）が 720 px になる値。
inline constexpr float kLightMinutesPerPixel = 2.0f;

// ビューポートに重ねる線を描くための投影。カメラの行列をそのまま使う。
struct ProjectedPoint {
    ImVec2 screen{};
    bool visible = false;
};

// ビューポートのカーソル位置からワールドへ飛ばすレイ（原点は近クリップ面の上）。
// 投影が逆行列を持たなければ偽。
inline bool ViewportMouseRay(const DirectX::XMMATRIX& viewProjection, const ImVec2& mouse, const ImVec2& min,
                             const ImVec2& size, DirectX::XMFLOAT3& outOrigin, DirectX::XMFLOAT3& outDirection) {
    using namespace DirectX;
    if (size.x <= 0.0f || size.y <= 0.0f) return false;
    XMVECTOR determinant;
    const XMMATRIX inverse = XMMatrixInverse(&determinant, viewProjection);
    if (XMVectorGetX(determinant) == 0.0f) return false;
    const float ndcX = ((mouse.x - min.x) / size.x) * 2.0f - 1.0f;
    const float ndcY = 1.0f - ((mouse.y - min.y) / size.y) * 2.0f;
    const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
    const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
    XMStoreFloat3(&outOrigin, nearPoint);
    XMStoreFloat3(&outDirection, XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint)));
    return true;
}

inline ProjectedPoint ProjectToViewport(const DirectX::XMMATRIX& viewProjection,
                                 const DirectX::XMFLOAT3& world, const ImVec2& min,
                                 const ImVec2& size) {
    using namespace DirectX;
    const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&world), viewProjection);
    const float w = XMVectorGetW(clip);
    ProjectedPoint out;
    // カメラの後ろに回った点は描かない。
    if (w <= 1e-4f) {
        return out;
    }
    const float ndcX = XMVectorGetX(clip) / w;
    const float ndcY = XMVectorGetY(clip) / w;
    out.screen = ImVec2(min.x + (ndcX * 0.5f + 0.5f) * size.x,
                        min.y + (0.5f - ndcY * 0.5f) * size.y);
    out.visible = true;
    return out;
}

// ライトの向きを示すギズモ。地面のリング、水平方向、仰角の弧、光が来る向きの矢印。
//
// **ビューポートと素材プレビューで同じ絵を使う。** 見る場所が変わっても読み方を
// 変えないため。半径は見ているものの実寸（平面の一辺の半分）で渡す。
// 色はテーマから引かない。座標軸ギズモと同じく「意味を持つ色」として固定する。
inline void DrawLightGizmoOverlay(const DirectX::XMMATRIX& viewProjection, const ImVec2& viewportMin,
                                  const ImVec2& viewportMax, float gizmoRadius, float azimuth,
                                  float elevation, const DirectX::XMFLOAT3& direction, float fade,
                                  const char* text, const ImVec2& textMin) {
    using namespace DirectX;
    const ImVec2 size(viewportMax.x - viewportMin.x, viewportMax.y - viewportMin.y);
    if (size.x <= 0.0f || size.y <= 0.0f || fade <= 0.001f) {
        return;
    }

    const XMFLOAT3 origin{0.0f, 0.0f, 0.0f};
    const XMFLOAT3 horizontal{std::sin(azimuth), 0.0f, std::cos(azimuth)};

    const auto color = [fade](int r, int g, int b, int a) {
        return IM_COL32(r, g, b, static_cast<int>(static_cast<float>(a) * fade));
    };
    const auto offset = [](const XMFLOAT3& base, const XMFLOAT3& dir, float amount) {
        return XMFLOAT3{base.x + dir.x * amount, base.y + dir.y * amount,
                        base.z + dir.z * amount};
    };
    const auto project = [&](const XMFLOAT3& world) {
        return ProjectToViewport(viewProjection, world, viewportMin, size);
    };

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(viewportMin, viewportMax, true);

    const auto drawWorldLine = [&](const XMFLOAT3& a, const XMFLOAT3& b, ImU32 lineColor,
                                   float thickness) {
        const ProjectedPoint pa = project(a);
        const ProjectedPoint pb = project(b);
        if (pa.visible && pb.visible) {
            drawList->AddLine(pa.screen, pb.screen, lineColor, thickness);
        }
    };

    // 地面のリング。方位角の目安になる。
    constexpr int kRingSegments = 72;
    ProjectedPoint previous;
    for (int i = 0; i <= kRingSegments; ++i) {
        const float t = (static_cast<float>(i) / kRingSegments) * 2.0f * 3.14159265f;
        const ProjectedPoint current =
            project(XMFLOAT3{std::sin(t) * gizmoRadius, 0.0f, std::cos(t) * gizmoRadius});
        if (i > 0 && previous.visible && current.visible) {
            drawList->AddLine(previous.screen, current.screen, color(150, 160, 175, 130), 1.6f);
        }
        previous = current;
    }

    // 水平方向への投影と、そこから仰角ぶんの弧。
    drawWorldLine(origin, offset(origin, horizontal, gizmoRadius), color(150, 160, 175, 170), 1.8f);

    constexpr int kArcSegments = 32;
    ProjectedPoint previousArc;
    for (int i = 0; i <= kArcSegments; ++i) {
        const float angle = elevation * (static_cast<float>(i) / kArcSegments);
        const float ring = std::cos(angle) * gizmoRadius;
        const ProjectedPoint current = project(
            XMFLOAT3{horizontal.x * ring, std::sin(angle) * gizmoRadius, horizontal.z * ring});
        if (i > 0 && previousArc.visible && current.visible) {
            drawList->AddLine(previousArc.screen, current.screen, color(255, 206, 112, 150), 1.6f);
        }
        previousArc = current;
    }

    // 光が来る向きの矢印。ライトの位置から原点へ向ける。
    const ProjectedPoint arrowStart = project(offset(origin, direction, gizmoRadius));
    const ProjectedPoint arrowEnd = project(offset(origin, direction, gizmoRadius * 0.22f));
    if (arrowStart.visible && arrowEnd.visible) {
        const ImU32 lightColor = color(255, 188, 76, 245);
        ImVec2 screenDir(arrowEnd.screen.x - arrowStart.screen.x,
                         arrowEnd.screen.y - arrowStart.screen.y);
        const float length = std::sqrt(screenDir.x * screenDir.x + screenDir.y * screenDir.y);
        if (length > 0.001f) {
            screenDir.x /= length;
            screenDir.y /= length;
            const ImVec2 side(-screenDir.y, screenDir.x);
            const float head = ui::Scaled(14.0f);
            const float halfWidth = ui::Scaled(6.0f);
            const ImVec2 base(arrowEnd.screen.x - screenDir.x * head,
                              arrowEnd.screen.y - screenDir.y * head);
            drawList->AddLine(arrowStart.screen, base, lightColor, ui::Scaled(3.5f));
            drawList->AddTriangleFilled(
                arrowEnd.screen, ImVec2(base.x + side.x * halfWidth, base.y + side.y * halfWidth),
                ImVec2(base.x - side.x * halfWidth, base.y - side.y * halfWidth), lightColor);
        }
    }

    if (const ProjectedPoint center = project(origin); center.visible) {
        drawList->AddCircle(center.screen, ui::Scaled(5.0f), color(200, 210, 220, 200), 20, 1.6f);
    }

    // いまの値。掴んだまま数字を確かめられるようにする。
    if (text != nullptr && text[0] != 0) {
        const ImVec2 textSize = ImGui::CalcTextSize(text);
        const ImVec2 padding(ui::Scaled(8.0f), ui::Scaled(5.0f));
        const ImVec2 textMax(textMin.x + textSize.x + padding.x * 2.0f,
                             textMin.y + textSize.y + padding.y * 2.0f);
        drawList->AddRectFilled(textMin, textMax, color(8, 10, 12, 190), ui::Scaled(4.0f));
        drawList->AddText(ImVec2(textMin.x + padding.x, textMin.y + padding.y),
                          color(235, 235, 235, 255), text);
    }

    drawList->PopClipRect();
}

// ビューポート左下に置く座標軸ギズモ。
//
// 軸の色は DCC 共通の意味色（X=赤 / Y=緑 / Z=青）なので、テーマからは引かない。
// 透視投影は掛けず、向きだけを見せる。
inline void DrawAxisGizmo(const renderer::Camera& camera, const ImVec2& viewportMin,
                   const ImVec2& viewportMax) {
    const renderer::CameraBasis basis = camera.Basis();

    const float radius = ui::Scaled(30.0f);
    const float margin = ui::Scaled(16.0f);
    const ImVec2 center(viewportMin.x + margin + radius, viewportMax.y - margin - radius);
    if (center.x + radius > viewportMax.x || center.y - radius < viewportMin.y) {
        return;  // ビューポートが小さすぎる。
    }

    struct Axis {
        float direction[3];
        ImU32 color;
        const char* label;
    };
    static const Axis kAxes[] = {
        {{1.0f, 0.0f, 0.0f}, IM_COL32(226, 96, 96, 255), "X"},
        {{0.0f, 1.0f, 0.0f}, IM_COL32(124, 196, 104, 255), "Y"},
        {{0.0f, 0.0f, 1.0f}, IM_COL32(96, 146, 226, 255), "Z"},
    };

    struct Projected {
        ImVec2 tip;
        float depth = 0.0f;  // 正なら画面の奥を向いている
        ImU32 color = 0;
        const char* label = nullptr;
    };

    const auto project = [](const float* axis, const DirectX::XMFLOAT3& b) {
        return axis[0] * b.x + axis[1] * b.y + axis[2] * b.z;
    };

    Projected projected[IM_ARRAYSIZE(kAxes)];
    for (int i = 0; i < IM_ARRAYSIZE(kAxes); ++i) {
        const float x = project(kAxes[i].direction, basis.right);
        const float y = project(kAxes[i].direction, basis.up);
        projected[i].depth = project(kAxes[i].direction, basis.forward);
        projected[i].tip = ImVec2(center.x + x * radius, center.y - y * radius);
        projected[i].label = kAxes[i].label;

        // 奥を向いている軸は落として、手前と見分けられるようにする。
        const ImU32 color = kAxes[i].color;
        projected[i].color = (projected[i].depth > 0.0f)
                                 ? ((color & ~IM_COL32_A_MASK) | (110u << IM_COL32_A_SHIFT))
                                 : color;
    }

    // 奥の軸から先に描く。
    std::sort(std::begin(projected), std::end(projected),
              [](const Projected& a, const Projected& b) { return a.depth > b.depth; });

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    // 文字を置くぶん、線を先端の手前で止める幅。
    const float labelGap = ui::Scaled(7.0f);
    for (const Projected& axis : projected) {
        const ImVec2 delta(axis.tip.x - center.x, axis.tip.y - center.y);
        const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);

        // **線は文字の手前で止める。** 先端に丸を置かないので、
        // 文字まで引くと字画と重なって読めなくなる。
        // 真正面を向いた軸は線が点に潰れるため、文字だけを置く。
        if (length > labelGap) {
            const ImVec2 direction(delta.x / length, delta.y / length);
            const ImVec2 lineEnd(axis.tip.x - direction.x * labelGap,
                                 axis.tip.y - direction.y * labelGap);
            drawList->AddLine(center, lineEnd, axis.color, ui::Scaled(1.6f));
        }

        // **文字は軸の色で描く。** 下に敷く丸が無いので、
        // 暗い色だとビューポートの背景に沈んで読めない。
        const ImVec2 textSize = ImGui::CalcTextSize(axis.label);
        const ImVec2 textPos(axis.tip.x - textSize.x * 0.5f, axis.tip.y - textSize.y * 0.5f);
        drawList->AddText(textPos, axis.color, axis.label);
    }
}


// ノードエディタのドットグリッドの背景（グラフと参照ビューアで共通）。ed::Begin の中で、
// キャンバスの画面上の矩形を渡して呼ぶ。定義は ApplicationGraphPanel.cpp。
void DrawGraphDots(const ImVec2& screenMin, const ImVec2& screenMax);

}  // namespace tg
