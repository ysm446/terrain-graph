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
// Blur     : 合成しない**加工**。下地のハイトをぼかし、法線を作り直す。
//            色もマスクも持たない（下地と競合する相手ではないため）。
// Sediment : 同じく加工。重力で土砂を再分配し、谷に厚く積もらせる。
enum class LayerKind : uint32_t {
    Surface = 0,
    Shape = 1,
    Liquid = 2,
    Blur = 3,
    Sediment = 4,
    // 崩落。発生源から岩屑を斜面下へ流し、止まった所へ積む。
    Crumbling = 5,
    // 積雪。雪を降らせ、急な雪面から低い所へ滑らせて溜める。
    Snow = 6,
    // 河川。川筋から河床を掘り、下流へ単調に下がる水面を張る。
    River = 7,
    // 水滴侵食。水滴を落として斜面を下らせ、削って運んで積む（terrain-editor の Droplet Erosion）。
    Droplet = 8,
    // 散布。単純な形（半球 / 円錐）をばら撒き、分布のマスクを出す
    // （terrain-editor の Scatter）。
    Scatter = 9,
};

// 散布する形。terrain-editor の ScatterShapeType と同じ。
// シェーダの TG_SCATTER_SHAPE_* と一致させること。
enum class ScatterShape : uint32_t {
    Hemisphere = 0,  // 丸い低木や樹冠の分布確認向き
    Cone = 1,        // 尖った草や針葉樹の簡易 proxy 向き
};

// 地形の傾きに対する散布形状の扱い。terrain-editor の RockOrientationRule と同じ。
// シェーダの TG_SCATTER_ORIENT_* と一致させること。
enum class ScatterOrientation : uint32_t {
    Flat = 0,          // 傾きを見ない
    FollowGround = 1,  // 斜面に沿わせ、法線の上向き成分で高さを落とす
    SlopeOriented = 2, // 個体の向きを斜面の向きへ寄せる
};

// 岩片の形。terrain-editor の RockStyle と同じ 3 種類。
enum class RockStyle : uint32_t {
    Classic = 0,    // 丸いドーム
    Polygonal = 1,  // 6 角の多面体
    Shard = 2,      // 4 角で尖った破片
};

// 合成せずハイトを書き換える加工か。**下地にはなれない**（ならす相手が要る）。
inline bool IsHeightOperationKind(LayerKind kind) {
    return kind == LayerKind::Blur || kind == LayerKind::Sediment ||
           kind == LayerKind::Crumbling || kind == LayerKind::Snow ||
           kind == LayerKind::River || kind == LayerKind::Droplet ||
           kind == LayerKind::Scatter;
}

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
// **並びを変えないこと。** プロジェクトには名前で保存するが、シェーダへは
// 数値で渡すので、シェーダの TG_NOISE_* と一致している必要がある。
enum class NoiseType : uint32_t {
    Fbm = 0,     // 一般的なフラクタルノイズ（値ノイズ）
    Ridged = 1,  // 尾根状。稜線や割れ目に向く
    Worley = 2,  // セル状。石畳や砂利に向く
    // 勾配ノイズ（Perlin）。値ノイズより滑らかで、方向のあるうねりになる。
    Perlin = 3,
    // 雲状（billow）。勾配ノイズの絶対値。丸い塊が寄り集まった見た目。
    Billow = 4,
    // 割れ目（Worley の F2 − F1）。セルの境目が明るくなる。
    Cracks = 5,
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
    // マスクのノードグラフの結果（`MaskProgram` の op）。`maskOp` がどれかを指す。
    // 中間結果由来と同じく、評価前にマスクを焼くパスが要る。
    Node = 8,
};

