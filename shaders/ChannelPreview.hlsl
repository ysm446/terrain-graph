// 「チャンネル」パネルの表示用テクスチャを作る。
//
// 合成結果のチャンネル（ベースカラー / 法線 / サーフェス / ハイト / 水チャンネル）を、真上から見た
// 1 枚の絵として RGBA8 へ焼く。数値のチャンネルは見やすい色へ直す。ImGui はテクスチャの値を
// そのままバックバッファへ書くので、ここで sRGB へ直しておく（TexturePreview.hlsl と同じ理由）。
//
// 拡大・パンは UV の範囲（uvRect）で受ける。出力はいつも表示の大きさなので、寄ってもぼけない。
// カーソルの位置の元の値（表示用に直す前）も、結果のバッファへ 1 つ書く。

#include "Common.hlsli"
#include "LayerWeights.hlsli"

// C++ 側（ApplicationChannelPreview.cpp）の ChannelMode と揃える。
#define TG_CHANNEL_BASECOLOR      0
#define TG_CHANNEL_NORMAL         1
#define TG_CHANNEL_ROUGHNESS      2
#define TG_CHANNEL_METALLIC       3
#define TG_CHANNEL_AO             4
#define TG_CHANNEL_HEIGHT         5
#define TG_CHANNEL_WATER_COVER    6
#define TG_CHANNEL_WATER_DEPTH    7
#define TG_CHANNEL_WATER_DISTANCE 8
#define TG_CHANNEL_WATER_WAVE     9
#define TG_CHANNEL_FLOW           10
#define TG_CHANNEL_RAPIDS         11
#define TG_CHANNEL_LAYERS         12

struct ChannelPreviewConstants
{
    uint sourceIndex;  // 見るチャンネルのテクスチャの SRV
    uint outputIndex;  // 表示用テクスチャの UAV
    uint width;        // 出力の大きさ
    uint height;

    uint mode;         // TG_CHANNEL_*
    uint resultIndex;  // カーソルの位置の値を書くバッファの UAV（float 4 つ）
    uint probeValid;   // カーソルが絵の上にあるか
    uint pad;

    float4 uvRect;     // 表示する UV の範囲（min.xy, max.xy）
    float2 probeUv;    // カーソルの位置の UV
    float rangeA;      // 表示の目盛り（水深: 最大の深さ m、距離: 等値線の間隔 m、流れ: 最大の速さ m/s）
    float rangeB;      // ハイト: 0 / 1 にあたる値（未使用のモードでは 0）
};

ConstantBuffer<ChannelPreviewConstants> g_constants : register(b0);

// 水の場の「水なし」（CompositeLayer.hlsl と揃える）。
static const float kWaterNone = -10000.0f;

float3 HueToRgb(float hue)
{
    const float3 k = float3(1.0f, 2.0f / 3.0f, 1.0f / 3.0f);
    return saturate(abs(frac(hue + k) * 6.0f - 3.0f) - 1.0f);
}

// Surface の ID ごとの色（黄金角で色相を回す。0 = Surface なしは暗い灰）。
float3 LayerIdColor(uint id)
{
    if (id == 0u)
    {
        return float3(0.03f, 0.03f, 0.03f);
    }
    const float3 hue = HueToRgb(frac(float(id) * 0.61803399f));
    // 隣り合う番号で明るさも変える（色相だけだと近い色が出る）。
    return hue * ((id % 2u == 0u) ? 0.85f : 0.45f) + 0.04f;
}

// Surface ごとの重み（R32_UINT）を、ID の色を重みで混ぜた色にする。点で読む（ID は補間できない）。
float3 LayerWeightsColor(uint packed)
{
    const LayerWeights layers = DecodeLayerWeights(packed);
    return LayerIdColor(layers.ids.x) * layers.weights.x + LayerIdColor(layers.ids.y) * layers.weights.y +
           LayerIdColor(layers.ids.z) * layers.weights.z;
}

// 値をそのまま表示の明るさにするモード（法線マップの色や 0〜1 のスカラー）。
// sRGB へ直さずに書くので、ChannelColor の戻り値は表示値そのもの。
bool IsRawValueMode(uint mode)
{
    return mode == TG_CHANNEL_NORMAL || mode == TG_CHANNEL_ROUGHNESS || mode == TG_CHANNEL_METALLIC ||
           mode == TG_CHANNEL_AO || mode == TG_CHANNEL_HEIGHT || mode == TG_CHANNEL_WATER_COVER ||
           mode == TG_CHANNEL_WATER_WAVE || mode == TG_CHANNEL_RAPIDS;
}

