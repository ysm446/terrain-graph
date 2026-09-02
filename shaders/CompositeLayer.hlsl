// レイヤー 1 枚ぶんを合成結果へ積む。
//
// 出力の 4 枚は UAV として読み書きする。各スレッドは自分のテクセルしか触らないので、
// 同一ディスパッチ内での読み書きは安全。レイヤー間は UAV バリアで区切る。
//
// 出力タイル矩形と解像度を引数に取る形は崩さないこと（エクスポート時のタイル評価に必要）。

#include "CompositeCommon.hlsli"

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
};

ConstantBuffer<LayerConstants> g_layer : register(b1);

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

// ハイトの基準面。ソースの値がこの値のとき、そのテクセルは基準の高さちょうどになる。
// ディスプレイスメントマップの「中間グレーが変位ゼロ」という慣習に合わせている。
// compositor::kHeightPivot と一致させること。
static const float kHeightPivot = 0.5f;

// h = 基準の高さ + (ソースの値 - 基準面) * 起伏の強さ。
// 基準面を挟むことで、起伏の強さを変えても平均の高さが動かない。
float SampleLayerHeight(float2 uv, float uvPerOutputTexel)
{
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
float3 ComputeLayerNormal(float2 uv, float2 texelSize, float uvPerOutputTexel)
{
    if (g_layer.textureIndices0.y != kInvalidTextureIndex)
    {
        const float3 sampled =
            SampleLayerTexture(g_layer.textureIndices0.y, uv, uvPerOutputTexel).rgb;
        return normalize(sampled * 2.0f - 1.0f);
    }

    // 標高差 0 なら地形は平ら。勾配を取るまでもない。
    const float heightPerSize = g_layer.blendParams.y;
    if (heightPerSize <= 0.0f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }

    const float hx0 = SampleLayerHeight(uv - float2(texelSize.x, 0.0f), uvPerOutputTexel);
    const float hx1 = SampleLayerHeight(uv + float2(texelSize.x, 0.0f), uvPerOutputTexel);
    const float hy0 = SampleLayerHeight(uv - float2(0.0f, texelSize.y), uvPerOutputTexel);
    const float hy1 = SampleLayerHeight(uv + float2(0.0f, texelSize.y), uvPerOutputTexel);

    // UV 単位の勾配（合成解像度に依らない）。
    const float dx = (hx1 - hx0) * 0.5f / max(texelSize.x, 1e-6f);
    const float dy = (hy1 - hy0) * 0.5f / max(texelSize.y, 1e-6f);

    // 実寸の勾配へ。tan(傾き) がそのまま法線の xy になる。
    const float scale = heightPerSize * g_layer.blendParams.z;
    return normalize(float3(-dx * scale, -dy * scale, 1.0f));
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
    const float2 uv = outputUv * g_layer.blendParams.z;
    const float2 noiseTexelSize = texelSize * g_layer.blendParams.z;

    // 出力テクセル 1 つが張る UV 幅。テクスチャのミップ選択に使う。
    const float uvPerOutputTexel = texelSize.x * g_layer.blendParams.z;

    // --- レイヤーの値 ------------------------------------------------------
    float3 layerBaseColor = g_layer.baseColor.rgb;
    float layerRoughness = g_layer.surfaceParams.x;
    float layerMetallic = g_layer.surfaceParams.y;
    float layerAo = g_layer.surfaceParams.z;

    if (g_layer.textureIndices0.x != kInvalidTextureIndex)
    {
        layerBaseColor *= SampleLayerTexture(g_layer.textureIndices0.x, uv, uvPerOutputTexel).rgb;
    }
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
    const float3 layerNormal = ComputeLayerNormal(uv, noiseTexelSize, uvPerOutputTexel);

    const bool isBaseLayer = (g_layer.flags & TG_FLAG_BASE_LAYER) != 0u;
    const bool isShape = (g_layer.flags & TG_FLAG_KIND_SHAPE) != 0u;
    const bool isLiquid = (g_layer.flags & TG_FLAG_KIND_LIQUID) != 0u;
    // 下地に沿わせるのは合成相手がいるときだけ。一番下では意味を持たない。
    const bool isWrap = (g_layer.flags & TG_FLAG_WRAP) != 0u && !isBaseLayer;

    float weight = 1.0f;
    if (!isBaseLayer)
    {
        const float mask = SampleLayerMask(uv, outputUv, outputUv, uvPerOutputTexel);
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
        baseColorTarget[texel] = float4(lerp(destination, layerBaseColor, weight), 1.0f);
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
        const float3 destination = isBaseLayer ? layerSurface : surfaceTarget[texel].rgb;
        surfaceTarget[texel] = float4(lerp(destination, layerSurface, weight), 1.0f);
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
}

// --- マスクのサムネイル ---------------------------------------------------
//
// レイヤー一覧に出す小さなマスク画像を作る。合成パスと同じ LayerConstants を
// そのまま使い、次の 3 つだけ差し替えて呼ぶ。
//
//   outputIndices.x : サムネイルの UAV
//   resolution      : サムネイルの一辺
//   tile            : (0, 0, 一辺, 一辺)
//
// 中間結果由来のマスクはこのレイヤーを合成する直前の下地から作られるため、
// 合成ループの中、そのレイヤーの順番で呼ぶこと。

// 1 テクセルあたりの格子の一辺。合成解像度との差を埋めるための平均化に使う。
//
// 1 点だけ拾うと、傾斜や窪みのマスクのように細かい模様が砂嵐になる。
// 16 点（4 x 4）で平均はほぼ収束する（8 x 8 にしても見た目は変わらない）。
#define TG_MASK_THUMBNAIL_TAPS 4

[numthreads(8, 8, 1)]
void CsMaskThumbnail(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_layer.tile.z || dispatchThreadId.y >= g_layer.tile.w)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_layer.outputIndices.x];

    // 一番下のレイヤーは下地なのでマスクが効かない。一覧でも「全面」と示す。
    const bool isBaseLayer = (g_layer.flags & TG_FLAG_BASE_LAYER) != 0u;
    if (isBaseLayer)
    {
        output[dispatchThreadId.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    const float2 texelSize = 1.0f / float2(g_layer.resolution);
    const float uvPerOutputTexel = texelSize.x * g_layer.blendParams.z;

    // **1 テクセルにつき 1 回だけ評価すると使いものにならない。**
    // 傾斜や窪みのマスクは合成解像度そのままの細かさを持つので、
    // 数十テクセルに 1 点だけ拾うと砂嵐にしか見えない。テクセルが覆う範囲を
    // 格子状に取って平均する。カーブとレベルを掛けたあとの値を平均するので、
    // 一覧に出るのは「そのあたりがどれだけ覆われるか」になる。
    float sum = 0.0f;
    for (int y = 0; y < TG_MASK_THUMBNAIL_TAPS; ++y)
    {
        for (int x = 0; x < TG_MASK_THUMBNAIL_TAPS; ++x)
        {
            const float2 offset =
                (float2(x, y) + 0.5f) / float(TG_MASK_THUMBNAIL_TAPS);
            const float2 outputUv = (float2(dispatchThreadId.xy) + offset) * texelSize;
            sum += SampleLayerMask(outputUv * g_layer.blendParams.z, outputUv, outputUv,
                                   uvPerOutputTexel);
        }
    }

    const float mask = sum / float(TG_MASK_THUMBNAIL_TAPS * TG_MASK_THUMBNAIL_TAPS);
    output[dispatchThreadId.xy] = float4(mask, mask, mask, 1.0f);
}
