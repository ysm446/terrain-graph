#include "AtmosphereCommon.hlsli"
// マテリアルプレビューのメッシュ描画。
// 出力はトーンマップ前の線形放射輝度で、露出は後段の TonemapPass で掛ける。
//
// 2 枚目のレンダーターゲットへマテリアル UV を書き出す。ペイントのブラシパスが
// 「画面のこの画素はマテリアルのどこか」を引くために使う。CPU へ読み戻さずに
// 済ませるため、ID バッファではなく UV をそのまま持たせている。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"

struct MeshConstants
{
    float4x4 viewProjection;
    // カメラ空間で法線を見るための、投影を掛ける前のビュー行列。
    float4x4 view;
    float4x4 model;
    float4x4 normalMatrix;

    float3 cameraPosition;
    float waterTime;         // 波を動かす時刻（秒）

    float3 lightDirection;   // サーフェスから光源へ向かう方向
    float lightIlluminance;  // lux 相当

    float3 lightColor;
    float waveStrength;      // 水面の波の傾きの強さ。0 で波なし

    float3 baseColor;
    float roughness;

    float metallic;
    float iblIntensity;
    uint prefilteredMipCount;
    float waveScale;         // 一番大きなうねりの波長（m）

    uint irradianceIndex;    // irradiance キューブの SRV
    uint prefilteredIndex;   // プリフィルタ済みキューブの SRV
    uint brdfLutIndex;       // 環境 BRDF の LUT
    uint useMaterialTextures;  // 0 なら UI の単色パラメータを使う

    // 合成結果のチャンネル（bindless）
    uint materialBaseColorIndex;
    uint materialNormalIndex;
    uint materialSurfaceIndex;
    uint materialHeightIndex;

    // ビューポートに何を出すか（0 = シェーディング結果）。TG_VIEW_* と一致させる。
    uint debugView;
    // ハイトを形状に反映する量。0 なら押し出さない。
    float displacementScale;
    // 環境光を雲あり / 雲なしで混ぜる高さの範囲（m）。SampleAmbientIrradiance を参照。
    float ambientLow;
    float ambientHigh;

    float4x4 lightViewProjection;

    uint shadowIndex;  // 0xFFFFFFFF なら影を落とさない
    float shadowTexelSize;
    float shadowBias;
    float waveSpeed;         // 波の進む速さの倍率

    float4x4 cascadeViewProjections[4];
    uint4 cascadeShadowIndices;
    float4 cascadeSplits;
    float4 cascadeBiases;
    float cascadeBlend;
    float cascadeNear;
    uint shadowCascadeCount;
    float cascadePadding;

    // テセレーションの分割量を画面上の辺の長さから決めるために使う。
    // **シャドウパスでも本描画と同じ値を渡す。** 分割が違うと形がずれ、
    // 自分の影が自分に落ちて縞（シャドウアクネ）になる。
    float4x4 tessellationViewProjection;
    float2 viewportSize;
    float tessellationMaxFactor;
    float pad6;

    // マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引く。
    // maskPreviewLow / High は、マスク 0 / 1 に対応するベースカラー。
    uint maskPreviewHatch;
    float maskPreviewLow;
    float maskPreviewHigh;
    float waveDirection;     // うねりの進む向き（ラジアン。XZ 平面で +X から +Z へ）
    AtmosphericParameters atmosphere;
    uint cloudNoiseIndex;
    uint atmosphericMode;
    uint clearIrradianceIndex;  // 雲なしの環境の irradiance。0xFFFFFFFF なら混ぜない
    float ambientOcclusion;     // 雲が環境光を遮る強さ（0〜1）

    // 水の場（x = 水際からの符号付き距離 m、y = 符号付きの水深 m。水の中が正）と、波打ち際の設定。
    uint materialWaterIndex;
    float shoreFoam;     // 泡の量（0〜1）
    float shoreRunup;    // 寄せる波が水位より上へ這い上がる高さ（m）
    float shoreSpacing;  // 泡の筋の間隔（m）

    float shoreWidth;    // 泡の筋が出る、水際からの幅（m）
    float riverFoam;     // 川の早瀬の白波の量（0〜1）
    float2 shorePad;

    // 流れの場（xy = 速度 m/s〔ワールドの +X / +Z〕、z = 川の水面の被覆、w = 早瀬の度合い）と、川の波の設定。
    uint materialFlowIndex;
    float riverWaveStrength;  // 流れる波の傾きの強さ。0 で無し
    float riverWaveScale;     // 一番大きい模様の波長（m）
    float riverSpeed;         // 流れの速さの倍率
};

// 「ハイト（ローカル）」で周りの平均を取る半径（合成テクセル）と、
// 引いた差を 0〜1 へ伸ばす倍率。素材の凹凸が見える強さとして選んである。
static const float kLocalHeightRadiusTexels = 6.0f;
static const float kLocalHeightGain = 16.0f;

// ビューポートの表示モード。C++ 側の renderer::DebugView と一致させること。
#define TG_VIEW_SHADED          0
#define TG_VIEW_BASECOLOR       1
#define TG_VIEW_NORMAL_VIEW     2
#define TG_VIEW_NORMAL_WORLD    3
#define TG_VIEW_ROUGHNESS       4
#define TG_VIEW_METALLIC        5
#define TG_VIEW_AO              6
#define TG_VIEW_HEIGHT          7
#define TG_VIEW_HEIGHT_LOCAL    8
#define TG_VIEW_WIREFRAME       9
#define TG_VIEW_CLAY            10
#define TG_VIEW_LOD             11

ConstantBuffer<MeshConstants> g_mesh : register(b1);