// 中間結果由来かどうか。真なら評価前にマスク生成パスが要る。
inline bool IsDerivedMaskSource(MaskSource source) {
    return source == MaskSource::Height || source == MaskSource::Slope ||
           source == MaskSource::Curvature || source == MaskSource::Cavity;
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
    // マスクのノードグラフの結果を使うときの op の添字（source == Node）。
    // **保存しない。** グラフの繋ぎ方からコンパイルのたびに決まる。
    int maskOp = -1;
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

    // 堆積（kind == LayerKind::Sediment のときだけ意味を持つ）。
    //
    // terrain-editor の Sediment を移したもの。**可動な土砂**を重力で再分配し、
    // 安息角を超えた斜面から低い隣へ滑らせる。谷底に厚く積もり、尾根が痩せる。
    struct SedimentSettings {
        // 上乗せする土砂の厚み（m）。「地形を土砂にする」が入のときは追加ぶん。
        float emissionMeters = 0.5f;
        // 供給を何割の反復にかけて積むか。0 なら最初の 1 反復で全量。
        float emissionTime = 0.0f;
        // 1 反復あたりの沈降距離（m）。大きいほど広い盆地が早く落ち着くが重い。
        float detailMeters = 8.0f;
        int iterations = 40;      // 外側の緩和反復
        int stabilization = 2;    // 1 反復のなかで滑らせる回数
        // 流動性。0 で完全流体（水平に均される）、1 で 80 度まで粘る。
        // 角度は viscosity^2 * 80 度（terrain-editor と同じ曲線）。
        float viscosity = 0.2f;
        // **入力の地形そのものを可動な土砂として扱う。** 切ると入力は動かない
        // 基盤になり、供給量で足したぶんだけが流れる。
        bool convertTerrain = true;
        // 計算グリッド。合成解像度とは別に持つ（反復回数がそのまま効くため）。
        uint32_t resolution = 512;
        // Mask 出力（土砂の厚み）のコントラスト。0 で線形。
        float maskContrast = 0.0f;
        // **Mask が 1 になる厚み（m）。** 実寸で正規化する。
        //
        // 0 にすると「一番厚い所で 1」になる（昔の挙動）。それだと少数の
        // 分厚い点が基準になって残りが 0 付近へ潰れ、マスクとして使えない
        // （実測で 0.5 以上の面積が 0.29% しかなかった）。
        float maskThicknessMeters = 0.5f;
    };
    SedimentSettings sediment;

    // ハイトのぼかし（kind == LayerKind::Blur のときだけ意味を持つ）。
    // 半径は**メートル**。合成解像度を変えても効きが変わらないようにするため、
    // テクセルではなく実寸で持つ（地形の一辺と解像度からテクセル数へ直す）。
    struct BlurSettings {
        float radiusMeters = 3.0f;
        float strength = 1.0f;
        int iterations = 1;
    };
    BlurSettings blur;

    // 崩落（kind == LayerKind::Crumbling のときだけ意味を持つ）。
    //
    // terrain-editor の Crumbling を移したもの。発生源のマスクが明るい所から
    // 岩片を生み、地形の低い方へ歩かせ、止まった位置へ岩片の形を積む。
    // サイズは m で持つ（実寸で地形を扱うため）。
    struct CrumblingSettings {
        int physicsCount = 48;         // 岩片を下へ進めるステップ数
        float amount = 0.65f;          // 岩屑の量。粒子数と盛り上がりの強さに効く
        float sizeMinMeters = 2.0f;    // 岩片の最小直径
        float sizeMaxMeters = 8.0f;    // 岩片の最大直径
        RockStyle style = RockStyle::Shard;
        float gravity = 0.75f;         // 低い方へ流れる強さ。高いほど直線的に下る
        float spread = 0.35f;          // 進行方向から横へ逸れる強さ
        int seed = 0;
    };
    CrumblingSettings crumbling;

    // 積雪（kind == LayerKind::Snow のときだけ意味を持つ）。
    //
    // terrain-editor の Snow を移したもの。雪を一様に降らせ、**雪面**
    // （下地 + 積雪厚）が安息角より急な所から、**一番急な下り 1 方向**へ
    // 滑らせる。谷・棚・緩い尾根に溜まり、急な岩肌には残らない。
    //
    // 堆積との違いは行き先の数（土砂は 4 近傍へ配り、雪は 1 方向へ寄る）と、
    // 下地を削らないこと（雪は必ず上に乗る）。
    struct SnowSettings {
        // 降らせる雪の総量（m）。0 なら入力をそのまま通す。
        float emissionMeters = 1.0f;
        // 供給を何割の段にかけて降らせるか。0 なら最初の 1 段で全量。
        float emissionTime = 0.0f;
        int iterations = 40;      // シミュレーションの段数
        int settlingPasses = 4;   // 1 段のなかで滑らせる回数
        // 雪が動き出す角度。**雪面の角度**で、地形の角度ではない。
        float motionSlopeDegrees = 35.0f;
        // 1 回の滑らせで動く割合。高いほど急斜面から早く逃げる。
        float transportRate = 0.45f;
        // 積もった雪面だけをならす強さ。0 で切る。
        float surfaceSmoothing = 0.25f;
        // 雪が移動先を探す最大スケール（m）。ならしの半径にも使う。
        float detailMeters = 8.0f;
        // 計算グリッド。合成解像度とは別に持つ（段数がそのまま効くため）。
        uint32_t resolution = 512;
        // **Mask 出力がこの厚みで白へ寄る（m）。**
        float maskThresholdMeters = 0.02f;
        // 積雪境界のグレーの幅（m）。小さいほど二値に近くなる。
        float maskFeatherMeters = 0.015f;
    };
    SnowSettings snow;

    // 河川（kind == LayerKind::River のときだけ意味を持つ）。
    //
    // 川筋（フロー累積）から幅を決めて河床を掘り、下流へ単調に下がる水面を張る。
    // 設計は docs/reference/river-node.md。水面は Planchon–Darboux 法の窪み埋め
    // （最小勾配 ε 付き）そのもので、盆地は自然に湖になる。
    // 単位は**すべて実寸（m）**で、評価器が正規化ハイト / セル数へ直す。
    struct RiverSettings {
        // --- 川筋 ---
        // 川とみなす流量。**全セル数に対する割合**（Mask Fluvial と同じ単位）。
        float threshold = 0.002f;
        // 流向を読む前にならす大きさ（m）。
        float detailMeters = 8.0f;
        // 下流への配分の集中度（MFD の指数）。
        float concentration = 4.0f;
        // 川筋を計算するグリッド。**合成解像度とは別。**
        uint32_t resolution = 512;
        // --- 掘る ---
        // **流量が最大のセル**での川幅（m）。幅の基準はここ。
        float mainWidthMeters = 60.0f;
        // しきい値ぎりぎりの細い沢でも、これより細くしない（m）。
        float minWidthMeters = 3.0f;
        // 流量に対する幅の指数。0.5 が水理幾何の標準、0 で一定幅。
        float widthExponent = 0.5f;
        // 水面から河床まで（m）。
        float bedDepthMeters = 3.0f;
        // 水際から岸の上端までの距離（m）。**掘る形**の話。
        float bankWidthMeters = 8.0f;
        // 0 でなだらかな土手、1 で切り立った崖。
        float bankHardness = 0.35f;
        // --- 水面 ---
        // 切ると乾いた河床のまま残す（涸れ川・旧河道）。マスクは出る。
        bool fillWater = true;
        // 水面が下流へ下がる最小の傾き（無次元。0.001 = 1 km で 1 m）。
        // 大きいほど平坦な谷底が上流側から水に浸かる。流向計算には 0 でも下限が入る。
        float minSlope = 0.0002f;
        // --- 河原（Bank マスク） ---
        // 水際から外側へ、河原とみなす距離（m）。主流での値で、支流は幅と同じ比で縮む。
        float shoreWidthMeters = 15.0f;
        // 水面からこの高さまでを河原とする（m）。増水時に浸かる帯。
        float shoreHeightMeters = 2.0f;
        // 縁のなだらかさ。広がりと比高それぞれに対する割合（0〜1）。
        float shoreFeather = 0.3f;
    };
    RiverSettings river;

    // 水滴侵食（kind == LayerKind::Droplet のときだけ意味を持つ）。
    //
    // terrain-editor の Droplet Erosion（GPU 版）を移したもの。容量ベースの粒子侵食で、
    // 水滴を落として慣性つきで斜面を下らせ、運べる量より少なければ削り、多ければ積む。
    // **合成解像度とは別の解析グリッド**で回し、差分を Height へ足し戻す。
    // 距離は m、密度は解析グリッドのセルあたりで持つので、結果は合成解像度に依らない。
    struct DropletSettings {
        // 水滴の数。解析グリッドの**セルあたり**（絶対数ではない）。
        float dropletDensity = 0.25f;
        // 1 滴が進む距離（m）。セルの大きさで歩数に直す。
        float travelMeters = 512.0f;
        float erosionStrength = 0.30f;     // 1 歩で削る割合
        float depositionStrength = 0.30f;  // 容量を超えた土砂を捨てる割合
        float inertia = 0.05f;             // 0 で傾斜どおり、1 で前の向きを保つ
        float minSlope = 0.01f;            // 平坦でも運べるようにする傾斜の下限
        float sedimentCapacity = 4.0f;     // 運べる量の係数
        float evaporationPerMeter = 0.002f;  // 1 m 進むごとに失う水の割合
        float gravity = 4.0f;              // 下りでの加速
        bool multigrid = true;             // 64² から倍々に上げる
        // 反復（水滴を分けて流す回数）。掘れた谷を次の反復が見るので、多いほど水系が育つ。
        int iterations = 30;
        int seed = 1337;
        // 解析グリッド。**合成解像度とは別。**
        uint32_t resolution = 1024;
    };
    DropletSettings droplet;

    // 散布（kind == LayerKind::Scatter のときだけ意味を持つ）。
    //
    // terrain-editor の Scatter を移したもの。地形を `散布の間隔` で区切った格子の
    // 各マスへ 1 個ずつ形を置き、中心をずらして散らす。**大きさも間隔も m で持つ**
    // ので、合成解像度を変えても分布は変わらない。
    // 高さ 0 でも Mask / Unique は出る（分布だけを使う繋ぎ方のため）。
    struct ScatterSettings {
        ScatterShape shape = ScatterShape::Hemisphere;
        ScatterOrientation orientation = ScatterOrientation::Flat;
        int seed = 0;
        float densityMeters = 8.0f;    // 散布点の間隔
        float coverage = 1.0f;         // 実際に置く確率
        float sizeMinMeters = 5.0f;    // 個体の最小直径
        float sizeMaxMeters = 10.0f;   // 個体の最大直径
        float heightMeters = 0.0f;     // 盛り上げる高さ。0 でもマスクは出る
        float heightJitter = 0.5f;     // 個体ごとの高さのばらつき
        float rotationVariation = 1.0f;  // 向きのばらつき
        float aspectVariation = 0.3f;    // 細長さのばらつき
        // 重なりの溶け方。0 で高さの max（交差が折り目になる）、
        // 上げるほど soft max へ寄って metaball のように溶け合う。
        // **既定は 0**（terrain-editor と同じ見え方）。
        float smoothness = 0.0f;
    };
    ScatterSettings scatter;

    // **Height へ書き戻さない。** 加工（堆積 / 崩落）を、マスクを得るためだけに
    // 走らせるときに立てる。Result を繋がずに Mask だけを使う繋ぎ方のためのもの。
    // **保存しない。** グラフの繋ぎ方からコンパイルのたびに決まる。
    bool maskOnly = false;

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
