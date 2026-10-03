// レイヤー 1 枚ぶんを合成結果へ積む。
//
// 出力の 4 枚は UAV として読み書きする。各スレッドは自分のテクセルしか触らないので、
// 同一ディスパッチ内での読み書きは安全。レイヤー間は UAV バリアで区切る。
//
// 出力タイル矩形と解像度を引数に取る形は崩さないこと（エクスポート時のタイル評価に必要）。

#include "CompositeCommon.hlsli"
#include "CompositePath.hlsli"
#include "LayerMaterial.hlsli"

#define TG_SOURCE_CONSTANT 0
#define TG_SOURCE_NOISE    1
#define TG_SOURCE_TEXTURE  2
// 3..6 は合成の中間結果に由来するマスク。CompositeMask パスが事前に計算する。
#define TG_SOURCE_DERIVED  3
// ブラシで描いたマスク。PaintMaskStore が持つテクスチャをそのまま読む。
#define TG_SOURCE_PAINT    7


#define TG_FLAG_MASK_INVERT 0x1u
#define TG_FLAG_BASE_LAYER  0x2u
// レイヤーの種類（compositor::LayerKind）。どちらも立っていなければサーフェス。
#define TG_FLAG_KIND_SHAPE  0x4u
#define TG_FLAG_KIND_LIQUID 0x8u
// 下地に沿わせる（Mixer の Wrap to Underlying。サーフェスのみ）。
#define TG_FLAG_WRAP        0x10u
// 法線マップの緑を反転して読む（OpenGL 規約の素材）。
#define TG_FLAG_FLIP_NORMAL_GREEN 0x20u

struct LayerConstants
{
    uint4 outputIndices;  // BaseColor, Normal, Surface, Height の UAV
    uint4 tile;           // x, y, width, height（出力全体の中での矩形）
    uint2 resolution;     // 出力全体の解像度
    uint channelMask;     // 書き込むチャンネルのビット
    uint flags;

    float4 baseColor;      // rgb
    float4 surfaceParams;  // roughness, metallic, ao, heightBase
    float4 blendParams;    // blendRange, heightPerSize, uvScale, heightSource
    float4 maskParams;     // constant, levelsLow, levelsHigh, maskSource
    // ハイトはノイズの amount を使わず、y に heightGain を入れる。
    float4 heightNoise;    // scale, heightGain, octaves, offset
    float4 maskNoise;      // scale, amount, octaves, offset

    // 参照するテクスチャの SRV インデックス。kInvalidTextureIndex なら定数を使う。
    uint4 textureIndices0;  // baseColor, normal, roughness, metallic
    uint4 textureIndices1;  // ao, height, mask, 中間結果由来マスクの SRV

    float4 maskCurve;   // contrast, 未使用 x3（derivedScale は CompositeMask 側で適用済み）
    uint4 noiseTypes;   // height, mask, 未使用, 未使用
    uint4 paintParams;  // ペイントマスクの SRV, 未使用 x3
    // スカラーのマップのチャンネル指定。4bit ずつ TG_CHANNEL_SLOT_* の順で詰めてある。
    uint4 mapChannels;  // x にすべて入る。yzw は未使用
    // ベースカラーの調整。マテリアルが持つ（ティントを掛けた**あと**に効く）。
    float4 colorAdjust;  // 色相（ラジアン）, 彩度, 明度, 未使用
    // パス UV（Surface の UV Path）。繰り返し長（m）, 幅方向の枚数, 進行方向のずれ（m）, 一辺（m）
    float4 pathUvParams;
    // 線分バッファの SRV（無ければ kInvalidTextureIndex）, 線分数,
    // フラグ（bit0: 進行方向を U に当てる、bit1: 点の強さを覆い具合に掛ける）,
    // マスク画像の SRV（無ければ kInvalidTextureIndex）
    uint4 pathUvIndices;
    // 縁のカーブ（ガンマ）, マスク画像の繰り返し長（m）, マスク画像の幅方向の枚数, マスク画像を反転（0 / 1）
    float4 pathUvParams2;
    float4 mountain0; // 有効、周波数、尾根、尖り
    float4 mountain1; // 方向（rad）、伸長、うねり、細部
    uint4 mountain2; // シード、水の場の UAV、流れの場の UAV、未使用
    // 水面（Liquid）の見た目。浅瀬の色 rgb, 色の変わる深さ（m。0 で深い所の色だけ）
    float4 liquid0;
    // 下地が透ける深さ（m。0 で透けない）, ハイト 0〜1 の全幅（m）, 未使用 x2
    float4 liquid1;
    LayerMaterialData layerMaterial;
};

ConstantBuffer<LayerConstants> g_layer : register(b1);

// 水の場の「Liquid が書いていない」値（CompositeWater.hlsl / MeshPbr.hlsl と揃える）。
static const float kWaterNone = -10000.0f;

