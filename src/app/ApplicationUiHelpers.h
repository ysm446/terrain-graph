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
    layer.name = "Liquid";
    layer.baseColor = {0.01f, 0.03f, 0.035f};
    layer.roughness = 0.07f;
    layer.metallic = 0.0f;
    layer.heightSource = compositor::ValueSource::Constant;
    layer.heightBase = 0.35f;   // 水位
    layer.blendRange = 0.01f;   // 汀線のフェザー幅
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

inline const compositor::MaterialLayer& DefaultLayerFor(compositor::LayerKind kind) {
    switch (kind) {
        case compositor::LayerKind::Shape:
            return kDefaultShapeLayer;
        case compositor::LayerKind::Liquid:
            return kDefaultLiquidLayer;
        case compositor::LayerKind::Blur:
            return kDefaultBlurLayer;
        default:
            return kDefaultLayer;
    }
}

// レイヤー一覧のツールチップなどで使う種類の表示名。LayerKind の並びと一致させること。
inline const char* const kLayerKindLabels[] = {"サーフェス", "シェイプ", "水面", "ブラー"};
inline const compositor::BrushSettings kDefaultBrush;
inline const renderer::LightSettings kDefaultLight;
inline const renderer::ExposureSettings kDefaultExposure;
inline const renderer::MaterialSettings kDefaultMaterial;
inline const renderer::CameraState kDefaultCamera;
inline const renderer::SkySettings kDefaultSky;

inline const char* const kNoiseTypeLabels[] = {"fBm", "尾根状", "セル状"};
inline const char* const kValueSourceLabels[] = {"定数", "ノイズ", "テクスチャ"};
inline const char* const kMaskSourceLabels[] = {
    "定数",       "ノイズ",     "テクスチャ", "下地の高さ",
    "下地の傾斜", "下地の曲率", "下地の窪み", "ペイント",
    "下地の川筋",
};
// 川筋マスクの出力カーブ。compositor::FluvialCurve の並びと一致させること。
inline const char* const kFluvialCurveLabels[] = {"対数", "しきい値", "線形"};
// 川筋マスクの計算グリッド。合成解像度とは別に持つ。
inline const char* const kFluvialResolutionLabels[] = {"256", "512", "1024"};
inline constexpr uint32_t kFluvialResolutionValues[] = {256, 512, 1024};
inline const char* const kChannelLabels[] = {"BaseColor", "Normal", "Surface", "Height"};

// ビューポートの表示モード。renderer::DebugView と並びを合わせること。
inline const char* const kDebugViewLabels[] = {
    "シェーディング", "ベースカラー", "法線（接空間）", "法線（ワールド）",
    "ラフネス",       "メタルネス",   "AO",             "ハイト",
    "ワイヤーフレーム",
};
inline const char* const kResolutionLabels[] = {"512", "1024", "2048", "4096"};
inline constexpr uint32_t kResolutionValues[] = {512, 1024, 2048, 4096};

// レイヤー一覧のドラッグ＆ドロップで使うペイロードの種別。
inline constexpr const char* kLayerDragDropType = "TG_LAYER";
// レイヤー一覧の行に並べるサムネイルの一辺（96 DPI 基準）。行の高さはこれで決まる。
// 中身（マテリアルとマスク）を読めることを優先して、文字より大きく取る。
inline constexpr float kLayerRowThumbnail = 40.0f;
// レイヤー一覧の行で、部品どうしと行の左右に空ける間隔（96 DPI 基準）。
// ImGui の ItemInnerSpacing（6）では目・サムネイル・マスク・名前が詰まって
// 1 つの塊に見える。**どれも意味の違う情報なので、読み分けられる間隔を取る。**
inline constexpr float kLayerRowGap = 12.0f;
// 目のアイコンの一辺。**サムネイルより小さくする。**
// 同じ大きさだと切り替えのアイコンが素材と同じ重みで並び、目線が散る。
inline constexpr float kLayerRowEye = 20.0f;
// レイヤーパネルの一覧側（上の区画）の高さの下限と上限（96 DPI 基準）。
// 既定値は AppSettings が持ち、境界のドラッグで変わる。
// 下限はツールバーの 1 行 + 行 2 つ + ヒントの 1 行が入る高さ。
inline constexpr float kLayerListMinHeight = 120.0f;
inline constexpr float kLayerListMaxHeight = 640.0f;

// テクスチャの拡大プレビューの一辺（96 DPI 基準）。
// サムネイル（72）では中身を確かめられないので、その 3 倍弱を取る。
inline constexpr float kTexturePreviewSize = 200.0f;

// テクスチャ一覧からマップ欄へのドラッグ＆ドロップで使うペイロードの種別。
inline constexpr const char* kTextureDragDropType = "TG_TEXTURE";
inline constexpr const char* kTextureRemoveModalTitle = "テクスチャを削除";

