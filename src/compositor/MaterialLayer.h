#pragma once

#include <DirectXMath.h>

#include <cstdint>
#include <string>

namespace tg::compositor {

// 合成対象のチャンネル。出力テクスチャの構成と対応する。
enum class Channel : uint32_t {
    BaseColor = 0,
    Normal = 1,
    Surface = 2,  // R: Roughness, G: Metallic, B: AO
    Height = 3,
    Count = 4,
};

inline constexpr uint32_t ChannelBit(Channel channel) {
    return 1u << static_cast<uint32_t>(channel);
}

inline constexpr uint32_t kAllChannelBits = 0xFu;

// レイヤーの値のソース。
enum class ValueSource : uint32_t {
    Constant = 0,
    Noise = 1,
    Texture = 2,
};

// レイヤーの種類。**高さの合成規則が種類ごとに違う**（Quixel Mixer の
// Surface / Noise / Liquid に相当）。詳細は docs/design/compositing.md を参照。
//
//   Surface: 高さ + マスクの競合。勝った方が置き換える（従来の動作）。
//   Shape:   下地の高さへの加算。下のレイヤーの細部が保存されるので、
//            地形スケールの大きな形を作れる。BaseColor / Surface は書かない。
//            加算後の高さは 0〜1 へ切り詰める。
//   Liquid:  絶対値の水位。下地が水位より低い所だけ高さを水位へ置き換える。
//            重みは「水位 − 下地の高さ」だけから決めるので、
//            水位を動かしても下地は変形しない。
// Blur   : 合成しない**加工**。下地のハイトをぼかし、法線を作り直す。
//          色もマスクも持たない（下地と競合する相手ではないため）。
enum class LayerKind : uint32_t {
    Surface = 0,
    Shape = 1,
    Liquid = 2,
    Blur = 3,
};

// ハイトの基準面。ソースの値がこの値のとき、そのテクセルは「基準の高さ」ちょうどになる。
//
// ディスプレイスメントマップは「中間グレーが変位ゼロ」という慣習で作られるため、
// マップ自身の平均ではなく 0.5 を固定で使う。goals.md の「業界標準に合わせる」に従う。
// シェーダの kHeightPivot と一致させること。
inline constexpr float kHeightPivot = 0.5f;

// テクスチャの ID。0 は「なし」。TextureLibrary が払い出す。
using TextureId = uint32_t;
inline constexpr TextureId kNoTexture = 0;

// ペイントマスクの ID。0 は「なし」。PaintMaskStore が払い出す。
using PaintMaskId = uint32_t;
inline constexpr PaintMaskId kNoPaintMask = 0;

// マテリアルの ID。0 は「なし」。MaterialLibrary が払い出す。
using MaterialAssetId = uint32_t;
inline constexpr MaterialAssetId kNoMaterialAsset = 0;

// シェーダへ渡す「参照しない」を表すインデックス。
inline constexpr uint32_t kInvalidTextureIndex = 0xFFFFFFFFu;

// テクスチャのどのチャンネルを読むか。シェーダの SelectChannel と一致させること。
//
// Megascans の `_ORD` のように、1 枚のテクスチャへ複数のマップを詰めたものがある
// （O = Occlusion / R = Roughness / D = Displacement）。
// スカラーのマップはどれも「テクスチャ + チャンネル」で指定する。
enum class TextureChannel : uint32_t {
    R = 0,
    G = 1,
    B = 2,
    A = 3,
};

// スカラーのマップ 1 つぶん。
struct MapSlot {
    TextureId texture = kNoTexture;
    TextureChannel channel = TextureChannel::R;
};

// チャンネル指定をまとめてシェーダへ渡すための詰め方。4bit ずつ、最大 8 スロット。
// 並びはシェーダの TG_CHANNEL_* と一致させること。
inline constexpr uint32_t PackChannel(TextureChannel channel, uint32_t slotIndex) {
    return static_cast<uint32_t>(channel) << (slotIndex * 4u);
}

// ノイズの種類。シェーダの TG_NOISE_* と一致させること。
enum class NoiseType : uint32_t {
    Fbm = 0,     // 一般的なフラクタルノイズ
    Ridged = 1,  // 尾根状。稜線や割れ目に向く
    Worley = 2,  // セル状。石畳や砂利に向く
};

// フラクタルノイズのパラメータ。ハイトとマスクで共通に使う。
struct NoiseParams {
    NoiseType type = NoiseType::Fbm;
    float scale = 6.0f;    // UV に掛ける周波数
    float amount = 1.0f;   // 出力への寄与
    int octaves = 5;
    float offset = 0.0f;   // 同じレイヤー内で別パターンにしたいときにずらす
};

// マスクのソース。合成の中間結果に由来するものを含む。
//
// 「下地」とは、このレイヤーより下のレイヤーを合成した結果のこと。
// 傾斜や曲率を使うと「急斜面にだけ岩を出す」「窪みにだけ苔を生やす」が書ける。
enum class MaskSource : uint32_t {
    Constant = 0,
    Noise = 1,
    Texture = 2,
    Height = 3,     // 下地の高さ
    Slope = 4,      // 下地の傾斜（0 = 平坦、1 = 急）
    Curvature = 5,  // 下地の曲率（0.5 = 平坦、> 0.5 = 凸、< 0.5 = 凹）
    Cavity = 6,     // 下地の窪み（簡易 AO。1 に近いほど窪んでいる）
    Paint = 7,      // ブラシで描いたマスク（PaintMaskStore が持つテクスチャ）
    Fluvial = 8,    // 下地の川筋（フロー累積。水が集まる所ほど 1 に近い）
};

// 中間結果由来かどうか。真なら評価前にマスク生成パスが要る。
inline bool IsDerivedMaskSource(MaskSource source) {
    return source == MaskSource::Height || source == MaskSource::Slope ||
           source == MaskSource::Curvature || source == MaskSource::Cavity ||
           source == MaskSource::Fluvial;
}

// 川筋マスクの出力カーブ。シェーダの TG_FLUVIAL_CURVE_* と一致させること。
enum class FluvialCurve : uint32_t {
    Log = 0,        // 対数。細い支流から主流まで 1 枚に収まる（既定）
    Threshold = 1,  // しきい値。川とみなす所だけを二値寄りに抜く
    Linear = 2,     // 線形。主流が強く出る
};

// 川筋（フロー累積）マスクのパラメータ。source == MaskSource::Fluvial で使う。
// 既定値は terrain-editor の Mask Fluvial に合わせてある。
struct FluvialParams {
    FluvialCurve curve = FluvialCurve::Log;
    // Log / 線形ではノイズフロア、しきい値では「川とみなす」境目。
    // **全セル数に対する割合**で持つ（解像度を変えても効きが変わらない）。
    float threshold = 0.0f;
    float gamma = 0.5f;        // Log / 線形のカーブ。下げると細い支流が明るくなる
    float softness = 0.15f;    // しきい値の遷移幅
    float edgePower = 1.6f;    // しきい値のときの川縁のテーパー
    // 流向を読む前にならす最大スケール（m）。大きいほど小さな凹凸を無視して
    // 大きな谷筋を優先する。
    float detailMeters = 8.0f;
    // 下流への配分の集中度（MFD の指数）。大きいほど主流へ集まる。
    float concentration = 4.0f;
    // 計算グリッドの一辺。**合成解像度とは別**に持つ。反復回数が解像度に比例して
    // 効くので、川筋の形が決まる粗さだけあればよい。
    uint32_t resolution = 512;
};

// マスク。ソースの値に定数を掛け、カーブ・レベル調整・反転を掛ける。
struct LayerMask {
    MaskSource source = MaskSource::Constant;
    float constant = 1.0f;
    NoiseParams noise{NoiseType::Fbm, 4.0f, 1.0f, 4, 37.0f};
    // 中間結果由来のマスクの強調度。傾斜や曲率の効き方を調整する。
    float derivedScale = 1.0f;
    // 川筋マスクの設定。source が Fluvial のときだけ参照する。
    FluvialParams fluvial;
    // 川筋を**レイヤー列のどこまで合成した Height から作るか**。
    // グラフのコンパイルが Mask Fluvial ノードの入力から決めて入れる。
    // -1 は「このレイヤーの直下」（入力を繋いでいないときの既定）。
    // **保存しない。** 繋ぎ方から毎回決まる値なので、ファイルには残さない。
    int fluvialSourceIndex = -1;
    // カーブ。1 で線形、> 1 で中間を締める（コントラストが上がる）。
    float contrast = 1.0f;
    float levelsLow = 0.0f;
    float levelsHigh = 1.0f;
    bool invert = false;
    // ペイントマスク。source が Paint のときだけ参照する。
    PaintMaskId paint = kNoPaintMask;
    // マスク用テクスチャ。source が Texture のときだけ参照する。
    // マテリアルのマップとは用途が別なので、レイヤー側で持つ。
    MapSlot texture;
};

// 1 レイヤーぶんの設定。
//
// 種類でフィールドの解釈が一部変わる。同じ意味の値を 2 か所に置かないため、
// 新しいフィールドは足さずに読み替える。
//   Shape:  heightBase は全体の持ち上げ（0.5 で変化なし）。
//   Liquid: heightBase が水位（絶対値）、blendRange が汀線のフェザー幅。
struct MaterialLayer {
    std::string name = "Layer";
    bool enabled = true;
    LayerKind kind = LayerKind::Surface;