// コンピュートシェーダでは暗黙の LOD が使えないため、出力テクセル 1 つが張る
// UV 幅からミップレベルを求めて SampleLevel する。
float TextureLod(Texture2D<float4> texture, float uvPerOutputTexel)
{
    uint width = 0;
    uint height = 0;
    uint mipCount = 0;
    texture.GetDimensions(0, width, height, mipCount);

    const float texelsPerOutputTexel = max(float(width) * uvPerOutputTexel, 1.0f);
    return clamp(log2(texelsPerOutputTexel), 0.0f, float(max(mipCount, 1u) - 1u));
}

float4 SampleLayerTexture(uint index, float2 uv, float uvPerOutputTexel)
{
    Texture2D<float4> texture = ResourceDescriptorHeap[index];
    return texture.SampleLevel(g_samplerLinearWrap, uv, TextureLod(texture, uvPerOutputTexel));
}

// スカラーのマップを 1 つ読む。指定されたチャンネルだけを取り出す。
float SampleLayerScalar(uint index, uint channelSlot, float2 uv, float uvPerOutputTexel)
{
    const float4 sampled = SampleLayerTexture(index, uv, uvPerOutputTexel);
    return SelectChannel(sampled, UnpackChannel(g_layer.mapChannels.x, channelSlot));
}

// このレイヤーが素材を引く UV。
//
// 通常は地形の UV に UV スケールを掛けたもの。**UV Path に Path を繋いだ Surface は、
// パスに沿った帯の座標で引く**（既定では進行方向の弧長が V、幅方向が U。
// pathUvIndices.z の bit0 が立っていれば入れ替えて進行方向を U に当てる）。進行方向は繰り返し長（m）
// ごとに 1 周するので、模様が進行方向にループする。帯の外は coverage が 0 になり、
// マスクに掛けて乗らないようにする。
struct LayerUv
{
    float2 uv;               // サンプルに使う UV
    float uvPerOutputTexel;  // 出力テクセル 1 つが張る UV 幅（ミップ選択用）
    bool path;               // パス UV か
    float2 xAxis;            // テクスチャの U 軸が向く、地形 UV 空間の単位ベクトル
    float2 yAxis;            // 同じく V 軸
    float coverage;          // 帯の内側なら 1、フェザーの外で 0
    float2 uvPerMeter;       // U / V それぞれの 1 m あたりの UV 幅（法線の勾配用）
};

LayerUv ComputeLayerUv(float2 outputUv, float2 texelSize, uint begin, uint end)
{
    LayerUv result;
    result.path = false;
    result.uv = outputUv * g_layer.blendParams.z;
    result.uvPerOutputTexel = texelSize.x * g_layer.blendParams.z;
    result.xAxis = float2(1.0f, 0.0f);
    result.yAxis = float2(0.0f, 1.0f);
    result.coverage = 1.0f;
    result.uvPerMeter = float2(0.0f, 0.0f);
    if (g_layer.pathUvIndices.x == kInvalidTextureIndex || g_layer.pathUvIndices.y == 0u)
    {
        return result;
    }
    ByteAddressBuffer segments = ResourceDescriptorHeap[g_layer.pathUvIndices.x];
    const float sizeMeters = max(g_layer.pathUvParams.w, 1e-3f);
    const PathFrame frame =
        ComputePathFrameRange(segments, begin, end, outputUv * sizeMeters, sizeMeters);
    const float repeatMeters = max(g_layer.pathUvParams.x, 1e-3f);
    const float widthRepeat = max(g_layer.pathUvParams.y, 1e-3f);
    const float widthMeters = max(frame.width, 1e-3f);
    result.path = true;
    // 縁のカーブ。Mask Path のガンマと同じ（1 より大きいと内側へ締まる）。
    result.coverage = pow(saturate(frame.coverage), max(g_layer.pathUvParams2.x, 1e-3f));
    // 点の強さ。Mask Path と同じく帯の値に掛ける（縁のカーブの後。強さはカーブで歪めない）。
    if ((g_layer.pathUvIndices.z & 2u) != 0u)
    {
        result.coverage *= frame.intensity;
    }
    // 幅方向は中心線で widthRepeat の半分（帯の幅にちょうど widthRepeat 枚が並ぶ）。
    const float acrossPerMeter = widthRepeat / widthMeters;
    const float alongPerMeter = 1.0f / repeatMeters;
    const float acrossUv = frame.across * acrossPerMeter + 0.5f * widthRepeat;
    const float alongUv = (frame.along + g_layer.pathUvParams.z) * alongPerMeter;
    // 幅方向の軸は進行方向の右手（direction を -90 度回した向き）。
    const float2 acrossAxis = float2(frame.direction.y, -frame.direction.x);
    if ((g_layer.pathUvIndices.z & 1u) != 0u)
    {
        // 進行方向をテクスチャの横（U）に当てる。
        result.uv = float2(alongUv, acrossUv);
        result.uvPerMeter = float2(alongPerMeter, acrossPerMeter);
        result.xAxis = frame.direction;
        result.yAxis = acrossAxis;
    }
    else
    {
        result.uv = float2(acrossUv, alongUv);
        result.uvPerMeter = float2(acrossPerMeter, alongPerMeter);
        result.xAxis = acrossAxis;
        result.yAxis = frame.direction;
    }
    const float texelMeters = texelSize.x * sizeMeters;
    result.uvPerOutputTexel = texelMeters * max(result.uvPerMeter.x, result.uvPerMeter.y);

    // マスク画像。素材と同じ帯の座標で、繰り返し長と幅方向の枚数だけ別に持って貼る。
    // 帯の覆い具合に掛けるので、Mask 入力とも掛け合わさる。
    if (g_layer.pathUvIndices.w != kInvalidTextureIndex)
    {
        const float maskAlongPerMeter = 1.0f / max(g_layer.pathUvParams2.y, 1e-3f);
        const float maskWidthRepeat = max(g_layer.pathUvParams2.z, 1e-3f);
        const float maskAcrossPerMeter = maskWidthRepeat / widthMeters;
        const float maskAcrossUv = frame.across * maskAcrossPerMeter + 0.5f * maskWidthRepeat;
        const float maskAlongUv = (frame.along + g_layer.pathUvParams.z) * maskAlongPerMeter;
        const float2 maskUv = ((g_layer.pathUvIndices.z & 1u) != 0u) ? float2(maskAlongUv, maskAcrossUv)
                                                                     : float2(maskAcrossUv, maskAlongUv);
        const float maskUvPerOutputTexel = texelMeters * max(maskAlongPerMeter, maskAcrossPerMeter);
        float pathMask = saturate(SampleLayerScalar(g_layer.pathUvIndices.w, TG_CHANNEL_SLOT_PATH_MASK,
                                                    maskUv, maskUvPerOutputTexel));
        if (g_layer.pathUvParams2.w != 0.0f)
        {
            pathMask = 1.0f - pathMask;
        }
        result.coverage *= pathMask;
    }
    return result;
}