// --- 合成結果のサンプリング ------------------------------------------------
// 平面 + UV スケール 1（タイルしない 1 枚絵のプレビュー）ではクランプで読む。
// wrap だと UV 端のバイリニア補間が反対側の端と混ざり、地形の縁が
// 反対側の高さへ引っ張られる。球はシーム（経度の 0/1）の連続性に wrap が
// 必要で、UV スケール > 1 は明示的なタイリングなので wrap のまま。
// サンプラは三項演算子で選べない（unique global resource の制約）ので分岐で書く。

// 合成結果は**タイルしない 1 枚絵**（平面 1 枚に等倍で貼る）なので、常にクランプで読む。
// wrap だと UV 端のバイリニア補間が反対側の端と混ざり、地形の縁が
// 反対側の高さへ引っ張られる。
float4 SampleMaterialColor(Texture2D<float4> map, float2 uv)
{
    return map.Sample(g_samplerAnisoClamp, uv);
}

float2 SampleMaterialNormal(Texture2D<float2> map, float2 uv)
{
    return map.Sample(g_samplerAnisoClamp, uv);
}

float SampleMaterialScalar(Texture2D<float> map, float2 uv)
{
    return map.Sample(g_samplerAnisoClamp, uv);
}

// 頂点 / ドメインシェーダ用（微分が無いので SampleLevel）。
float SampleMaterialScalarLevel(Texture2D<float> map, float2 uv)
{
    return map.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

// --- 水面の波 --------------------------------------------------------------------
// Liquid の水面へ重ねる、動く波の高さ（m）。合成には焼かない（時刻で動くので）。
// 波長を段々に小さくした勾配ノイズを、向きを少しずつ振りながら流して重ねる。大きい段がうねり、
// 小さい段が太陽のきらめきを作るさざ波。速さは深水波の分散（√波長に比例）に合わせる。
// footprint は 1 画素が水面で張る幅（m）。それより細かい段は消す（遠くでちらつかないように）。
static const int kWaveOctaves = 6;

// 段 i が、1 画素の幅 footprint に対してどれだけ残るか（0 で消える）。
float WaveOctaveFade(float wavelength, float footprint)
{
    return saturate(wavelength / max(footprint, 1e-4f) * 0.5f - 1.0f);
}

// 画素より細かくて消した段の割合（0〜1）。消した波の傾きは、鏡面の広がり（ラフネス）として戻す。
// そうしないと、遠くの水面が鏡のように平らになり、太陽の反射が細い線や点滅になる。
float WaveFilteredFraction(float footprint)
{
    float wavelength = max(g_mesh.waveScale, 0.1f);
    float lost = 0.0f;
    for (int i = 0; i < kWaveOctaves; ++i)
    {
        lost += 1.0f - WaveOctaveFade(wavelength, footprint);
        wavelength *= 0.37f;
    }
    return lost / float(kWaveOctaves);
}

float WaveHeight(float2 positionMeters, float footprint)
{
    const float time = g_mesh.waterTime * g_mesh.waveSpeed;
    float wavelength = max(g_mesh.waveScale, 0.1f);
    float angle = g_mesh.waveDirection;
    float sum = 0.0f;
    for (int i = 0; i < kWaveOctaves; ++i)
    {
        const float fade = WaveOctaveFade(wavelength, footprint);
        if (fade > 0.0f)
        {
            const float2 direction = float2(cos(angle), sin(angle));
            const float speed = 1.25f * sqrt(wavelength);
            const float2 p = (positionMeters - direction * (speed * time)) / wavelength + float(i) * 17.3f;
            // 高さを波長に比例させる（どの段も同じくらいの傾きになる）。
            sum += PerlinNoise(p, 4096.0f) * wavelength * fade;
        }
        wavelength *= 0.37f;
        // 段ごとに向きを左右へ振る（全部が同じ向きに流れると縞に見える）。
        angle += (i % 2 == 0) ? 0.7f : -1.1f;
    }
    return sum;
}

// 波の傾き（d高さ/dx, d高さ/dz）。中心差分。
float2 WaveSlope(float2 positionMeters, float footprint)
{
    const float epsilon = max(footprint * 0.5f, 0.02f);
    const float2 ex = float2(epsilon, 0.0f);
    const float2 ez = float2(0.0f, epsilon);
    return float2(WaveHeight(positionMeters + ex, footprint) - WaveHeight(positionMeters - ex, footprint),
                  WaveHeight(positionMeters + ez, footprint) - WaveHeight(positionMeters - ez, footprint)) /
           (2.0f * epsilon);
}

// --- 川の流れ --------------------------------------------------------------------
// River の水面へ重ねる、下流へ流れる波。流れの場（速度）で模様を運ぶ。
//
// 場所ごとに速度が違う模様をそのまま流し続けると、時間とともに引き伸ばされて崩れる。そこで
// **半周期ずらした 2 つの位相**を用意し、それぞれ「0 から 1 周期ぶんだけ流して最初へ戻す」を繰り返し、
// 戻す瞬間の重みが 0 になるように三角波で混ぜる（フローマップの定番）。戻る瞬間が全体で揃うと
// 脈打って見えるので、位相を場所でずらす。
static const int kRiverOctaves = 3;
// 模様を流しては戻す 1 周期（秒）。長いほど模様が長く続くが、速い流れで引き伸ばされる。
static const float kRiverCycleSeconds = 3.0f;

// 流れに乗る前の、止まった波の高さ（m）。
float RiverWaveHeight(float2 positionMeters, float footprint)
{
    float wavelength = max(g_mesh.riverWaveScale, 0.05f);
    float sum = 0.0f;
    for (int i = 0; i < kRiverOctaves; ++i)
    {
        const float fade = WaveOctaveFade(wavelength, footprint);
        if (fade > 0.0f)
        {
            sum += PerlinNoise(positionMeters / wavelength + float(i) * 23.1f, 4096.0f) * wavelength * fade;
        }
        wavelength *= 0.37f;
    }
    return sum;
}

float2 RiverWaveSlopeAt(float2 positionMeters, float footprint)
{
    const float epsilon = max(footprint * 0.5f, 0.01f);
    const float2 ex = float2(epsilon, 0.0f);
    const float2 ez = float2(0.0f, epsilon);
    return float2(RiverWaveHeight(positionMeters + ex, footprint) - RiverWaveHeight(positionMeters - ex, footprint),
                  RiverWaveHeight(positionMeters + ez, footprint) - RiverWaveHeight(positionMeters - ez, footprint)) /
           (2.0f * epsilon);
}

struct RiverSample
{
    float2 slope;  // 波の傾き（d高さ/dx, d高さ/dz）
    float foam;    // 早瀬の白波（0〜1）
};

// velocity はワールドの XZ の速度（m/s）、rapids は早瀬の度合い（0〜1）。
RiverSample SampleRiverFlow(float2 worldXz, float2 velocity, float rapids, float footprint)
{
    RiverSample result;
    const float2 flow = velocity * g_mesh.riverSpeed;
    // 位相のずれ（戻る瞬間を場所でばらす）。
    const float offset = PerlinNoise(worldXz / 37.0f, 4096.0f) * 0.5f;
    const float phase0 = frac(g_mesh.waterTime / kRiverCycleSeconds + offset);
    const float phase1 = frac(phase0 + 0.5f);
    // 模様は流れと一緒に動く: 位置 − 速度 × 経過。2 つ目は別の模様に見えるよう大きくずらす。
    const float2 p0 = worldXz - flow * (phase0 * kRiverCycleSeconds);
    const float2 p1 = worldXz - flow * (phase1 * kRiverCycleSeconds) + float2(53.7f, 19.3f);
    const float weight0 = 1.0f - abs(2.0f * phase0 - 1.0f);

    result.slope = float2(0.0f, 0.0f);
    if (g_mesh.riverWaveStrength > 0.0f)
    {
        result.slope = lerp(RiverWaveSlopeAt(p1, footprint), RiverWaveSlopeAt(p0, footprint), weight0);
    }
    result.foam = 0.0f;
    if (g_mesh.riverFoam > 0.0f && rapids > 0.0f)
    {
        // 白波も流れに乗せる。早瀬の度合いが上がるほど、泡の切れ目が埋まる。
        const float lace0 = PerlinNoise(p0 / 1.3f + 7.0f, 4096.0f);
        const float lace1 = PerlinNoise(p1 / 1.3f + 7.0f, 4096.0f);
        const float lace = lerp(lace1, lace0, weight0);
        result.foam = saturate(lace * 1.3f + rapids * 0.7f - 0.15f) * rapids * g_mesh.riverFoam;
    }
    return result;
}

// --- 波打ち際 ------------------------------------------------------------------
// 水の場（水際からの距離と水深）から、岸へ向かって進む泡の筋と、浜を這い上がって引く波を作る。
//
// - 泡の筋の位相は「水際からの距離 / 間隔 + 時刻」。距離の等値線は岸の形に沿うので、どの筋も
//   向きを指定しなくても岸へ進む。位相を場所ごとに大きくずらすので、筋は岸と平行に揃わず、
//   着く時刻も間隔も場所でばらつく。
// - **波には 1 本ずつ番号がある**（位相の整数部）。番号ごとに違う模様で「岸に沿ったどこで強いか」を
//   決めるので、1 本の波が岸の全周で同時に寄せることはなく、区間ごとにばらばらに押し寄せる。
//   強い区間は浜の高い所まで這い上がり、弱い区間はほとんど上がらない。
// - 寄せ返しは、筋が水際へ着いた瞬間から始まる。素早く這い上がり、ゆっくり引く。
//   **引く波**は、水の膜に乗った泡の模様を沖の向き（距離の傾き）へ流して見せ、引いた跡には
//   しばらくつやが残る。浅い水の中でも、同じ模様が沖へ流れる。
struct ShoreSample
{
    float foam;      // 泡（0〜1）
    float wash;      // 寄せた波に覆われている（陸の側。0〜1）
    float sheen;     // 波が引いた直後のつや（陸の側。0〜1）
    float wet;       // 波が届く範囲の濡れ（陸の側。0〜1）
};

// 水の場の「Liquid が書いていない」値（CompositeLayer.hlsl と揃える）。
static const float kWaterNone = -10000.0f;
// 泡の筋が岸へ進む速さ（m/s。浅瀬の波は水深が浅いほど遅い。数 m の水深の目安）。
static const float kShoreWaveSpeed = 3.0f;
// 寄せ返しの 1 周期のうち、這い上がるのに使う割合（残りで引く）。
static const float kShoreUprush = 0.25f;

float ShoreNoise(float2 p)
{
    return PerlinNoise(p, 4096.0f);
}

// 番号 index の波が、この場所でどれだけ強いか（0〜1）。番号ごとに模様をずらす。
float ShoreWaveStrength(float2 setCoordinate, float index)
{
    return saturate(ShoreNoise(setCoordinate + index * float2(13.7f, 7.3f)) * 1.4f + 0.5f);
}

ShoreSample SampleShore(float2 uv, float2 worldXz)
{
    ShoreSample result;
    result.foam = 0.0f;
    result.wash = 0.0f;
    result.sheen = 0.0f;
    result.wet = 0.0f;

    Texture2D<float2> waterMap = ResourceDescriptorHeap[g_mesh.materialWaterIndex];
    const float2 field = waterMap.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float distance = field.x;  // 水の中が正
    const float depth = field.y;     // 水の中が正。陸では −（水位からの高さ）

    // 沖へ向かう向き（ワールドの XZ）。距離の傾きを、画面微分から XZ の傾きへ直す
    // （UV とワールドの対応に依らない）。分岐の前で取る。
    const float2 dx = ddx(worldXz);
    const float2 dy = ddy(worldXz);
    const float distanceDx = ddx(distance);
    const float distanceDy = ddy(distance);
    const float determinant = dx.x * dy.y - dx.y * dy.x;
    float2 offshore = float2(0.0f, 0.0f);
    if (abs(determinant) > 1e-12f)
    {
        const float2 gradient = float2(dy.y * distanceDx - dx.y * distanceDy,
                                       dx.x * distanceDy - dy.x * distanceDx) / determinant;
        const float gradientLength = length(gradient);
        if (gradientLength > 1e-4f)
        {
            offshore = gradient / gradientLength;
        }
    }

    if ((g_mesh.shoreFoam <= 0.0f && g_mesh.shoreRunup <= 0.0f) || depth < kWaterNone * 0.5f)
    {
        return result;
    }

    const float spacing = max(g_mesh.shoreSpacing, 1.0f);
    const float time = g_mesh.waterTime * g_mesh.waveSpeed;
    // 位相のずれ。大きい模様で着く時刻を 1 周期以上ずらし（筋が岸に対して斜めになり、間隔もばらつく）、
    // 小さい模様で筋の線をうねらせる。
    const float along = ShoreNoise(worldXz / (spacing * 6.0f)) * 1.5f +
                        ShoreNoise(worldXz / (spacing * 1.7f) + 31.7f) * 0.35f;
    // 水際（距離 0）での位相。整数部が「いま水際へ着いている波の番号」、小数部 0 が着いた瞬間。
    const float arrival = time * kShoreWaveSpeed / spacing + along;
    // 波ごとの強さの模様（岸に沿った区間の大きさ）。
    const float2 setCoordinate = worldXz / (spacing * 2.5f);

    // --- 寄せ返しの状態（水際へ着いている波） -----------------------------------------
    const float swashIndex = floor(arrival);
    const float swash = arrival - swashIndex;
    const float swashStrength = ShoreWaveStrength(setCoordinate, swashIndex);
    const float up = smoothstep(0.0f, kShoreUprush, swash);     // 這い上がり（0 → 1）
    const float down = smoothstep(kShoreUprush, 1.0f, swash);   // 引き（0 → 1）
    const bool retreating = swash >= kShoreUprush;
    // この波が届く一番高い所と、いまの水の縁の高さ（m。水位から）。
    const float crestLevel = g_mesh.shoreRunup * (0.25f + 1.0f * swashStrength);
    const float level = crestLevel * (retreating ? 1.0f - down : up);
    // 水の膜に乗った泡の模様。這い上がる間は陸へ、引く間は沖へ流す（m）。
    const float slide = retreating ? down * 6.0f : -up * 2.5f;
    const float lace = saturate(ShoreNoise((worldXz - offshore * slide) / 0.9f) * 1.7f + 0.3f) *
                       saturate(ShoreNoise((worldXz - offshore * slide * 0.6f) / 3.1f + 11.0f) * 1.5f + 0.6f);
    // 膜の泡の濃さ。寄せた直後が一番濃く、引くにつれて消える。
    const float laceAmount = retreating ? (1.0f - down) : up;

    // 泡のむら（細かく切れる）。
    const float breakup = saturate(ShoreNoise(worldXz / 3.0f + time * 0.15f) * 0.8f + 0.65f);

    if (depth > 0.0f)
    {
        // --- 水の中: 岸へ進む泡の筋 -------------------------------------------------
        if (g_mesh.shoreFoam > 0.0f && g_mesh.shoreWidth > 0.0f)
        {
            const float raw = distance / spacing + arrival;
            const float index = floor(raw);
            const float phase = raw - index;
            // 筋の前（岸側）は切り立ち、後ろへ尾を引く。phase 0 が筋の前縁。
            const float crest = smoothstep(0.0f, 0.06f, phase) * pow(saturate(1.0f - phase), 4.0f);
            // 沖では出さず、岸へ近づくほど濃く。
            const float zone = saturate(1.0f - distance / g_mesh.shoreWidth);
            // この波がこの区間で強いか。弱い区間は筋が途切れる。
            const float strength = ShoreWaveStrength(setCoordinate, index);
            result.foam = crest * zone * breakup * 2.4f * strength * strength;
            // 汀線の縁の泡（水深がごく浅い帯に細く残る）。
            result.foam += smoothstep(0.25f, 0.0f, depth) * breakup * 0.5f;
            // 浅い所では、引く波に乗った泡が沖へ流れる。
            result.foam += (1.0f - smoothstep(0.0f, 0.6f, depth)) * lace * laceAmount * swashStrength * 0.9f;
            result.foam *= g_mesh.shoreFoam;
        }
    }
    else if (g_mesh.shoreRunup > 0.0f)
    {
        // --- 陸の側: 寄せる波、引く波、濡れ -------------------------------------------
        const float above = -depth;  // 水位からの高さ（m）
        result.wash = smoothstep(level, level - 0.03f, above);
        // 先端の泡。這い上がる間は濃く、引く間は薄れる。
        const float edge = smoothstep(level - 0.12f, level - 0.02f, above) * result.wash;
        // 膜に乗った泡（引く間は沖へ流れて消えていく）。
        const float sheet = result.wash * lace * laceAmount * 0.8f;
        result.foam = (edge * (1.0f - 0.7f * down) * 1.5f + sheet) * g_mesh.shoreFoam;
        // 引いた跡。この波が届いた高さまで、つやが残って乾いていく。
        result.sheen = retreating
            ? smoothstep(crestLevel, crestLevel - 0.05f, above) * (1.0f - result.wash) * (1.0f - down)
            : 0.0f;
        // 波が届きうる範囲は濡れている（一番強い波が届く高さの少し上まで、ぼかして）。
        result.wet = smoothstep(g_mesh.shoreRunup * 1.45f, g_mesh.shoreRunup * 1.1f, above);
    }
    result.foam = saturate(result.foam);
    return result;
}

struct VsInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct VsOutput
{
    float4 clipPosition  : SV_Position;
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
};

// ライトから見た深度と比べて、この画素が影の中かを返す（1 = 当たっている）。
//
// 深度は普通の Texture2D として読む（比較サンプラは使わない）。
// 3x3 のポイントサンプルで平均を取り、境界のジャギーを和らげる。
float SampleShadow(float3 worldPosition, float nDotL, uint shadowIndex, float texelSize,
                   float bias, float4x4 lightViewProjection)
{
    if (shadowIndex == kInvalidTextureIndex)
    {
        return 1.0f;
    }

    const float4 lightClip = mul(lightViewProjection, float4(worldPosition, 1.0f));
    if (lightClip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 ndc = lightClip.xyz / lightClip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    // 範囲の外は影を落とさない（シャドウマップが覆っていない）。
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    // 斜めに当たっているほど自己遮蔽しやすいので、下駄を増やす。
    const float slopeBias = bias * (1.0f + 3.0f * (1.0f - saturate(nDotL)));

    Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(shadowIndex)];
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * texelSize;
            const float depth = shadowMap.SampleLevel(g_samplerPointClamp, uv + offset, 0.0f);
            visibility += (ndc.z - slopeBias <= depth) ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

// カメラ前方距離で選択し、隣接する区間の重複範囲で混ぜる。
float SampleCascadedShadow(float3 worldPosition, float nDotL)
{
    if (g_mesh.shadowCascadeCount == 0)
        return SampleShadow(worldPosition, nDotL, g_mesh.shadowIndex,
                            g_mesh.shadowTexelSize, g_mesh.shadowBias, g_mesh.lightViewProjection);
    if (g_mesh.cascadeShadowIndices.x == kInvalidTextureIndex) return 1.0f;
    if (g_mesh.shadowCascadeCount == 1)
        return SampleShadow(worldPosition, nDotL, g_mesh.cascadeShadowIndices.x,
            g_mesh.shadowTexelSize, g_mesh.cascadeBiases.x, g_mesh.cascadeViewProjections[0]);
    const uint lastCascade = g_mesh.shadowCascadeCount - 1;
    const float distance = -mul(g_mesh.view, float4(worldPosition, 1.0f)).z;
    if (distance > g_mesh.cascadeSplits[lastCascade]) return 1.0f;
    uint cascade = 0;
    while (cascade < lastCascade && distance > g_mesh.cascadeSplits[cascade]) ++cascade;
    const float visibility = SampleShadow(worldPosition, nDotL, g_mesh.cascadeShadowIndices[cascade],
        g_mesh.shadowTexelSize, g_mesh.cascadeBiases[cascade], g_mesh.cascadeViewProjections[cascade]);
    const float start = cascade == 0 ? g_mesh.cascadeNear : g_mesh.cascadeSplits[cascade - 1];
    const float end = g_mesh.cascadeSplits[cascade];
    const float blendStart = end - (end - start) * g_mesh.cascadeBlend;
    if (distance <= blendStart) return visibility;
    const uint nextCascade = min(cascade + 1, lastCascade);
    const float next = cascade < lastCascade ? SampleShadow(worldPosition, nDotL, g_mesh.cascadeShadowIndices[nextCascade],
        g_mesh.shadowTexelSize, g_mesh.cascadeBiases[nextCascade], g_mesh.cascadeViewProjections[nextCascade]) : 1.0f;
    return lerp(visibility, next, smoothstep(blendStart, end, distance));
}

// --- ディスプレイスメント -------------------------------------------------
// 合成した Height を読み、ワールド空間の法線方向へ押し引きする。
// **VsMain と DsMain の両方がこの関数を通る。** 別々の式を書くと、
// モデル行列を入れたときにテセレーションの ON / OFF で形が変わってしまう。
// 頂点 / ドメインシェーダには微分が無いので SampleLevel を使う。
float3 ApplyDisplacement(float3 worldPosition, float3 worldNormal, float2 uv)
{
    if (g_mesh.useMaterialTextures == 0u || g_mesh.displacementScale == 0.0f)
    {
        return worldPosition;
    }
    Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
    const float height = SampleMaterialScalarLevel(heightMap, uv);
    // 高さの中央（0.5）を基準にする。全体が膨らまないようにするため。
    return worldPosition + worldNormal * ((height - 0.5f) * g_mesh.displacementScale);
}

VsOutput VsMain(VsInput input)
{
    VsOutput output;

    const float3 worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    float3 worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    worldPosition = ApplyDisplacement(worldPosition, normalize(worldNormal), input.uv);

    output.worldPosition = worldPosition;
    output.clipPosition = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    output.worldNormal = worldNormal;
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;

    return output;
}

// --- テセレーション -------------------------------------------------------
//
// 分割量は**画面上の辺の長さ**から決める。細かいメッシュではそのまま 1 になり、
// 近づいて 1 辺が伸びたときだけ細かく割る。ディスプレイスメントは
// ドメインシェーダで掛ける（分割後の点で高さを引くため）。

// 1 辺をおよそ何ピクセルに保つか。小さいほど細かく割る。
static const float kTessellationTargetPixels = 10.0f;

struct HsControlPoint
{
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
};

struct HsPatchConstants
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

// 投影も変位もせず、ワールド空間の制御点を出すだけ。
HsControlPoint VsControl(VsInput input)
{
    HsControlPoint output;
    output.worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    output.worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    return output;
}

// ワールド空間の 2 点が画面上で何ピクセル離れるか。
float ScreenEdgeFactor(float3 a, float3 b)
{
    const float4 clipA = mul(g_mesh.tessellationViewProjection, float4(a, 1.0f));
    const float4 clipB = mul(g_mesh.tessellationViewProjection, float4(b, 1.0f));
    // カメラの後ろに回った辺は判断できないので、最大まで割る。
    if (clipA.w <= 0.0f || clipB.w <= 0.0f)
    {
        return g_mesh.tessellationMaxFactor;
    }

    const float2 screenA = (clipA.xy / clipA.w) * 0.5f * g_mesh.viewportSize;
    const float2 screenB = (clipB.xy / clipB.w) * 0.5f * g_mesh.viewportSize;
    const float pixels = length(screenA - screenB);
    return clamp(pixels / kTessellationTargetPixels, 1.0f, g_mesh.tessellationMaxFactor);
}

HsPatchConstants HsConstant(InputPatch<HsControlPoint, 3> patch)
{
    HsPatchConstants output;
    // SV_TessFactor[i] は「制御点 i の向かい側の辺」に対応する。
    output.edges[0] = ScreenEdgeFactor(patch[1].worldPosition, patch[2].worldPosition);
    output.edges[1] = ScreenEdgeFactor(patch[2].worldPosition, patch[0].worldPosition);
    output.edges[2] = ScreenEdgeFactor(patch[0].worldPosition, patch[1].worldPosition);
    output.inside = (output.edges[0] + output.edges[1] + output.edges[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HsConstant")]
HsControlPoint HsMain(InputPatch<HsControlPoint, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
VsOutput DsMain(HsPatchConstants patchConstants, float3 barycentric : SV_DomainLocation,
                const OutputPatch<HsControlPoint, 3> patch)
{
    VsOutput output;

    float3 worldPosition = patch[0].worldPosition * barycentric.x +
                           patch[1].worldPosition * barycentric.y +
                           patch[2].worldPosition * barycentric.z;
    const float3 worldNormal = normalize(patch[0].worldNormal * barycentric.x +
                                         patch[1].worldNormal * barycentric.y +
                                         patch[2].worldNormal * barycentric.z);
    const float3 worldTangent = patch[0].worldTangent * barycentric.x +
                                patch[1].worldTangent * barycentric.y +
                                patch[2].worldTangent * barycentric.z;
    const float2 uv = patch[0].uv * barycentric.x + patch[1].uv * barycentric.y +
                      patch[2].uv * barycentric.z;

    // 分割後の点で高さを引いて押し出す。式は VsMain と共通の ApplyDisplacement。
    worldPosition = ApplyDisplacement(worldPosition, worldNormal, uv);

    output.worldPosition = worldPosition;
    output.clipPosition = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.tangentSign = patch[0].tangentSign;
    output.uv = uv;
    return output;
}

struct PsOutput
{
    float4 color : SV_Target0;
    // xy: マテリアル UV（タイル 1 枚ぶんに畳んだもの）、z: メッシュに当たったか
    float4 materialUv : SV_Target1;
};

PsOutput PsMain(VsOutput input)
{
    const float3 geometricNormal = normalize(input.worldNormal);
    const float3 viewDirection = normalize(g_mesh.cameraPosition - input.worldPosition);

    float3 baseColor = g_mesh.baseColor;
    float roughnessValue = g_mesh.roughness;
    float metallicValue = g_mesh.metallic;
    float ambientOcclusion = 1.0f;
    float3 normal = geometricNormal;

    // **クレイ表示**は、形（変位）はそのままで陰影だけをテクスチャ抜きにする。
    // 合成の色 / 法線 / サーフェスを読まず、単色マテリアルと面の向きで塗る。
    // LOD の色分け表示でも地形はクレイで塗る（色は配置モデルだけに付ける）。
    const bool clay = (g_mesh.debugView == TG_VIEW_CLAY || g_mesh.debugView == TG_VIEW_LOD);
    const bool useMaterialShading = (g_mesh.useMaterialTextures != 0u) && !clay;

    if (clay)
    {
        // **面から法線を起こす。** 平面メッシュの頂点法線は押し出しても上を向いた
        // ままなので、そのまま陰影を付けると形が出ない。画面微分から取れば
        // 実際に描かれた三角形の向きになり、分割の粗さが面として見える
        // （メッシュの確認にはこれが要る）。
        const float3 faceNormal =
            normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
        // 三角形の巻き方によって裏返るので、視線の側へ向ける。
        normal = (dot(faceNormal, viewDirection) < 0.0f) ? -faceNormal : faceNormal;
    }

    if (useMaterialShading)
    {
        Texture2D<float4> baseColorMap = ResourceDescriptorHeap[g_mesh.materialBaseColorIndex];
        Texture2D<float2> normalMap    = ResourceDescriptorHeap[g_mesh.materialNormalIndex];
        Texture2D<float4> surfaceMap   = ResourceDescriptorHeap[g_mesh.materialSurfaceIndex];

        const float2 uv = input.uv;

        baseColor = SampleMaterialColor(baseColorMap, uv).rgb;

        // アルファは 1 − 水面の被覆（CompositeLayer.hlsl の Liquid が書く）。
        const float4 surface = SampleMaterialColor(surfaceMap, uv);
        roughnessValue = surface.r;
        metallicValue = surface.g;
        ambientOcclusion = surface.b;

        // タンジェント空間法線をワールド空間へ移す。
        const float3 tangentNormal = DecodeTangentNormal(SampleMaterialNormal(normalMap, uv));
        const float3 tangent =
            normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
        const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           geometricNormal * tangentNormal.z);

        // 水面には動く波の傾きを重ねる。水面は水平なので、ワールドの XZ の傾きをそのまま法線へ足す。
        const float water = 1.0f - surface.a;
        if (water > 0.001f && g_mesh.waveStrength > 0.0f)
        {
            const float2 worldXz = input.worldPosition.xz;
            const float footprint = max(length(ddx(worldXz)), length(ddy(worldXz)));
            // 勾配ノイズの傾きは 1 前後まで出る。強さ 1 で「荒れた海」程度（傾き 0.3 前後）に収める。
            const float2 slope = WaveSlope(worldXz, footprint) * (g_mesh.waveStrength * 0.3f * water);
            normal = normalize(normal + float3(-slope.x, 0.0f, -slope.y));
            // 画素より細かい波は、ラフネスとして残す。
            const float filtered = 0.45f * g_mesh.waveStrength * sqrt(WaveFilteredFraction(footprint));
            roughnessValue = lerp(roughnessValue, max(roughnessValue, filtered), water);
        }

        // 川（River）の水面には、流れの場に沿って下流へ流れる波と、早瀬の白波を重ねる。
        if (g_mesh.riverWaveStrength > 0.0f || g_mesh.riverFoam > 0.0f)
        {
            Texture2D<float4> flowMap = ResourceDescriptorHeap[g_mesh.materialFlowIndex];
            const float4 flow = flowMap.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
            if (flow.z > 0.001f)
            {
                const float2 worldXz = input.worldPosition.xz;
                const float footprint = max(length(ddx(worldXz)), length(ddy(worldXz)));
                const RiverSample river = SampleRiverFlow(worldXz, flow.xy, flow.w, footprint);
                const float2 slope = river.slope * (g_mesh.riverWaveStrength * 0.3f * flow.z);
                normal = normalize(normal + float3(-slope.x, 0.0f, -slope.y));
                const float foam = river.foam * flow.z;
                baseColor = lerp(baseColor, float3(0.82f, 0.85f, 0.86f), foam);
                roughnessValue = lerp(roughnessValue, 0.6f, foam);
            }
        }

        // 波打ち際。濡れた浜は暗くつややかに、寄せた波は薄い水の膜に、泡は白くざらつかせる。
        const ShoreSample shore = SampleShore(uv, input.worldPosition.xz);
        if (shore.wet > 0.0f || shore.foam > 0.0f || shore.sheen > 0.0f)
        {
            baseColor = lerp(baseColor, baseColor * 0.55f, shore.wet);
            roughnessValue = lerp(roughnessValue, 0.35f, shore.wet * 0.7f);
            roughnessValue = lerp(roughnessValue, 0.14f, shore.sheen * 0.85f);
            roughnessValue = lerp(roughnessValue, 0.08f, shore.wash);
            baseColor = lerp(baseColor, float3(0.82f, 0.85f, 0.86f), shore.foam);
            roughnessValue = lerp(roughnessValue, 0.6f, shore.foam);
            // 泡と水の膜は地面の細かい凹凸を覆う。
            normal = normalize(lerp(normal, geometricNormal, saturate(shore.foam + shore.wash) * 0.7f));
        }
    }

    // --- マスクのプレビューで、飽和した所へ斜線を引く ----------------------
    //
    // マスクのプレビューは「0 の色」と「1 の色」の間を塗るだけなので、
    // ベースカラーから元のマスクへ戻せる。**0 か 1 に張り付いている所**へ
    // 1px 幅・4px 周期の斜線を中間の灰色で重ね、飽和していることを見せる。
    // 濃淡が付いている所と、上限に当たって潰れた所は、絵では見分けが付かない。
    //
    // 画素の座標は **x と y を別々に切り捨ててから足す**。float のまま足して
    // 丸めると桁落ちで縞の位相が揺れ、太いバンドに見える（terrain-editor で踏んだ）。
    if (g_mesh.maskPreviewHatch != 0u && useMaterialShading)
    {
        const float low = g_mesh.maskPreviewLow;
        const float high = g_mesh.maskPreviewHigh;
        const float mask = saturate((baseColor.r - low) / max(high - low, 1e-4f));
        if (mask >= 0.99f || mask <= 0.01f)
        {
            const int2 pixel = int2(int(input.clipPosition.x), int(input.clipPosition.y));
            if (((pixel.x + pixel.y) & 3) == 3)
            {
                const float stripe = lerp(low, high, 0.5f);
                baseColor = float3(stripe, stripe, stripe);
            }
        }
    }

    // --- チャンネルを覗く表示 ----------------------------------------------
    // チャンネルの中身をそのまま出す。露出もトーンマップも掛けない
    // （後段の TonemapPass が素通しする）。**クレイはここへ来ない。**
    // 陰影を付ける表示なので、下のシェーディングをそのまま通す。
    if (g_mesh.debugView != TG_VIEW_SHADED && !clay)
    {
        float3 debugColor = float3(0.0f, 0.0f, 0.0f);
        if (g_mesh.debugView == TG_VIEW_BASECOLOR)
        {
            // ベースカラーはリニアで持っているので、見た目を合わせて sRGB で出す。
            debugColor = LinearToSrgb(saturate(baseColor));
        }
        else if (g_mesh.debugView == TG_VIEW_NORMAL_VIEW)
        {
            // 陰影に使う向きを**カメラ空間**で見る。ビュー行列は回転と平行移動だけ
            // なので、上 3x3 を掛ければ向きが移る（正規化は数値誤差の始末）。
            // カメラは -Z を向く（右手系）ので、正面を向いた面が +Z＝水色になり、
            // 法線マップと同じ読み方（平らなら水色）ができる。
            const float3 viewNormal = normalize(mul((float3x3)g_mesh.view, normal));
            debugColor = viewNormal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == TG_VIEW_NORMAL_WORLD)
        {
            // 陰影に実際に使う向き。法線マップを当てたあとのワールド空間法線。
            debugColor = normal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == TG_VIEW_ROUGHNESS)
        {
            debugColor = roughnessValue.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_METALLIC)
        {
            debugColor = metallicValue.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_AO)
        {
            debugColor = ambientOcclusion.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_WIREFRAME)
        {
            // 線だけを見る表示。塗りではないので単色で描く。
            debugColor = float3(0.66f, 0.72f, 0.78f);
        }
        else if (g_mesh.debugView == TG_VIEW_HEIGHT)
        {
            float height = 0.0f;
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                height = SampleMaterialScalar(heightMap, input.uv);
            }
            debugColor = saturate(height).xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_HEIGHT_LOCAL)
        {
            // **その場の起伏だけ**を見る。地形の大きな高さ（標高差 600m の傾き）を
            // 周りの平均として引き、残りを 0.5 中心へ伸ばす。
            // 素材のハイトマップをそのまま貼ったような見た目になる。
            float local = 0.5f;
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                const float center = SampleMaterialScalar(heightMap, input.uv);

                // 周りの平均。半径は合成テクセル基準で固定する
                // （解像度を変えても「どのくらい大きな形を引くか」が変わらない）。
                float2 size = float2(1.0f, 1.0f);
                heightMap.GetDimensions(size.x, size.y);
                const float2 texel = 1.0f / max(size, float2(1.0f, 1.0f));
                const float radius = kLocalHeightRadiusTexels;
                float sum = 0.0f;
                [unroll]
                for (int i = 0; i < 8; ++i)
                {
                    const float angle = (float(i) / 8.0f) * 6.28318530718f;
                    const float2 offset = float2(cos(angle), sin(angle)) * radius * texel;
                    sum += SampleMaterialScalar(heightMap, input.uv + offset);
                }
                local = 0.5f + (center - sum / 8.0f) * kLocalHeightGain;
            }
            debugColor = saturate(local).xxx;
        }

        PsOutput debugOutput;
        debugOutput.color = float4(debugColor, 1.0f);
        debugOutput.materialUv = float4(frac(input.uv), 1.0f, 0.0f);
        return debugOutput;
    }

    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallicValue, diffuseColor, f0);

    const float roughness = clamp(roughnessValue, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_mesh.lightDirection);
    // 影は直接光にだけ掛ける。環境光（IBL）は別に扱う。
    const float shadow = SampleCascadedShadow(input.worldPosition, dot(normal, lightDirection));

    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_mesh.lightColor, g_mesh.lightIlluminance,
                                            diffuseColor, f0, roughness) *
                      shadow * (g_mesh.atmosphericMode != 0 ?
                          CloudShadow(input.worldPosition, g_mesh.atmosphere, g_mesh.cloudNoiseIndex) : 1.0);

    // --- IBL（分割和近似） -------------------------------------------------
    // saturate + 加算だと最大 1.00001 になり、FresnelSchlickRoughness の
    // pow(1 - nDotV, 5) が負の底で NaN になる。clamp で上限も守る。
    const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

    TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_mesh.prefilteredIndex];
    Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_mesh.brdfLutIndex];

    // irradiance マップには E / pi（平均放射輝度）が入っているので、
    // diffuseColor を掛けるだけでよい。雲の上の地形は、雲なしの環境で照らす。
    const float3 irradiance = SampleAmbientIrradiance(g_mesh.irradianceIndex, g_mesh.clearIrradianceIndex, normal,
                                                      input.worldPosition.y, g_mesh.ambientLow, g_mesh.ambientHigh,
                                                      g_mesh.ambientOcclusion);

    const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, roughness);
    const float3 kD = 1.0f - fresnel;
    const float3 diffuseIbl = kD * diffuseColor * irradiance;

    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float mipLevel = roughness * float(max(g_mesh.prefilteredMipCount, 1u) - 1u);
    const float3 prefiltered =
        prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;

    const float2 environmentBrdf =
        brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, roughness), 0.0f);
    const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

    radiance += (diffuseIbl + specularIbl) * g_mesh.iblIntensity * ambientOcclusion;

    PsOutput output;
    // シーンカラーは R16G16B16A16_FLOAT。half の上限（65504）を超えると Inf になり、
    // トーンマップを経て NaN → ハイライト中心の黒点になる。上限手前でクランプする。
    output.color = float4(min(radiance, 60000.0f), 1.0f);
    // ペイントマスクはタイル 1 枚ぶんのテクスチャなので、UV も畳んで書き出す。
    output.materialUv = float4(frac(input.uv), 1.0f, 0.0f);
    return output;
}
