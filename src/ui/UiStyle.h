#pragma once

#include <imgui.h>

#include <cstddef>

// UI の見た目とプロパティ行の共通部品。
//
// **UI を変更するときは docs/design/design-guide.md に従うこと。**
// 個々のパネルが ImGui のウィジェットを直接呼ぶのではなく、
// ここのヘルパーを通すことで、ラベルの体裁・幅・既定値・ツールチップが揃う。
namespace tg::ui {

// 部品の寸法。96 DPI 基準の値を置き、使うときに Scaled() で現在の DPI へ合わせる。
// 値の意味と使い分けは design-guide.md にある。種類を勝手に増やさない。
inline constexpr float kLabelColumnWidth = 108.0f;
inline constexpr float kSliderMinWidth = 76.0f;
inline constexpr float kSliderMaxWidth = 176.0f;
inline constexpr float kComboMaxWidth = 190.0f;
inline constexpr float kButtonWidth = 68.0f;
inline constexpr float kWideButtonWidth = 148.0f;

// 文字の基準サイズ（px、拡大率を掛ける前）の既定値と、設定で選べる範囲。
// **これが寸法の基準**で、下の kCaptionFontSize や TextScaled() はこの値を 1.0 とみなす。
// 小さすぎると日本語が潰れ、大きすぎるとパネルに収まらないので上下を切る。
inline constexpr float kDefaultFontSize = 17.0f;
inline constexpr float kMinFontSize = 11.0f;
inline constexpr float kMaxFontSize = 28.0f;

// 一覧のサムネイルに添える名前の文字サイズ（基準は 17）。
// **これ以外の場所で文字サイズを変えない。**
// 本文よりはっきり小さくして、サムネイルの添え物だと分かるようにする。
// 素材名は長い（`T_Rocky_Soil_..._D.EXR`）ので、小さいほど省略が減る。
inline constexpr float kCaptionFontSize = 12.0f;

// グラフのノードに出すサムネイル（マスクの結果 / マテリアル）の一辺。
// レイヤー一覧の 40 では模様が読めず、一覧の 84 ではノードが縦に伸びすぎる。
inline constexpr float kNodeThumbnail = 64.0f;

// 列の境界を掴める幅（96 DPI 基準）。線そのものより広く取らないと狙って掴めない。
inline constexpr float kSplitterGrabWidth = 8.0f;
// 境界の線と、その左右の列との間に空ける幅。
// **ここを詰めると、一覧の枠が境界に貼り付いて窮屈に見える。**
inline constexpr float kSplitterMargin = 8.0f;
inline constexpr float kTextInputWidth = 190.0f;

// グレー基調のテーマを適用する。ImGui のコンテキストを作った直後に 1 回だけ呼ぶ。
void ApplyTheme(float dpiScale);

// 96 DPI 基準の寸法を現在の DPI へ合わせる。
float Scaled(float value);

// **文字が入る寸法**（ラベル列の幅、スライダーやボタンの幅）に使う。
// DPI に加えて文字サイズの倍率も掛かるので、文字を大きくしても
// ラベルが切れたりボタンから文字がはみ出したりしない。
// 余白・線幅・サムネイルのような、文字と関係ない寸法には使わない（Scaled を使う）。
float TextScaled(float value);

// 文字サイズの倍率（kDefaultFontSize を 1.0 とする）。
// 文字サイズに追従させたい寸法を自分で組み立てるときに使う。
float FontScale();

// --- プロパティ行 ---------------------------------------------------------
//
// すべての設定値は「ラベル：ウィジェット」の 2 列テーブルの 1 行として描く。
// 左列にラベル（末尾に全角コロン）、右列にウィジェットを置く。

bool BeginPropertyTable(const char* id);
void EndPropertyTable();

// セクション見出し。プロパティテーブルの外で呼ぶ。
void SectionHeader(const char* label);

// 行を開き、ラベル列を描いて値列へ移動する。既製の行で表せないウィジェットを
// 自分で置きたいときに使う。ID を積むので、行の終わりで PropertyEnd() を呼ぶこと。
void PropertyLabel(const char* label, const char* tooltip = nullptr);
// ラベルを置かない行。ボタンだけを値列に並べたいときに使う。id は行ごとに固有にする。
void PropertyLabelEmpty(const char* id);
void PropertyEnd();

// snapStep を渡すと、**ドラッグ中だけ**その刻みへ吸着する。
// 「UV スケールを 2.00 にしたい」のように、きりのいい値を狙う行で使う。
// Ctrl + クリックの直接入力は丸めない（狙って入れた値を動かさないため）。
bool PropertyFloat(const char* label, float* value, float minValue, float maxValue,
                   float defaultValue, const char* tooltip = nullptr,
                   const char* format = "%.3f", ImGuiSliderFlags flags = 0,
                   float snapStep = 0.0f);
bool PropertyInt(const char* label, int* value, int minValue, int maxValue, int defaultValue,
                 const char* tooltip = nullptr);
bool PropertyBool(const char* label, bool* value, bool defaultValue,
                  const char* tooltip = nullptr);
// **表示色（sRGB）をそのまま持つ値**の行。背景色のように、
// 画面へ出す値をそのまま格納しているものに使う。
bool PropertyColor(const char* label, float* rgb, const float* defaultRgb,
                   const char* tooltip = nullptr);
// **リニアで持つ色**の行。描画に使う色（アルベド、ライト、空）はこちら。
// 値はリニアのまま読み書きし、ピッカーには sRGB へ直して見せる。
// そうしないと、スウォッチの見た目と描画結果が食い違う
// （docs/design/design-guide.md の「色空間」を参照）。
bool PropertyColorLinear(const char* label, float* linearRgb, const float* defaultLinearRgb,
                         const char* tooltip = nullptr);
// items は要素数 itemCount の配列。ImGui の "A\0B\0" 形式ではなく配列で受ける。
bool PropertyCombo(const char* label, int* value, const char* const items[], int itemCount,
                   int defaultValue, const char* tooltip = nullptr);
bool PropertyTextInput(const char* label, char* buffer, size_t bufferSize,
                       const char* tooltip = nullptr);
// 表示専用の値。
void PropertyValue(const char* label, const char* format, ...);

// 値列いっぱいに広げないボタン。幅は kButtonWidth / kWideButtonWidth のどちらか。
bool Button(const char* label, float width = kButtonWidth);

// --- サムネイル一覧 -------------------------------------------------------

// サムネイル 1 枚ぶんの選択枠。**画像を描いた後に呼ぶこと。**
//
// 選択は「背景を敷く」では表せない。サムネイルが不透明だと下の色が完全に隠れる。
// 画像の上に枠を重ねて描く。
//
// 呼ぶ前に ImGui::Image / Button を置き、その矩形（GetItemRectMin / Max）を渡す。
void ThumbnailFrame(const ImVec2& min, const ImVec2& max, bool selected, bool hovered);

struct Thumbnail {
    bool clicked = false;
    bool hovered = false;
    // ダブルクリック。**1 回目のクリックは clicked にも立つ**ので、
    // 「選ぶ」と「開く」を両方やりたい側はそのまま両方見ればよい。
    bool doubleClicked = false;
};

// サムネイル 1 枚。**ドラッグ元にできる形で置く。**
//
// `ImGui::Image()` は ID を持たないアイテムなので、そのままでは
// `BeginDragDropSource()` がドラッグを開始できず、黙って false を返す。
// ここでは `InvisibleButton` で ID を確保してから画像を描く。
//
// 戻った直後に `BeginDragDropSource()` を置いてよい
// （枠の描画は最後のアイテムを変えない）。
Thumbnail ThumbnailButton(const char* id, ImTextureID texture, float size, bool selected);

// 一覧の行に置く小さなサムネイル。**選択枠は付けない。**
// 行そのものが選択を示すので、画像側にも枠を出すと選択が二重に見える。
// texture の ptr が 0 なら中身の代わりに枠だけを描く。
void ThumbnailImage(ImTextureID texture, float size);

// 一覧の行に置く単色のサムネイル。テクスチャを持たないもの
// （マテリアルを割り当てていないレイヤーなど）の代わりに使う。
void ColorSwatch(const ImVec4& color, float size);

// 一覧の行に置くレイヤー種類のアイコン。マテリアルのサムネイルが意味を
// 持たないレイヤーの代わりに使う。目のアイコンと同じく、字形が無いので
// 図形で描く（design-guide.md の「記号を使うとき」を参照）。
void MountainIcon(float size);  // シェイプ（高さへの加算）: 山の稜線
void WavesIcon(float size);     // 水面: 横に走る 2 本の波

// --- 表示・非表示の目印 ---------------------------------------------------

// 目のアイコンでオン / オフを切り替える。値が変わったら true。
//
// **字形ではなく図形で描く。** フォントは日本語が出せるシステムフォントを
// 読むだけで、目のような記号の字形を持っていない（design-guide.md の
// 「記号を使うとき」を参照）。
bool EyeToggle(const char* id, bool* value, float size);

// 通知の意味色。ステータスバーで警告とエラーを区別するためだけに使う。
// グレー基調を崩さないよう彩度は低く抑えてある。配色の一部なので UiStyle.cpp に置く。
ImU32 WarnColor();
ImU32 ErrorColor();

// 補助テキスト。操作の説明や単位の目安を 1 行で添えるときに使う。
void HintText(const char* format, ...);

// 上下に積んだ 2 つの区画の境界。ドラッグで上の区画の高さを変える。
//
// height は**実ピクセル**（`Scaled()` を通した後の値）で受け渡しする。
// width には区画の幅を渡す。`GetContentRegionAvail()` に頼ると、
// 直前の子ウィンドウの都合で幅が変わってしまう。
//
// 戻り値は「掴んでいた手を離したフレーム」で true。
// 設定の保存のように、ドラッグ中に毎フレームやりたくない処理をここへ吊るす。
bool HorizontalSplitter(const char* id, float* height, float minHeight, float maxHeight,
                        float width);

// 一覧のサムネイルの下に置く名前。**幅はサムネイルに合わせて渡すこと。**
// **常に 2 行**で描く（行数が変わると升目の高さが揃わない）。
// 収まらないぶんは中央を省略し、先頭と末尾の両方を残す。
// 全体はホバーのツールチップで読める。
void GridCaption(const char* text, float width);

}  // namespace tg::ui
