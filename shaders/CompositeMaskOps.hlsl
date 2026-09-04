// マスクのノードグラフを 1 枚ずつ焼くパス。
//
// グラフのマスクは `compositor::MaskProgram`（op の列）へ落ちていて、
// 評価器が入力の順に 1 op ずつここへ流す。結果は op ごとのテクスチャに残るので、
// 合流（Mask Blend）や、同じマスクを 2 か所で使うことができる。
//
// 川筋（Fluvial）だけは反復が要るので別（CompositeFluvial.hlsl）。

#include "CompositeCommon.hlsli"

// compositor::CurvatureMode と一致させること。
#define TG_CURVATURE_RIDGES   0
#define TG_CURVATURE_VALLEYS  1
#define TG_CURVATURE_ABSOLUTE 2

// compositor::MaskBlendMode と一致させること。
#define TG_BLEND_ADD      0
#define TG_BLEND_MULTIPLY 1
#define TG_BLEND_MIN      2
#define TG_BLEND_MAX      3

struct MaskOpConstants
{
    // x: 出力 UAV、y: 入力 A の SRV、z: 入力 B の SRV
    //   （Height だけは最低 / 最高をためる UAV。入力 B を取らないので流用する）、
    // w: 素材の SRV（Image は画像、Slope / Curv / Height は合成の Height）
    uint4 indices;
    // x: 出力の一辺、y: 読むチャンネル（Image）/ 種類（Noise）/ 全範囲（Height）、
    // z: 反転 / ブレンドの種類、w: オクターブ（Noise）
    uint4 params0;
    // Slope : 最小角（度）, 最大角（度）, ガンマ, 実寸比（標高差 / 一辺）
    // Levels: 黒点, 白点, ガンマ, 未使用
    // Blend : 強さ, 未使用 x3
    // Noise : 周波数, 量, オフセット, 未使用
    // Curv  : 感度（正規化ハイト）, しきい値, ガンマ, 未使用
    // Height: 最低（正規化ハイト）, 最高, ガンマ, フェザー
    float4 params1;
    // x: 傾斜を測る距離 / 曲率で比べる周りの広さ（テクセル）、yzw: 未使用
    // Blur  : x = ぼかし半径（テクセル）、y = 強さ
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

// 下地の曲率をマスクにする。**周りの平均との高さの差**を見る。
//
// terrain-editor は箱ぼかしを 1 枚作ってから引くが、ここは同心円の
// **4 本のリング（32 点）を半径で重み付けして平均**し、円板の平均を近似する。
// 半径が 64 テクセルまで伸びるので、まともに畳むと 1 テクセルあたり 1 万点を超える。
// 高さは滑らかなので、この近似で十分に同じ絵になる。
[numthreads(8, 8, 1)]
void CsCurvature(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    Texture2D<float> height = ResourceDescriptorHeap[g_op.indices.w];

    const float resolution = float(MaskResolution());
    const float2 uv = MaskUv(texel);
    const float radius = max(g_op.params2.x, 1.0f);  // 比べる周りの広さ（テクセル）
    const float center = height.SampleLevel(g_samplerLinearClamp, uv, 0.0f);

    // 円板の平均。リングの重みは面積に比例させる（半径そのもの）。
    float sum = center * 0.5f;
    float weight = 0.5f;
    for (int ring = 1; ring <= 4; ++ring)
    {
        const float ringRadius = radius * (float(ring) / 4.0f);
        const float ringWeight = float(ring);
        for (int i = 0; i < 8; ++i)
        {
            const float angle = (float(i) / 8.0f) * 6.28318530718f;
            const float2 offset = float2(cos(angle), sin(angle)) * ringRadius / resolution;
            sum += height.SampleLevel(g_samplerLinearClamp, uv + offset, 0.0f) * ringWeight;
            weight += ringWeight;
        }
    }
    const float average = sum / max(weight, 1e-6f);

    // 周りより高ければ正、低ければ負。
    const float delta = center - average;
    const uint mode = uint(g_op.params0.y);
    float curvature = abs(delta);
    if (mode == TG_CURVATURE_RIDGES) { curvature = delta; }
    else if (mode == TG_CURVATURE_VALLEYS) { curvature = -delta; }

    // 感度は正規化ハイトへ直して渡してある。
    float value = saturate(curvature / max(g_op.params1.x, 1e-9f));
    const float threshold = saturate(g_op.params1.y);
    value = saturate((value - threshold) / max(1.0f - threshold, 1e-4f));
    value = pow(value, max(g_op.params1.z, 1e-3f));
    output[texel] = saturate(ApplyInvert(value));
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
    // **反転の前に 0〜1 へ収める。** EXR のように 0〜1 を外れる画像だと、
    // 1 - 1.3 = -0.3 のように潰れて、反転したように見えなくなる。
    // 他の op は saturate 済みの値を反転している。
    output[texel] = saturate(ApplyInvert(saturate(SelectChannel(sampled, g_op.params0.y))));
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

// 下地の標高。**メートルで切る。**
//
// ハイト 0〜1 の全幅が標高差なので、m で指定した値はその比へ直して渡してある
// （0 m が地形の一番低い所、標高差 m が一番高い所）。
//
// **全範囲**（`useFullRange`）のときは、地形の**実際の**最低 / 最高で正規化する。
// これは 2 パス（クリア → 集計）を先に走らせて 1 枚へためておく。
// 非負の float はビット列の大小が uint と同じ順序なので、そのまま
// InterlockedMin / InterlockedMax に掛けられる（川筋の最大値集計と同じ手）。

[numthreads(1, 1, 1)]
void CsHeightRangeClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2D<uint> range = ResourceDescriptorHeap[g_op.indices.z];
    range[uint2(0, 0)] = 0xFFFFFFFFu;  // 最低（min の初期値）
    range[uint2(1, 0)] = 0u;           // 最高（max の初期値）
}

[numthreads(8, 8, 1)]
void CsHeightRangeReduce(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    Texture2D<float> height = ResourceDescriptorHeap[g_op.indices.w];
    RWTexture2D<uint> range = ResourceDescriptorHeap[g_op.indices.z];

    const uint bits = asuint(max(height.Load(int3(int2(texel), 0)), 0.0f));
    uint previous;
    InterlockedMin(range[uint2(0, 0)], bits, previous);
    InterlockedMax(range[uint2(1, 0)], bits, previous);
}

[numthreads(8, 8, 1)]
void CsHeight(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    Texture2D<float> height = ResourceDescriptorHeap[g_op.indices.w];

    const float sample = height.SampleLevel(g_samplerLinearClamp, MaskUv(texel), 0.0f);

    float value = 0.0f;
    if (g_op.params0.y != 0u)
    {
        // 全範囲。地形の一番低い所が 0、一番高い所が 1 になる。
        RWTexture2D<uint> range = ResourceDescriptorHeap[g_op.indices.z];
        const float low = asfloat(range[uint2(0, 0)]);
        const float high = asfloat(range[uint2(1, 0)]);
        value = saturate((sample - low) / max(high - low, 1e-6f));
    }
    else
    {
        // 標高帯。フェザーが 0 なら硬い帯、0 より大きいと上下の外側へなだらかに繋ぐ。
        const float low = g_op.params1.x;
        const float high = max(g_op.params1.y, low);
        const float feather = g_op.params1.w;
        if (feather <= 0.0f)
        {
            value = (sample >= low && sample <= high) ? 1.0f : 0.0f;
        }
        else
        {
            const float lower = smoothstep(low - feather, low, sample);
            const float upper = 1.0f - smoothstep(high, high + feather, sample);
            value = saturate(min(lower, upper));
        }
    }

    // **ガンマは他のマスクと同じ向き**（pow(value, gamma)）。terrain-editor は
    // 1/gamma だが、こちらは傾斜 / 曲率と揃えて「1 未満で弱い所を明るく」にする。
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


// --- マスクのぼかし -------------------------------------------------------
//
// terrain-editor の Mask Blur の移植。あちらは箱ぼかしの繰り返しだが、
// こちらは**分離型ガウス**にしてハイトのぼかし（CompositeBlur.hlsl）と揃える。
// 同じ「半径（m）/ 強さ / 反復」のつまみで同じ効き方をするほうが、
// 2 つのぼかしを行き来したときに戸惑わない。
//
//   種入れ（CsMaskBlurSeed）: 入力のマスクを結果テクスチャへ写す
//   反復 × { 水平（axis=0）: 結果 → 作業用 / 垂直（axis=1）: 作業用 → 結果 }
//
// **元のマスクと混ぜるのは垂直パスだけ。** 両方で混ぜると強さが 2 回掛かる。

// 入力のマスクを結果テクスチャへ写す。**ぼかしの反復はここを起点に回る。**
// 入力の解像度が違っても揃うよう、添字ではなく UV で引く。
[numthreads(8, 8, 1)]
void CsMaskBlurSeed(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    if (OutsideMask(texel)) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];
    if (g_op.indices.y == kInvalidTextureIndex)
    {
        output[texel] = 0.0f;
        return;
    }
    output[texel] = saturate(SampleMaskInput(g_op.indices.y, MaskUv(texel)));
}

// 分離型ガウスの 1 パス。重みはシェーダ内で作り、合計で正規化する。
//   w(x) = exp(-0.5 * (x / sigma)^2)、sigma = 半径 * 0.5
[numthreads(8, 8, 1)]
void CsMaskBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const int2 texel = int2(dispatchThreadId.xy);
    if (OutsideMask(uint2(texel))) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_op.indices.y];
    RWTexture2D<float> output = ResourceDescriptorHeap[g_op.indices.x];

    const int limit = int(MaskResolution()) - 1;
    const int2 step = (g_op.params0.y == 0u) ? int2(1, 0) : int2(0, 1);
    const float radiusTexels = g_op.params2.x;
    const int kernelRadius = clamp(int(ceil(radiusTexels)), 1, 128);
    const float sigma = max(radiusTexels * 0.5f, 0.5f);

    float sum = source.Load(int3(texel, 0));
    float weightSum = 1.0f;
    for (int offset = 1; offset <= kernelRadius; ++offset)
    {
        const float x = float(offset) / sigma;
        const float weight = exp(-0.5f * x * x);
        // 端は clamp-to-edge。境界のアーティファクトを最小にする。
        const int2 low = clamp(texel - step * offset, int2(0, 0), int2(limit, limit));
        const int2 high = clamp(texel + step * offset, int2(0, 0), int2(limit, limit));
        sum += (source.Load(int3(low, 0)) + source.Load(int3(high, 0))) * weight;
        weightSum += weight * 2.0f;
    }

    const float blurred = saturate(sum / weightSum);
    if (g_op.params0.y == 0u)
    {
        output[uint2(texel)] = blurred;
        return;
    }
    // 反転はどの op でも最後に掛ける、が、ぼかしは反転のつまみを持たない
    // （入力を反転したいなら Mask Levels を挟む）。ここでは強さだけを掛ける。
    output[uint2(texel)] =
        saturate(lerp(output[uint2(texel)], blurred, saturate(g_op.params2.y)));
}
