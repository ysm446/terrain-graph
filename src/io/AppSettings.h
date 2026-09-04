#pragma once

#include <filesystem>

namespace tg::io {

// アプリ側の状態を置くフォルダ（`%LOCALAPPDATA%/terrain-graph`）。
// プロジェクトの中身ではないもの（設定、最近使ったファイル）はここへ置く。
// 環境変数が引けないときは作業ディレクトリを返す。
std::filesystem::path AppDataDirectory();

// UI の見た目に関する設定。プロジェクトではなくアプリに紐づく。
struct UiSettings {
    // Windows の表示スケール（DPI）に合わせて UI を拡大するか。
    //
    // 既定は**合わせない**。クライアント領域を実ピクセルで固定しているので、
    // 追従させると作業面積がモニタ設定によって変わる（design-guide.md の「寸法と DPI」）。
    // 高 DPI で UI が小さすぎるときに、使う人が選べるようにしてある。
    bool followSystemScale = false;
    // 追従しないときの拡大率。
    float manualScale = 1.0f;
    // 文字の基準サイズ（px、拡大率を掛ける前）。UI 全体に効く。
    // 拡大率と違って余白や部品幅は動かないので、**文字だけを詰めたい / 大きくしたい**
    // ときに使う。既定と範囲は `ui/UiStyle.h` の kDefaultFontSize / kMinFontSize / kMaxFontSize と揃える。
    int fontSize = 17;
    // レイヤーパネルの一覧側（上の区画）の高さ（96 DPI 基準）。
    // 境界のドラッグで変わる。**拡大率を掛ける前の値で持つ**ので、
    // 表示スケールを変えても区画の見た目の高さが保たれる。
    float layerListHeight = 260.0f;
};

// 表示に関する設定（設定ウィンドウの「表示」節）。
// design-guide の「設定ウィンドウ」が settings.json に置くと定めているもの。
struct DisplaySettings {
    bool vsync = true;
    bool hotReload = true;
    // ビューポートの右上に FPS を出すか。
    bool showFps = false;
    // ビューポートの右上に描画の量（ドローコール・頂点・三角形）を出すか。
    bool showStats = false;
    // ハイトの範囲（height 0 / 0.5 / 1 の位置を示す枠）をビューポートに重ねるか。
    bool showHeightGuide = false;
    // 前面にあるときの FPS 上限。0 で上限なし。
    int frameRateLimit = 0;
    // **背面にあるときの FPS 上限。** 見えていない絵に GPU を回し続けないため、
    // 既定で低く抑える。0 で上限なし。
    int inactiveFrameRateLimit = 10;
    float clearColor[4] = {0.09f, 0.09f, 0.11f, 1.0f};
};

// 設定ファイル（`settings.json`）の読み書き。
//
// 値を変えたら Save() を呼ぶ。書けなくても動作は続ける（次回に持ち越せないだけ）。
class AppSettings {
public:
    void Load();
    bool Save() const;

    UiSettings& Ui() { return m_ui; }
    const UiSettings& Ui() const { return m_ui; }

    DisplaySettings& Display() { return m_display; }
    const DisplaySettings& Display() const { return m_display; }

private:
    UiSettings m_ui;
    DisplaySettings m_display;
};

}  // namespace tg::io
