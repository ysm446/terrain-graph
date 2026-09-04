#pragma once

// マスクのノードグラフを、評価器が焼ける形（op の並び）へ落としたもの。
//
// グラフ（`graph::NodeGraph`）はマスクのノードを DAG で持てるが、評価器は
// 「入力が先、出力が後」に並んだ**演算の列**だけを見る。分岐と合流は
// op の添字で表す（同じ op を複数の消費者が参照してよい）。
//
// **結果は op ごとに 1 枚のテクスチャへ焼く。** 中間結果を持つので、
// Mask Blend のような合流も、同じマスクを 2 か所で使うこともできる。
//
// UI / D3D12 には依存しない。

#include "compositor/MaterialLayer.h"

#include <cstdint>
#include <vector>

namespace tg::compositor {

// マスクのプレビューで貼る塗りの明るさ。マスク 0 と 1 に対応する。
// グラフがこの色で塗り、**描画側（飽和した所の斜線）が同じ値で読み戻す**ので、
// 1 か所にまとめてある。純黒 / 純白にしないのは、PBR で照らしたときに
// 潰れて濃淡が読めなくなるため。
inline constexpr float kMaskPreviewLow = 0.02f;
inline constexpr float kMaskPreviewHigh = 0.85f;

enum class MaskOpKind : uint32_t {
    Image = 0,    // 画像 1 枚（+ 読むチャンネル）
    Fluvial = 1,  // 下地の川筋（フロー累積）
    Slope = 2,    // 下地の傾斜（角度の範囲を 0〜1 へ）
    Levels = 3,   // 黒点 / 白点 / ガンマ / 反転
    Blend = 4,    // 2 枚の合成
    // 堆積レイヤーが積もらせた**土砂の厚み**。厚い所ほど 1 に近い。
    // 堆積した所へ別のマテリアルを乗せるためのもの。
    Sediment = 5,
    // 崩落レイヤーが積んだ**岩屑**。出力ピンによって厚みか、岩片ごとの乱数になる。
    Crumbling = 6,
    // ノイズ 1 枚。入力を持たないマスクのソース。
    Noise = 7,
    // 下地の曲率（周りより高い / 低い）。
    Curvature = 8,
    // 積雪レイヤーが積もらせた**雪の被覆**。積雪厚をしきい値とぼかしで 0〜1 にする。
    Snow = 9,
    // 下地の標高帯（この高さからこの高さまで）。
    Height = 10,
    // 河川レイヤーの出力。出力ピンによって水面の被覆 / 河原 / 水深になる。
    River = 11,
    // パス（向き付きの線）の足跡。中心線からの距離を幅とフェザーで 0〜1 にする。
    Path = 12,
};

// 曲率マスクの向き。シェーダの TG_CURVATURE_* と一致させること。
enum class CurvatureMode : uint32_t {
    Ridges = 0,    // 周りより高い所（尾根・出っ張り）
    Valleys = 1,   // 周りより低い所（谷・窪み）
    Absolute = 2,  // どちらも
};

// 曲率マスク。**周りの平均との高さの差**を見る。ラプラシアンではないので、
// 「どのくらいの広さの周りと比べるか」を実寸で指定できる。
struct CurvatureParams {
    CurvatureMode mode = CurvatureMode::Absolute;
    // 比べる周りの広さ（m）。大きいほど小さな凹凸を無視して広い尾根や谷を拾う。
    float detailMeters = 8.0f;
    // この高さの差で 1 になる（m）。小さいほど弱い曲率まで明るくなる。
    float sensitivityMeters = 1.0f;
    float threshold = 0.0f;  // これ以下を捨てる
    float gamma = 1.0f;
};

// シェーダの TG_BLEND_* と一致させること。
enum class MaskBlendMode : uint32_t {
    Add = 0,
    Multiply = 1,
    Min = 2,
    Max = 3,
};

// 標高マスク。**メートルで指定する。** ハイト 0〜1 の全幅が標高差なので、
// 0 m は地形の一番低い所（ハイト 0）、標高差 m が一番高い所（ハイト 1）に当たる。
struct HeightParams {
    // 地形の**実際の**最低 / 最高で 0〜1 へ正規化する。下の 3 つは使わない。
    bool useFullRange = false;
    float minMeters = 0.0f;      // これより低い所を 0 にする
    float maxMeters = 1000.0f;   // これより高い所を 0 にする
    float featherMeters = 0.0f;  // 標高帯の上下の境目をぼかす幅
    float gamma = 1.0f;
    bool invert = false;
};

// 傾斜マスク。**角度（度）で指定する。** 実寸で地形を扱うので、
// 「45 度より急な所」のように意味の分かる単位で持てる。
struct SlopeParams {
    // 傾斜を読む前にならす大きさ（m）。0 ならならさない。
    float detailMeters = 0.0f;
    float minDegrees = 25.0f;  // これ以下を黒に
    float maxDegrees = 60.0f;  // これ以上を白に
    float gamma = 1.0f;
    bool invert = false;
};

// レベル調整。黒点以下を 0、白点以上を 1 にし、間をガンマで曲げる。
struct LevelsParams {
    float blackPoint = 0.0f;
    float whitePoint = 1.0f;
    float gamma = 1.0f;
    bool invert = false;
};

// 2 枚の合成。強さは「前景」と「合成結果」の間の補間。
struct BlendParams {
    MaskBlendMode mode = MaskBlendMode::Multiply;
    float intensity = 1.0f;
};

// 堆積の厚みをマスクにするときの調整。**正規化は実寸**で、
// 基準の厚み（m）で 1 になる。0 のときだけ一番厚い所で 1 に落とす。
struct SedimentMaskParams {
    // 0 で線形、上げるほど「積もった / 積もっていない」がはっきりする。
    float contrast = 0.0f;
    // マスクが 1 になる厚み（m）。0 なら一番厚い所で正規化する。
    float thicknessMeters = 0.5f;
};

// 積雪の被覆をマスクにするときの調整。**しきい値もぼかしも実寸（m）**で、
// 厚みがしきい値を超えた所が白、その前後をぼかし幅ぶんだけなめらかに繋ぐ。
struct SnowMaskParams {
    float thresholdMeters = 0.02f;
    float featherMeters = 0.015f;
};

// 崩落の出力をマスクにするときの選択。**どの出力ピンから来たか**で決まる。
struct CrumblingMaskParams {
    // 0: 岩屑の厚み（形そのもの）、1: 岩片ごとの乱数（色や材質のばらつき用）
    uint32_t channel = 0;
};

// 河川の出力をマスクにするときの選択。**どの出力ピンから来たか**で決まる。
struct RiverMaskParams {
    // 0: 水面の被覆（Water）、1: 河原（Bank）、2: 水深（Depth）
    uint32_t channel = 0;
    // 河原（Bank）の形。レイヤーの RiverSettings から写す（積雪のしきい値と同じ扱い）。
    float shoreWidthMeters = 15.0f;
    float shoreHeightMeters = 2.0f;
    float shoreFeather = 0.3f;
    // 河原の広がりを流量で縮める基準（主流の幅）。
    float mainWidthMeters = 60.0f;
    float minWidthMeters = 3.0f;
};

// パスの線分 1 本。座標は地形平面の正規化 UV（0〜1）、幅 / フェザーは m。
// 両端で値が違えばエッジ上で補間する。孤立した点は a == b の線分（円）。
struct PathSegment {
    float ax = 0.0f;
    float ay = 0.0f;
    float bx = 0.0f;
    float by = 0.0f;
    float widthA = 0.0f;
    float widthB = 0.0f;
    float featherA = 0.0f;
    float featherB = 0.0f;
    float intensityA = 1.0f;
    float intensityB = 1.0f;
};

// パスをマスクにするときの調整。形（幅 / フェザー / 強さ）は点が持つ。
struct PathMaskParams {
    float gamma = 1.0f;
    bool invert = false;
};

// 演算 1 つ。入力は他の op の添字で、**自分より前**を指す（後方参照はしない）。
struct MaskOp {
    MaskOpKind kind = MaskOpKind::Image;
    int inputA = -1;  // Levels の入力 / Blend の前景
    int inputB = -1;  // Blend の背景
    // 高さ由来（Fluvial / Slope）が読む Height。**レイヤー列の添字**で、
    // 「そこまで合成し終えた Height」を使う。コンパイルが決める。
    int heightSourceLayer = -1;

    MapSlot map;
    FluvialParams fluvial;
    HeightParams height;
    SlopeParams slope;
    CurvatureParams curvature;
    LevelsParams levels;
    BlendParams blend;
    NoiseParams noise;
    SedimentMaskParams sedimentMask;
    CrumblingMaskParams crumblingMask;
    SnowMaskParams snowMask;
    RiverMaskParams riverMask;
    PathMaskParams pathMask;
    // Path のときだけ。コンパイルがパスから作る線分列（正規化 UV）。
    std::vector<PathSegment> pathSegments;
};

// 評価する順に並んだ op の列。
using MaskProgram = std::vector<MaskOp>;

}  // namespace tg::compositor