    // このレイヤーが書き込むチャンネル（Mixer と同じくチャンネル単位で切り替えられる）
    uint32_t channelMask = kAllChannelBits;

    // **リニアで持つ。既定は 18% グレー（0.18）。**
    // 写真と CG で共通の中間グレー基準で、画面上では sRGB 0.46 に見える。
    // 地面素材のアルベド（0.1〜0.3）にも収まる。
    // 0.5 をリニアで置くと sRGB 0.735 相当となり、コンクリートより明るくなる。
    DirectX::XMFLOAT3 baseColor = {0.18f, 0.18f, 0.18f};
    float roughness = 0.5f;
    float metallic = 0.0f;
    float ambientOcclusion = 1.0f;

    // ハイト。基準の高さに、ソースの値を kHeightPivot 基準で振れさせたぶんを足す。
    //
    //   定数          : h = heightBase
    //   ノイズ / 画像 : h = heightBase + (src - kHeightPivot) * heightGain
    //
    // heightGain を変えても平均の高さは heightBase のまま動かないので、
    // 「どこに座るか」と「どれだけ起伏するか」を独立に決められる。
    // ハイトでは NoiseParams::amount を使わない（heightGain がその役目を担う）。
    // 既定値は「起伏 1.0 で 0〜1 に収まり、平均が基準面に乗る」ように選んである。
    // 追加したてのレイヤーが既存のレイヤーより極端に低く沈まないようにするため。
    ValueSource heightSource = ValueSource::Noise;
    float heightBase = 0.5f;
    float heightGain = 1.0f;
    NoiseParams heightNoise{NoiseType::Fbm, 6.0f, 1.0f, 5, 0.0f};