// テクスチャの一覧に出すフォーマット名。DXGI の名前は長いので短く言い換える。
inline const char* TextureFormatLabel(const compositor::LibraryTexture& entry) {
    return entry.isFloat ? "RGBA16F (リニア)" : "RGBA8 (sRGB / リニア)";
}

// ステータスバーの通知を残す時間（秒）。情報だけが時間で消える。
inline constexpr float kStatusHoldSeconds = 6.0f;

// ビューポートの背景色の既定値。Application のメンバ初期化と揃えること。
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

// ノイズの種類を選ぶ行。
inline bool DrawNoiseTypeRow(const char* label, compositor::NoiseType& type,
                      compositor::NoiseType defaultType) {
    int selected = static_cast<int>(type);
    if (ui::PropertyCombo(label, &selected, kNoiseTypeLabels, IM_ARRAYSIZE(kNoiseTypeLabels),
                          static_cast<int>(defaultType),
                          "fBm: 一般的な起伏 / 尾根状: 稜線や割れ目 / セル状: 石畳や砂利")) {
        type = static_cast<compositor::NoiseType>(selected);
        return true;
    }
    return false;
}

// ノイズのパラメータをまとめて並べる。ハイトとマスクで共通。
// ハイトでは寄与の量を heightGain が担うので、showAmount を false にして「量」を出さない。
inline bool DrawNoiseRows(compositor::NoiseParams& noise, const compositor::NoiseParams& defaults,
                   bool showAmount = true) {
    bool changed = DrawNoiseTypeRow("種類", noise.type, defaults.type);
    // 周波数はタイルするよう整数へ丸めて評価される（Common.hlsli の SampleNoise）。
    // スライダーの刻みも 1 にして、丸めと表示がずれないようにする。
    changed |= ui::PropertyFloat("周波数", &noise.scale, 1.0f, 64.0f, defaults.scale,
                                 "大きいほど細かい模様になる。タイルさせるため整数で使う",
                                 "%.0f", 0, 1.0f);
    if (showAmount) {
        changed |= ui::PropertyFloat("量", &noise.amount, 0.0f, 3.0f, defaults.amount,
                                     "ノイズの寄与。0 で効かなくなる", "%.2f");
    }
    changed |= ui::PropertyInt("オクターブ", &noise.octaves, 1, 8, defaults.octaves,
                               "重ねる段数。多いほど細部が増え、計算も増える");
    changed |= ui::PropertyFloat("オフセット", &noise.offset, 0.0f, 64.0f, defaults.offset,
                                 "同じ設定で別の模様がほしいときにずらす", "%.1f", 0, 0.5f);
    return changed;
}

// マテリアルを選ぶ行。サムネイル付きの一覧から選ぶ。
inline bool DrawMaterialSlotRow(const char* label, compositor::MaterialAssetId& slot,
                         const compositor::MaterialLibrary& library) {
    ui::PropertyLabel(label, "「なし」ならレイヤーの定数値だけで塗る");

    std::string preview = "なし";
    if (const compositor::MaterialAsset* current = library.Find(slot); current != nullptr) {
        preview = current->name;
    }

    const float thumbnailSize = ImGui::GetFrameHeight();
    bool changed = false;
    ImGui::SetNextItemWidth(
        std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x));
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        if (ImGui::Selectable("なし", slot == compositor::kNoMaterialAsset)) {
            slot = compositor::kNoMaterialAsset;
            changed = true;
        }
        for (const compositor::MaterialAsset& asset : library.Entries()) {
            ImGui::PushID(static_cast<int>(asset.id));
            if (asset.thumbnail.IsValid()) {
                ImGui::Image(static_cast<ImTextureID>(asset.thumbnail.srv.gpu.ptr),
                             ImVec2(thumbnailSize, thumbnailSize));
                ImGui::SameLine();
            }
            if (ImGui::Selectable(asset.name.c_str(), slot == asset.id)) {
                slot = asset.id;
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ui::PropertyEnd();
    return changed;
}

// テクスチャを選ぶコンボ。行の中に置く部品。
inline bool DrawTextureCombo(const char* id, compositor::TextureId& slot,
                      const compositor::TextureLibrary& library, float width) {
    std::string preview = "なし";
    if (const compositor::LibraryTexture* current = library.Find(slot); current != nullptr) {
        preview = current->name;
    }

    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, preview.c_str())) {
        if (ImGui::Selectable("なし", slot == compositor::kNoTexture)) {
            slot = compositor::kNoTexture;
            changed = true;
        }
        for (const compositor::LibraryTexture& entry : library.Entries()) {
            ImGui::PushID(static_cast<int>(entry.id));
            if (ImGui::Selectable(entry.name.c_str(), slot == entry.id)) {
                slot = entry.id;
                changed = true;
            }
            ImGui::PopID();
        }
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
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort) &&
        ImGui::GetDragDropPayload() == nullptr) {
        if (const compositor::LibraryTexture* current = library.Find(slot); current != nullptr) {
            ImGui::SetTooltip("%s\n%u x %u", current->name.c_str(), current->texture.width,
                              current->texture.height);
        } else {
            ImGui::SetTooltip("なし\nテクスチャ一覧からドラッグしても割り当てられる");
        }
    }
    return changed;
}