// ハイトの基準面。ソースの値がこの値のとき、そのテクセルは基準の高さちょうどになる。
// ディスプレイスメントマップの「中間グレーが変位ゼロ」という慣習に合わせている。
// compositor::kHeightPivot と一致させること。
static const float kHeightPivot = 0.5f;

// シード付きの非周期勾配ノイズ。タイル境界ではなく出力全体の UV で評価する。
// 整数ハッシュを使い、シードを座標の巨大なずらし量にして精度を失わない。
float2 MountainGradient(int2 cell, uint seed)
{
    uint h = uint(cell.x) * 0x8da6b343u ^ uint(cell.y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    const float2 gradients[8] = {
        float2(1,0), float2(-1,0), float2(0,1), float2(0,-1),
        float2(0.70710678,0.70710678), float2(-0.70710678,0.70710678),
        float2(0.70710678,-0.70710678), float2(-0.70710678,-0.70710678)};
    return gradients[h & 7u];
}

float MountainNoise(float2 p, uint seed)
{
    int2 cell = int2(floor(p));
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    float a = dot(MountainGradient(cell, seed), f);
    float b = dot(MountainGradient(cell + int2(1,0), seed), f - float2(1,0));
    float c = dot(MountainGradient(cell + int2(0,1), seed), f - float2(0,1));
    float d = dot(MountainGradient(cell + int2(1,1), seed), f - float2(1,1));
    return clamp(lerp(lerp(a,b,u.x), lerp(c,d,u.x), u.y) * 1.41421356f, -1.0f, 1.0f);
}

float SampleMountain(float2 uv, float uvPerOutputTexel)
{
    float sn, cs;
    sincos(g_layer.mountain1.x, sn, cs);
    float2 q = (uv - 0.5f) * g_layer.mountain0.y;
    float2 p = float2(cs*q.x + sn*q.y, -sn*q.x + cs*q.y);
    p.x /= g_layer.mountain1.y;
    p += float2(17.31f, 29.73f);
    uint seed = g_layer.mountain2.x;
    p += g_layer.mountain1.z * float2(MountainNoise(p * 0.6f, seed + 101u),
                                                     MountainNoise(p * 0.6f, seed + 307u));
    float sum = 0.0f, weight = 0.0f, amplitude = 1.0f, frequency = 1.0f;
    float parent = 1.0f;
    // 細部を解像度に合わせて減衰し、縮小時のちらつきを抑える。
    for (int i = 0; i < 7; ++i)
    {
        float footprint = uvPerOutputTexel * g_layer.mountain0.y * frequency;
        float aa = 1.0f - smoothstep(0.2f, 0.5f, footprint);
        float n = MountainNoise(p * frequency, seed + uint(i) * 137u);
        float ridge = 1.0f - abs(n);
        ridge *= ridge;
        float h = lerp(n * 0.5f + 0.5f, ridge, g_layer.mountain0.z);
        sum += h * amplitude * parent * aa;
        weight += amplitude * aa;
        parent = lerp(1.0f, saturate(h * 2.0f), g_layer.mountain0.z);
        amplitude *= g_layer.mountain1.w;
        frequency *= 2.03f;
    }
    return pow(saturate(sum / max(weight, 1e-6f)), g_layer.mountain0.w);
}

// h = 基準の高さ + (ソースの値 - 基準面) * 起伏の強さ。
// 基準面を挟むことで、起伏の強さを変えても平均の高さが動かない。
float SampleLayerHeight(float2 uv, float uvPerOutputTexel)
{
    if (g_layer.mountain0.x > 0.5f) return SampleMountain(uv, uvPerOutputTexel);
    const float base = g_layer.surfaceParams.w;
    const float gain = g_layer.heightNoise.y;

    const uint source = uint(g_layer.blendParams.w);
    if (source == TG_SOURCE_NOISE)
    {
        const float noise = SampleNoise(g_layer.noiseTypes.x, uv, g_layer.heightNoise.x,
                                        g_layer.heightNoise.w, int(g_layer.heightNoise.z));
        return base + (noise - kHeightPivot) * gain;
    }
    if (source == TG_SOURCE_TEXTURE && g_layer.textureIndices1.y != kInvalidTextureIndex)
    {
        // シェイプのハイトマップは「タイルしない地形の 1 枚絵」前提なので
        // クランプで読む。wrap だと境界のバイリニア補間が反対側の端と混ざり、
        // 縁に壁や段差が出る。マテリアルのハイトマップはタイル素材なので wrap のまま。
        Texture2D<float4> texture = ResourceDescriptorHeap[g_layer.textureIndices1.y];
        const float lod = TextureLod(texture, uvPerOutputTexel);
        // サンプラは三項演算子で選べない（unique global resource の制約）ので分岐する。
        float4 sampled;
        if ((g_layer.flags & TG_FLAG_KIND_SHAPE) != 0u)
        {
            sampled = texture.SampleLevel(g_samplerLinearClamp, uv, lod);
        }
        else
        {
            sampled = texture.SampleLevel(g_samplerLinearWrap, uv, lod);
        }
        const float value =
            SelectChannel(sampled, UnpackChannel(g_layer.mapChannels.x, TG_CHANNEL_SLOT_HEIGHT));
        return base + (value - kHeightPivot) * gain;
    }

    // 定数。ソースの値がないので基準の高さそのもの。
    return base;
}

float SampleMaskSourceValue(float2 uv, float2 paintUv, float2 derivedUv, float uvPerOutputTexel)
{
    const uint source = uint(g_layer.maskParams.w);

    if (source == TG_SOURCE_PAINT)
    {
        if (g_layer.paintParams.x == kInvalidTextureIndex)
        {
            return g_layer.maskParams.x;
        }
        // ペイントマスクはレイヤーの UV スケールを掛けない出力そのものの座標で引く。
        // ブラシはメッシュ上で見えている位置に描くため、合成結果と 1 対 1 で対応する。
        Texture2D<float> paint = ResourceDescriptorHeap[g_layer.paintParams.x];
        return g_layer.maskParams.x *
               paint.SampleLevel(g_samplerLinearWrap, paintUv, 0.0f);
    }

    if (source == TG_SOURCE_NOISE)
    {
        const float noise = SampleNoise(g_layer.noiseTypes.y, uv, g_layer.maskNoise.x,
                                        g_layer.maskNoise.w, int(g_layer.maskNoise.z));
        // ノイズだけは加算。定数を基準に揺らす。
        return g_layer.maskParams.x + (noise - 0.5f) * g_layer.maskNoise.y;
    }

    if (source == TG_SOURCE_TEXTURE)
    {
        if (g_layer.textureIndices1.z == kInvalidTextureIndex)
        {
            return g_layer.maskParams.x;
        }
        return g_layer.maskParams.x * SampleLayerScalar(g_layer.textureIndices1.z,
                                                       TG_CHANNEL_SLOT_MASK, uv,
                                                       uvPerOutputTexel);
    }

    if (source >= TG_SOURCE_DERIVED)
    {
        if (g_layer.textureIndices1.w == kInvalidTextureIndex)
        {
            return g_layer.maskParams.x;
        }
        // テクセル参照ではなく UV で引く。合成パスは出力テクセルの中心を渡すので
        // 値は添字参照と一致し、サムネイルのように解像度が違う呼び出しでも使える。
        Texture2D<float> derived = ResourceDescriptorHeap[g_layer.textureIndices1.w];
        return g_layer.maskParams.x *
               derived.SampleLevel(g_samplerLinearClamp, derivedUv, 0.0f);
    }

    return g_layer.maskParams.x;
}

float SampleLayerMask(float2 uv, float2 paintUv, float2 derivedUv, float uvPerOutputTexel)
{
    float mask = saturate(SampleMaskSourceValue(uv, paintUv, derivedUv, uvPerOutputTexel));
    mask = ApplyMaskCurve(mask, g_layer.maskCurve.x);

    const bool invert = (g_layer.flags & TG_FLAG_MASK_INVERT) != 0u;
    return ApplyMaskLevels(mask, g_layer.maskParams.y, g_layer.maskParams.z, invert);
}

// ハイトの勾配からタンジェント空間法線を作る。解像度に依らない値になるよう、
// テクセル差ではなく UV 単位の微分を取る。
// 法線テクスチャが指定されている場合はそちらを使う。
//
// **勾配は実寸（m）で取る。** 強さのような無次元のつまみは持たない。
// ハイト 0〜1 の全幅が標高差（m）、出力 UV 0〜1 が地形の一辺（m）なので、
// blendParams.y = 標高差 / 一辺 を掛ければ d(高さ m) / d(距離 m) になる。
// UV スケールで模様を並べたぶんは同じだけ勾配が急になるので uvScale も掛ける。
//
// パス UV のときは帯の座標系（U = 幅方向、V = 進行方向）で法線を求めてから、
// 進行方向の回転で地形の UV 空間へ回す。回さないと、曲がった道の法線の陰影が
// 地形の X / Y に固定されたままになる。
float3 ComputeLayerNormal(LayerUv layerUv, float2 texelSize)
{
    const float2 uv = layerUv.uv;
    const float uvPerOutputTexel = layerUv.uvPerOutputTexel;
    float3 normal = float3(0.0f, 0.0f, 1.0f);
    if (g_layer.textureIndices0.y != kInvalidTextureIndex)
    {
        const float3 sampled =
            SampleLayerTexture(g_layer.textureIndices0.y, uv, uvPerOutputTexel).rgb;
        float3 tangentNormal = sampled * 2.0f - 1.0f;
        // **法線マップには 2 つの規約がある。**
        //   OpenGL : 緑 = 画像の上向き（−V）。Megascans などの既定
        //   DirectX: 緑 = 画像の下向き（+V）
        // このアプリの接空間と自前の法線は DirectX 規約なので、
        // OpenGL 規約のマップは緑を反転して読む（V 方向の陰影が逆になるため）。
        if ((g_layer.flags & TG_FLAG_FLIP_NORMAL_GREEN) != 0u)
        {
            tangentNormal.y = -tangentNormal.y;
        }
        normal = normalize(tangentNormal);
    }
    else if (g_layer.blendParams.y <= 0.0f)
    {
        // 標高差 0 なら地形は平ら。勾配を取るまでもない。
        normal = float3(0.0f, 0.0f, 1.0f);
    }
    else if (!layerUv.path)
    {
        const float heightPerSize = g_layer.blendParams.y;
        const float2 step = texelSize * g_layer.blendParams.z;
        const float hx0 = SampleLayerHeight(uv - float2(step.x, 0.0f), uvPerOutputTexel);
        const float hx1 = SampleLayerHeight(uv + float2(step.x, 0.0f), uvPerOutputTexel);
        const float hy0 = SampleLayerHeight(uv - float2(0.0f, step.y), uvPerOutputTexel);
        const float hy1 = SampleLayerHeight(uv + float2(0.0f, step.y), uvPerOutputTexel);

        // UV 単位の勾配（合成解像度に依らない）。
        const float dx = (hx1 - hx0) * 0.5f / max(step.x, 1e-6f);
        const float dy = (hy1 - hy0) * 0.5f / max(step.y, 1e-6f);

        // 実寸の勾配へ。tan(傾き) がそのまま法線の xy になる。
        const float scale = heightPerSize * g_layer.blendParams.z;
        normal = normalize(float3(-dx * scale, -dy * scale, 1.0f));
    }
    else
    {
        // 帯の座標系で、出力テクセル 1 つぶん（m）だけ離した所の高さから勾配を取る。
        const float sizeMeters = max(g_layer.pathUvParams.w, 1e-3f);
        const float heightMeters = g_layer.blendParams.y * sizeMeters;
        const float texelMeters = max(texelSize.x * sizeMeters, 1e-6f);
        const float2 step = layerUv.uvPerMeter * texelMeters;
        const float hx0 = SampleLayerHeight(uv - float2(step.x, 0.0f), uvPerOutputTexel);
        const float hx1 = SampleLayerHeight(uv + float2(step.x, 0.0f), uvPerOutputTexel);
        const float hy0 = SampleLayerHeight(uv - float2(0.0f, step.y), uvPerOutputTexel);
        const float hy1 = SampleLayerHeight(uv + float2(0.0f, step.y), uvPerOutputTexel);
        const float gx = (hx1 - hx0) * heightMeters * 0.5f / texelMeters;
        const float gy = (hy1 - hy0) * heightMeters * 0.5f / texelMeters;
        normal = normalize(float3(-gx, -gy, 1.0f));
    }

    if (layerUv.path)
    {
        // 帯の座標系から地形の UV 空間へ。テクスチャの U / V 軸が向く方向で写す。
        const float2 xy = normal.x * layerUv.xAxis + normal.y * layerUv.yAxis;
        normal = normalize(float3(xy, normal.z));
    }
    return normal;
}

struct SurfaceSample {
    float3 color;
    float3 normal;
    float3 surface;
    float height;
};
SurfaceSample EvaluateSurface(LayerUv layerUv, float2 outputUv, float2 texelSize) {
    const float2 uv = layerUv.uv;
    const float uvPerOutputTexel = layerUv.uvPerOutputTexel;
    // --- レイヤーの値 ------------------------------------------------------
    float3 layerBaseColor = g_layer.baseColor.rgb;
    float layerRoughness = g_layer.surfaceParams.x;
    float layerMetallic = g_layer.surfaceParams.y;
    float layerAo = g_layer.surfaceParams.z;

    if (g_layer.textureIndices0.x != kInvalidTextureIndex)
    {
        layerBaseColor *= SampleLayerTexture(g_layer.textureIndices0.x, uv, uvPerOutputTexel).rgb;
    }
    // 色相 / 彩度は**ティントを掛けたあと**に効かせる。順序を変えると、
    // 同じ設定でもティントの色に引きずられて結果が変わる。
    layerBaseColor =
        AdjustBaseColor(layerBaseColor, g_layer.colorAdjust.x, g_layer.colorAdjust.y,
                        g_layer.colorAdjust.z);
    if (g_layer.textureIndices0.z != kInvalidTextureIndex)
    {
        layerRoughness = SampleLayerScalar(g_layer.textureIndices0.z,
                                           TG_CHANNEL_SLOT_ROUGHNESS, uv, uvPerOutputTexel);
    }
    if (g_layer.textureIndices0.w != kInvalidTextureIndex)
    {
        layerMetallic = SampleLayerScalar(g_layer.textureIndices0.w, TG_CHANNEL_SLOT_METALLIC,
                                          uv, uvPerOutputTexel);
    }
    if (g_layer.textureIndices1.x != kInvalidTextureIndex)
    {
        layerAo = SampleLayerScalar(g_layer.textureIndices1.x, TG_CHANNEL_SLOT_AO, uv,
                                    uvPerOutputTexel);
    }

    float layerHeight = SampleLayerHeight(uv, uvPerOutputTexel);
    float3 layerNormal = ComputeLayerNormal(layerUv, texelSize);
    if (g_layer.layerMaterial.count > 0) {
        const float sizeMeters = max(g_layer.pathUvParams.w, 0.001f);
        // UV一周を1mとしてSurfaceのスケールを掛ける。worldUvは配置側の拡縮に依存しない。
        const LayerMaterialSample material = EvaluateLayerMaterial(g_layer.layerMaterial, uv, (outputUv - 0.5f) * sizeMeters, float2(uvPerOutputTexel, texelSize.x * sizeMeters), layerUv.path ? layerUv.uvPerMeter : g_layer.blendParams.zz / sizeMeters, layerUv.xAxis, layerUv.yAxis);
        layerBaseColor = material.color; layerRoughness = material.surface.x; layerMetallic = material.surface.y; layerAo = material.surface.z;
        layerHeight = g_layer.surfaceParams.w + (material.height - 0.5f) * g_layer.layerMaterial.displacementMeters / max(g_layer.blendParams.y * sizeMeters, 0.001f);
        layerNormal = material.normal;
        if (layerUv.path) layerNormal = normalize(float3(layerNormal.x * layerUv.xAxis + layerNormal.y * layerUv.yAxis, layerNormal.z));
    }

    SurfaceSample result;
    result.color = layerBaseColor; result.normal = layerNormal;
    result.surface = float3(layerRoughness, layerMetallic, layerAo); result.height = layerHeight;
    return result;
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_layer.tile.z || dispatchThreadId.y >= g_layer.tile.w)
    {
        return;
    }

    const uint2 texel = g_layer.tile.xy + dispatchThreadId.xy;

    RWTexture2D<float4> baseColorTarget = ResourceDescriptorHeap[g_layer.outputIndices.x];
    RWTexture2D<float2> normalTarget    = ResourceDescriptorHeap[g_layer.outputIndices.y];
    RWTexture2D<float4> surfaceTarget   = ResourceDescriptorHeap[g_layer.outputIndices.z];
    RWTexture2D<float>  heightTarget    = ResourceDescriptorHeap[g_layer.outputIndices.w];

    const float2 texelSize = 1.0f / float2(g_layer.resolution);
    // ペイントマスクは出力そのものの座標で引くため、UV スケールを掛ける前を残しておく。
    const float2 outputUv = (float2(texel) + 0.5f) * texelSize;
    const LayerUv layerUv = ComputeLayerUv(outputUv, texelSize, 0, g_layer.pathUvIndices.y);
    const float2 uv = layerUv.uv;
    // 出力テクセル 1 つが張る UV 幅。テクスチャのミップ選択に使う。
    const float uvPerOutputTexel = layerUv.uvPerOutputTexel;

    SurfaceSample sample = EvaluateSurface(layerUv, outputUv, texelSize);
    float layerMask = SampleLayerMask(uv, outputUv, outputUv, uvPerOutputTexel) * layerUv.coverage;
    if (layerUv.path) {
        // 各鎖は自身のUVで評価する。覆いを足して濃くせず、最大値を帯全体の覆いに使う。
        ByteAddressBuffer segments = ResourceDescriptorHeap[g_layer.pathUvIndices.x];
        const float sizeMeters = max(g_layer.pathUvParams.w, 1e-3f);
        float total = 0;
        layerMask = 0;
        uint begin = 0;
        [loop] while (begin < g_layer.pathUvIndices.y) {
            const uint end = PathStrandEnd(segments, begin, g_layer.pathUvIndices.y, sizeMeters);
            const LayerUv strandUv = ComputeLayerUv(outputUv, texelSize, begin, end);
            const float influence = strandUv.coverage * SampleLayerMask(strandUv.uv, outputUv, outputUv, strandUv.uvPerOutputTexel);
            if (influence > 0) {
                const SurfaceSample strand = EvaluateSurface(strandUv, outputUv, texelSize);
                const float weight = influence / (total + influence);
                sample.color = lerp(sample.color, strand.color, weight);
                sample.surface = lerp(sample.surface, strand.surface, weight);
                sample.height = lerp(sample.height, strand.height, weight);
                sample.normal = ReorientNormal(FlattenNormal(sample.normal, 1 - weight), FlattenNormal(strand.normal, weight));
                total += influence;
                layerMask = max(layerMask, influence);
            }
            begin = end;
        }
    }
    float3 layerBaseColor = sample.color;
    float3 layerNormal = sample.normal;
    float layerRoughness = sample.surface.x, layerMetallic = sample.surface.y, layerAo = sample.surface.z;
    float layerHeight = sample.height;

    const bool isBaseLayer = (g_layer.flags & TG_FLAG_BASE_LAYER) != 0u;
    const bool isShape = (g_layer.flags & TG_FLAG_KIND_SHAPE) != 0u;
    const bool isLiquid = (g_layer.flags & TG_FLAG_KIND_LIQUID) != 0u;
    // 下地に沿わせるのは合成相手がいるときだけ。一番下では意味を持たない。
    const bool isWrap = (g_layer.flags & TG_FLAG_WRAP) != 0u && !isBaseLayer;

    float weight = 1.0f;
    // 水越しに下地の色が見える割合（Liquid だけ。色にだけ効き、法線・ラフネス・高さは水面のまま）。
    float bedVisibility = 0.0f;
    // 水の場へ書く符号付きの水深（m。水の中が正、陸が負）。Liquid がマスクの中にだけ書く。
    float signedDepthMeters = 0.0f;
    bool writesWaterDepth = false;
    if (!isBaseLayer)
    {
        // パス UV のときは帯の外に乗らない（coverage が 0）。
        const float mask = layerMask;
        const float destinationHeight = heightTarget[texel];
        if (isWrap)
        {
            // 自分の高さを「下地 + 相対的な起伏」へ読み替える。以降は通常の競合に
            // 流れるが、勝った所の高さも下地基準なので大きな形が保たれる。
            // 基準の高さの 0.5 からのずれは、そのまま被せ物の厚みになる。
            layerHeight = destinationHeight + (layerHeight - kHeightPivot);
        }
        if (isShape)
        {
            // シェイプは競合せず加算する。マスクは加算量の係数。
            weight = mask;
        }
        else if (isLiquid)
        {
            // リキッドは「水位 − 下地の高さ」だけで勝敗を決める。
            // HeightBlendWeight を通すと汀線の遷移帯が下地を水平面へ引っ張り、
            // 水位を動かすたびに地形が変形してしまう。
            // フェザー（blendParams.x）は汀線を柔らかくする幅で、
            // 水面下では厳密に 1、水面上では厳密に 0 になる。
            const float depth = g_layer.surfaceParams.w - destinationHeight;
            weight = mask * smoothstep(0.0f, max(g_layer.blendParams.x, 1e-4f), depth);

            // 水深（m）で色を決める。浅瀬の色から深い所の色（ベースカラー）へ指数で寄せ、
            // 浅い所では下地の色を残す（Beer–Lambert の減衰。深さの値は 1/e になる深さ）。
            const float depthMeters = max(depth, 0.0f) * g_layer.liquid1.y;
            signedDepthMeters = clamp(depth * g_layer.liquid1.y, -9000.0f, 9000.0f);
            writesWaterDepth = mask > 0.5f;
            if (g_layer.liquid0.w > 0.0f)
            {
                layerBaseColor = lerp(layerBaseColor, g_layer.liquid0.rgb, exp(-depthMeters / g_layer.liquid0.w));
            }
            if (g_layer.liquid1.x > 0.0f)
            {
                bedVisibility = exp(-depthMeters / g_layer.liquid1.x);
            }
        }
        else
        {
            weight = HeightBlendWeight(destinationHeight, layerHeight, mask,
                                       g_layer.blendParams.x);
        }
    }

    // --- 各チャンネルへ積む ------------------------------------------------
    if ((g_layer.channelMask & 0x1u) != 0u)
    {
        const float3 destination = isBaseLayer ? layerBaseColor : baseColorTarget[texel].rgb;
        baseColorTarget[texel] = float4(lerp(destination, layerBaseColor, weight * (1.0f - bedVisibility)), 1.0f);
    }

    if ((g_layer.channelMask & 0x2u) != 0u)
    {
        float3 result;
        if (isBaseLayer)
        {
            result = layerNormal;
        }
        else if (isShape || isWrap)
        {
            // シェイプは高さを加算し、沿わせたサーフェスは下地の形を保つ。
            // どちらも下地の大きな法線が生きているべきなので、
            // 平坦化せずに RNM で重ねるだけにする。
            const float3 destination = DecodeTangentNormal(normalTarget[texel]);
            result = ReorientNormal(destination, FlattenNormal(layerNormal, weight));
        }
        else
        {
            // 重みに応じて下地を平坦へ寄せ、レイヤー側も弱めてから RNM で合成する。
            // weight = 0 で下地、weight = 1 でレイヤーそのものになる。
            const float3 destination = DecodeTangentNormal(normalTarget[texel]);
            const float3 flattenedBase = FlattenNormal(destination, 1.0f - weight);
            const float3 attenuatedDetail = FlattenNormal(layerNormal, weight);
            result = ReorientNormal(flattenedBase, attenuatedDetail);
        }
        normalTarget[texel] = EncodeTangentNormal(result);
    }

    if ((g_layer.channelMask & 0x4u) != 0u)
    {
        const float3 layerSurface = float3(layerRoughness, layerMetallic, layerAo);
        const float4 destinationSurface = surfaceTarget[texel];
        const float3 destination = isBaseLayer ? layerSurface : destinationSurface.rgb;
        // アルファは **1 − 水面の被覆**（地形の描画が波を重ねる範囲）。既定の 1 が「水なし」なので、
        // アルファを 1 で書くほかのパスは水を消す側に倒れる。Liquid は 0 へ、上に重なるほかのレイヤーは 1 へ寄せる。
        const float dry = isBaseLayer ? 1.0f : lerp(destinationSurface.a, isLiquid ? 0.0f : 1.0f, weight);
        surfaceTarget[texel] = float4(lerp(destination, layerSurface, weight), dry);
    }

    if ((g_layer.channelMask & 0x8u) != 0u)
    {
        if (isShape)
        {
            // 加算。基準面（0.5）からの振れだけを下地へ足すので、
            // 下のレイヤーの細部がそのまま残る。
            // 高さ競合・高さ由来マスク・水位・PNG 書き出しは 0〜1 を前提に
            // しているので、加算後は必ず切り詰める。
            const float destination = isBaseLayer ? kHeightPivot : heightTarget[texel];
            heightTarget[texel] = saturate(destination + (layerHeight - kHeightPivot) * weight);
        }
        else
        {
            const float destination = isBaseLayer ? layerHeight : heightTarget[texel];
            float result = lerp(destination, layerHeight, weight);
            // 沿わせると下地 + 厚みで 1 を超えうる。シェイプの加算と同じく切り詰める。
            if (isWrap)
            {
                result = saturate(result);
            }
            heightTarget[texel] = result;
        }
    }

    // --- 水の場 -----------------------------------------------------------------
    // x = 水際からの符号付き距離（CompositeWater.hlsl が後から書く）、y = 符号付きの水深（m）。
    // 一番下のレイヤーが「水なし」で埋め、Liquid がマスクの中に水深を書く（陸の側も。寄せる波が
    // 水位より少し高い所まで這い上がるのに使う）。
    RWTexture2D<float2> waterTarget = ResourceDescriptorHeap[g_layer.mountain2.y];
    if (isBaseLayer)
    {
        waterTarget[texel] = float2(kWaterNone, kWaterNone);
        // 流れの場（River が書く）も「流れなし」で埋める。
        RWTexture2D<float4> flowTarget = ResourceDescriptorHeap[g_layer.mountain2.z];
        flowTarget[texel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    else if (writesWaterDepth)
    {
        waterTarget[texel] = float2(waterTarget[texel].x, signedDepthMeters);
    }
}