    // レイヤー直結のハイトマップ。**マテリアルを持たないレイヤー（シェイプ）用。**
    // サーフェスのハイトはマテリアルのハイトマップから引く（同じ意味の値を
    // 2 か所に置かない）。マテリアルがあるときは参照しない。
    MapSlot heightTexture;

    // ハイトのぼかし（kind == LayerKind::Blur のときだけ意味を持つ）。
    // 半径は**メートル**。合成解像度を変えても効きが変わらないようにするため、
    // テクセルではなく実寸で持つ（地形の一辺と解像度からテクセル数へ直す）。
    struct BlurSettings {
        float radiusMeters = 3.0f;
        float strength = 1.0f;
        int iterations = 1;
    };
    BlurSettings blur;

    LayerMask mask;

    // このレイヤーが使うマテリアル（PBR のマップ一式）。
    // kNoMaterialAsset なら下の定数値だけで塗る。
    MaterialAssetId material = kNoMaterialAsset;

    // ハイトブレンドの境界の柔らかさ。0 に近いほど硬い置き換えになる。
    float blendRange = 0.15f;

    // 下地に沿わせる（Mixer の Wrap to Underlying。サーフェスのみ）。
    // オンにすると自分の高さを「下地 + 相対的な起伏」と読み替えてから競合するので、
    // 勝った所でも下地の大きな形が保たれ、自分の細部だけが上へ重なる。
    // 「山の上に雪を被せる」のようなコーティングに使う。
    // 基準の高さは厚みのバイアスになる（0.5 で厚みなし）。
    bool wrapToUnderlying = false;

    // このレイヤーの UV スケール。
    float uvScale = 1.0f;
};

}  // namespace tg::compositor