// 川筋（フロー累積）マスクの設定行。レイヤーのマスクとマスクノードの両方から使う。
// **プロパティテーブルの中で呼ぶこと。**
inline bool DrawFluvialRows(compositor::FluvialParams& fluvial) {
    const compositor::FluvialParams defaults;
    bool changed = false;

    int curve = static_cast<int>(fluvial.curve);
    if (ui::PropertyCombo("カーブ", &curve, kFluvialCurveLabels,
                          IM_ARRAYSIZE(kFluvialCurveLabels),
                          static_cast<int>(defaults.curve),
                          "対数は細い支流まで見える連続的な川筋、"
                          "しきい値は川とみなす所だけを抜く、線形は主流が強く出る")) {
        fluvial.curve = static_cast<compositor::FluvialCurve>(curve);
        changed = true;
    }

    const bool isThreshold = (fluvial.curve == compositor::FluvialCurve::Threshold);
    changed |= ui::PropertyFloat(
        "しきい値", &fluvial.threshold, 0.0f, 0.05f, defaults.threshold,
        isThreshold ? "これより多く水が集まる所を川とみなす（全セル数に対する割合）"
                    : "これ未満の流量を切り捨てる（全セル数に対する割合）",
        "%.4f", 0, 0.0005f);
    if (isThreshold) {
        changed |= ui::PropertyFloat("やわらかさ", &fluvial.softness, 0.001f, 2.0f,
                                     defaults.softness,
                                     "しきい値の前後をどれだけなだらかに繋ぐか", "%.3f");
        changed |= ui::PropertyFloat("川縁", &fluvial.edgePower, 0.1f, 8.0f,
                                     defaults.edgePower,
                                     "1 より大きいと川が細く、小さいと太くなる", "%.2f");
    } else {
        changed |= ui::PropertyFloat("ガンマ", &fluvial.gamma, 0.05f, 4.0f, defaults.gamma,
                                     "下げると細い支流が明るくなり、上げると主流だけが残る",
                                     "%.2f");
    }

    changed |= ui::PropertyFloat(
        "最大ディテール", &fluvial.detailMeters, 1.0f, 512.0f, defaults.detailMeters,
        "流向を読む前にならす大きさ（m）。大きいほど小さな凹凸を無視して大きな谷筋を追う",
        "%.1f m", ImGuiSliderFlags_Logarithmic);
    changed |= ui::PropertyFloat("集中度", &fluvial.concentration, 0.1f, 16.0f,
                                 defaults.concentration,
                                 "下流への配分の集中度。大きいほど主流へ集まり、"
                                 "小さいほど面的に広がる",
                                 "%.2f");

    int resolutionIndex = 1;
    for (int i = 0; i < IM_ARRAYSIZE(kFluvialResolutionValues); ++i) {
        if (kFluvialResolutionValues[i] == fluvial.resolution) {
            resolutionIndex = i;
        }
    }
    if (ui::PropertyCombo("解像度", &resolutionIndex, kFluvialResolutionLabels,
                          IM_ARRAYSIZE(kFluvialResolutionLabels), 1,
                          "川筋を計算するグリッド。**合成解像度とは別**。"
                          "上げるほど細かい支流が出るが、反復回数も比例して増える")) {
        fluvial.resolution = kFluvialResolutionValues[resolutionIndex];
        changed = true;
    }
    return changed;
}

// テクスチャスロットを選ぶ行。RGB をそのまま使うマップ（ベースカラー / 法線）用。
inline bool DrawTextureSlotRow(const char* label, compositor::TextureId& slot,
                        const compositor::TextureLibrary& library) {
    ui::PropertyLabel(label, "「なし」なら定数値を使う");
    const float width =
        std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x);
    const bool changed = DrawTextureCombo("##value", slot, library, width);
    ui::PropertyEnd();
    return changed;
}

// スカラーのマップを選ぶ行。テクスチャに加えて、どのチャンネルを読むかも選ぶ。
// Megascans の _ORD のように 1 枚へ複数のマップを詰めたテクスチャがあるため。
inline bool DrawMapSlotRow(const char* label, compositor::MapSlot& slot,
                    const compositor::TextureLibrary& library) {
    ui::PropertyLabel(label, "「なし」なら定数値を使う。右は読むチャンネル");

    const float channelWidth = ui::Scaled(52.0f);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float available = ImGui::GetContentRegionAvail().x;
    const float comboWidth =
        std::max(ui::Scaled(60.0f),
                 std::min(ui::Scaled(ui::kComboMaxWidth), available) - channelWidth - spacing);

    bool changed = DrawTextureCombo("##texture", slot.texture, library, comboWidth);

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

// ビューポートに重ねる線を描くための投影。カメラの行列をそのまま使う。
struct ProjectedPoint {
    ImVec2 screen{};
    bool visible = false;
};

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


}  // namespace tg
