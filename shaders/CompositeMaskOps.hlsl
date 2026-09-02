// マスクのノードグラフを 1 枚ずつ焼くパス。
//
// グラフのマスクは `compositor::MaskProgram`（op の列）へ落ちていて、
// 評価器が入力の順に 1 op ずつここへ流す。結果は op ごとのテクスチャに残るので、
// 合流（Mask Blend）や、同じマスクを 2 か所で使うことができる。
//
// 川筋（Fluvial）だけは反復が要るので別（CompositeFluvial.hlsl）。

#include "CompositeCommon.hlsli"

// compositor::MaskBlendMode と一致させること。
#define TG_BLEND_ADD      0
#define TG_BLEND_MULTIPLY 1
#define TG_BLEND_MIN      2
#define TG_BLEND_MAX      3

struct MaskOpConstants
{
    // x: 出力 UAV、y: 入力 A の SRV、z: 入力 B の SRV、
    // w: 素材の SRV（Image は画像、Slope は合成の Height）
    uint4 indices;
    // x: 出力の一辺、y: 読むチャンネル（Image）/ 種類（Noise）、
    // z: 反転 / ブレンドの種類、w: オクターブ（Noise）
    uint4 params0;
    // Slope : 最小角（度）, 最大角（度）, ガンマ, 実寸比（標高差 / 一辺）
    // Levels: 黒点, 白点, ガンマ, 未使用
    // Blend : 強さ, 未使用 x3
    // Noise : 周波数, 量, オフセット, 未使用
    float4 params1;
    // x: 傾斜を測る距離（テクセル）、yzw: 未使用
    float4 params2;
};

ConstantBuffer<MaskOpConstants> g_op : register(b1);

uint MaskResolution() { return g_op.params0.x; }

bool OutsideMask(uint2 texel)
{
    const uint resolution = MaskResolution();
    return texel.x >= resolution || texel.y >= resolution;
}

float2 MaskUv(uint2 texel)
{
    return (float2(texel) + 0.5f) / float(MaskResolution());
}

// 反転はどの op でも最後に掛ける。
float ApplyInvert(float value)
{
    return (g_op.params0.z != 0u) ? (1.0f - value) : value;
}

float SampleMaskInput(uint index, float2 uv)
{
    Texture2D<float> mask = ResourceDescriptorHeap[index];
    return mask.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

// ノイズ 1 枚をマスクにする。**入力を持たないマスクのソース。**
//
// 使うのは合成レイヤーと同じ `SampleNoise`（Common.hlsli）。周波数は整数へ
// 丸めて評価されるので、出力は必ずタイルする。
[numthreads(8, 8, 1)]
void CsNoise(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    const float noise = SampleNoise(g_op.params0.y, MaskUv(texel), g_op.params1.x,
                                    g_op.params1.z, int(g_op.params0.w));
    // 量は 0.5 を中心にした振れ幅。1 でノイズそのもの、0 で一様な 0.5 になる。
    const float value = saturate(0.5f + (noise - 0.5f) * g_op.params1.y);
    output[texel] = ApplyInvert(value);
}

// 画像 1 枚をマスクにする。**タイルしない 1 枚絵**として等倍で貼る
// （素材のように並べたいときはレイヤー側のマスクを使う）。
[numthreads(8, 8, 1)]
void CsImage(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    Texture2D<float4> source = ResourceDescriptorHeap[g_op.indices.w];

    const float4 sampled = source.SampleLevel(g_samplerLinearClamp, MaskUv(texel), 0.0f);
    output[texel] = saturate(ApplyInvert(SelectChannel(sampled, g_op.params0.y)));
}

// 下地の傾斜。**角度（度）で切る。**
//
// terrain-editor は解析用ハイトをぼかしてから勾配を取るが、こちらは
// **勾配を測る距離**（最大ディテール）で代える。1 テクセル差ではなく
// その距離ぶん離れた点との差を見るので、小さな凹凸は自然に均される。
[numthreads(8, 8, 1)]
void CsSlope(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    Texture2D<float> height = ResourceDescriptorHeap[g_op.indices.w];

    const float resolution = float(MaskResolution());
    const float2 uv = MaskUv(texel);
    const float span = max(g_op.params2.x, 1.0f);   // 勾配を測る距離（テクセル）
    const float2 step = float2(span, span) / resolution;

    const float hx0 = height.SampleLevel(g_samplerLinearClamp, uv - float2(step.x, 0.0f), 0.0f);
    const float hx1 = height.SampleLevel(g_samplerLinearClamp, uv + float2(step.x, 0.0f), 0.0f);
    const float hy0 = height.SampleLevel(g_samplerLinearClamp, uv - float2(0.0f, step.y), 0.0f);
    const float hy1 = height.SampleLevel(g_samplerLinearClamp, uv + float2(0.0f, step.y), 0.0f);

    // UV 単位の勾配 → 実寸の勾配（標高差 / 一辺）。tan(傾き) になる。
    const float dx = (hx1 - hx0) * 0.5f / step.x;
    const float dy = (hy1 - hy0) * 0.5f / step.y;
    const float slope = length(float2(dx, dy)) * g_op.params1.w;
    const float angleDegrees = degrees(atan(slope));

    const float low = g_op.params1.x;
    const float high = max(g_op.params1.y, low + 1e-3f);
    float value = saturate((angleDegrees - low) / (high - low));
    value = pow(value, max(g_op.params1.z, 1e-3f));
    output[texel] = saturate(ApplyInvert(value));
}

// レベル調整。黒点以下を 0、白点以上を 1 にして、間をガンマで曲げる。
[numthreads(8, 8, 1)]
void CsLevels(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    if (g_op.indices.y == kInvalidTextureIndex)
    {
        output[texel] = saturate(ApplyInvert(1.0f));
        return;
    }

    const float source = SampleMaskInput(g_op.indices.y, MaskUv(texel));
    const float black = g_op.params1.x;
    const float white = g_op.params1.y;
    // 黒点と白点が逆でも壊れないように、幅は下限で止める。
    float value = saturate((source - black) / max(white - black, 1e-4f));
    value = pow(value, max(g_op.params1.z, 1e-3f));
    output[texel] = saturate(ApplyInvert(value));
}

// マスク 2 枚の合成。**未接続は 0 ではなく「中立」**（繋いだほうを通す）。
[numthreads(8, 8, 1)]
void CsBlend(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    const float2 uv = MaskUv(texel);
    const bool hasA = (g_op.indices.y != kInvalidTextureIndex);
    const bool hasB = (g_op.indices.z != kInvalidTextureIndex);

    if (!hasA || !hasB)
    {
        const float single = hasA ? SampleMaskInput(g_op.indices.y, uv)
                                  : (hasB ? SampleMaskInput(g_op.indices.z, uv) : 0.0f);
        output[texel] = saturate(single);
        return;
    }

    const float foreground = SampleMaskInput(g_op.indices.y, uv);
    const float background = SampleMaskInput(g_op.indices.z, uv);

    float blended = foreground * background;
    const uint mode = g_op.params0.z;
    if (mode == TG_BLEND_ADD)      { blended = foreground + background; }
    else if (mode == TG_BLEND_MIN) { blended = min(foreground, background); }
    else if (mode == TG_BLEND_MAX) { blended = max(foreground, background); }

    // 強さは「前景そのまま」と「合成結果」の間の補間。
    output[texel] = saturate(lerp(foreground, blended, saturate(g_op.params1.x)));
}