// 表示色（リニア。IsRawValueMode のモードは表示値そのもの）。
float3 ChannelColor(float4 value)
{
    const uint mode = g_constants.mode;
    if (mode == TG_CHANNEL_BASECOLOR)
    {
        return value.rgb;
    }
    if (mode == TG_CHANNEL_NORMAL)
    {
        // タンジェント空間法線（xy のみ。z は再構成）。法線マップの見慣れた色で出す。
        const float2 xy = value.xy;
        const float z = sqrt(saturate(1.0f - dot(xy, xy)));
        return float3(xy, z) * 0.5f + 0.5f;
    }
    if (mode == TG_CHANNEL_ROUGHNESS) { return value.rrr; }
    if (mode == TG_CHANNEL_METALLIC) { return value.ggg; }
    if (mode == TG_CHANNEL_AO) { return value.bbb; }
    if (mode == TG_CHANNEL_HEIGHT) { return saturate(value.rrr); }
    if (mode == TG_CHANNEL_WATER_COVER) { return saturate(value.zzz); }
    if (mode == TG_CHANNEL_WATER_WAVE) { return saturate(value.zzz); }
    if (mode == TG_CHANNEL_RAPIDS) { return saturate(value.www); }
    if (mode == TG_CHANNEL_WATER_DEPTH)
    {
        // 水の中は深いほど濃い青、陸は灰（水位に近いほど明るい）、水の場に何も無ければ黒。
        const float depth = value.y;
        if (depth < kWaterNone * 0.5f) { return float3(0.0f, 0.0f, 0.0f); }
        if (depth > 0.0f)
        {
            const float t = saturate(depth / max(g_constants.rangeA, 1e-3f));
            return lerp(float3(0.35f, 0.75f, 0.85f), float3(0.0f, 0.03f, 0.25f), sqrt(t));
        }
        const float land = saturate(-depth / max(g_constants.rangeA, 1e-3f));
        return lerp(float3(0.30f, 0.27f, 0.20f), float3(0.03f, 0.03f, 0.03f), sqrt(land));
    }
    if (mode == TG_CHANNEL_WATER_DISTANCE)
    {
        // 水の中は青、陸は橙。水際から離れるほど暗く、等値線を一定の間隔で引く。
        const float distance = value.x;
        if (value.y < kWaterNone * 0.5f) { return float3(0.0f, 0.0f, 0.0f); }
        const float spacing = max(g_constants.rangeA, 1e-3f);
        const float away = abs(distance);
        const float fade = exp(-away / (spacing * 6.0f));
        const float3 tint = (distance > 0.0f) ? float3(0.10f, 0.35f, 0.80f) : float3(0.80f, 0.40f, 0.10f);
        const float cycle = frac(away / spacing);
        const float lineValue = smoothstep(0.0f, 0.06f, cycle) * smoothstep(1.0f, 0.94f, cycle);
        return tint * (0.15f + 0.85f * fade) * lerp(0.45f, 1.0f, lineValue);
    }
    if (mode == TG_CHANNEL_FLOW)
    {
        // 向きを色相、速さを明るさ。流れの無い所は黒。
        const float2 velocity = value.xy;
        const float speed = length(velocity);
        if (speed < 1e-4f) { return float3(0.0f, 0.0f, 0.0f); }
        const float hue = atan2(velocity.y, velocity.x) / (2.0f * kPi) + 0.5f;
        return HueToRgb(hue) * saturate(speed / max(g_constants.rangeA, 1e-3f));
    }
    return float3(1.0f, 0.0f, 1.0f);
}

[numthreads(8, 8, 1)]
void CsMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_constants.width || id.y >= g_constants.height)
    {
        return;
    }
    Texture2D<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const float2 t = (float2(id.xy) + 0.5f) / float2(g_constants.width, g_constants.height);
    const float2 uv = lerp(g_constants.uvRect.xy, g_constants.uvRect.zw, t);
    float3 color = float3(0.0f, 0.0f, 0.0f);
    const bool layersMode = (g_constants.mode == TG_CHANNEL_LAYERS);
    if (layersMode)
    {
        if (all(uv >= 0.0f) && all(uv <= 1.0f))
        {
            Texture2D<uint> layerSource = ResourceDescriptorHeap[g_constants.sourceIndex];
            uint layerWidth, layerHeight;
            layerSource.GetDimensions(layerWidth, layerHeight);
            const int2 texel = min(int2(uv * float2(layerWidth, layerHeight)), int2(layerWidth, layerHeight) - 1);
            color = LayerWeightsColor(layerSource.Load(int3(texel, 0)));
        }
    }
    else if (all(uv >= 0.0f) && all(uv <= 1.0f))
    {
        // 流れと距離は補間すると色が濁る（向きが平均されて消える）ので、点で読む。
        const bool nearest = (g_constants.mode == TG_CHANNEL_FLOW) ||
                           (g_constants.mode == TG_CHANNEL_WATER_DEPTH) ||
                           (g_constants.mode == TG_CHANNEL_WATER_DISTANCE);
        const float4 value = nearest ? source.SampleLevel(g_samplerPointClamp, uv, 0.0f)
                                   : source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
        color = ChannelColor(value);
    }
    // 値をそのまま見せるモードは sRGB へ直さない（直して戻すだけの往復になる）。
    const float3 shown = saturate(color);
    output[id.xy] = float4(IsRawValueMode(g_constants.mode) ? shown : LinearToSrgb(shown), 1.0f);

    // カーソルの位置の元の値。1 スレッドだけが書く。
    if (id.x == 0u && id.y == 0u)
    {
        RWStructuredBuffer<float> result = ResourceDescriptorHeap[g_constants.resultIndex];
        float4 value = float4(0.0f, 0.0f, 0.0f, 0.0f);
        if (g_constants.probeValid != 0u && layersMode)
        {
            // 上位 2 つの ID と重みを返す（ID, 重み, ID, 重み）。
            Texture2D<uint> layerSource = ResourceDescriptorHeap[g_constants.sourceIndex];
            uint layerWidth, layerHeight;
            layerSource.GetDimensions(layerWidth, layerHeight);
            const int2 texel = min(int2(saturate(g_constants.probeUv) * float2(layerWidth, layerHeight)),
                                   int2(layerWidth, layerHeight) - 1);
            const LayerWeights layers = DecodeLayerWeights(layerSource.Load(int3(texel, 0)));
            value = float4(float(layers.ids.x), layers.weights.x, float(layers.ids.y), layers.weights.y);
        }
        else if (g_constants.probeValid != 0u)
        {
            value = source.SampleLevel(g_samplerPointClamp, g_constants.probeUv, 0.0f);
        }
        result[0] = value.x;
        result[1] = value.y;
        result[2] = value.z;
        result[3] = value.w;
    }
}
