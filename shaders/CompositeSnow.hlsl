// 積雪（Snow）。terrain-editor の同名ノードを移植したもの。
//
// 雪を一様に降らせ、**雪面**（下地 + 積雪厚）が安息角より急な所から
// 一番低い隣のセルへ滑らせる。これを何段も繰り返すと、谷・棚・緩い尾根に
// 溜まり、急な岩肌には残らない積もり方になる（GeoGen 風）。
//
// 堆積（CompositeSediment.hlsl）との違いは**行き先が 1 つだけ**なところ。
// 土砂は安息角を超えた 4 近傍すべてへ配るが、雪は一番急な 1 方向へ滑る。
// 雪庇や吹き溜まりのような「片側へ寄る」積もり方はこれで出る。
//
// 1 回の「滑らせ」は競合しないよう 2 掃引に分ける。
//   CsFlow  : 各セルが「どの向きへ、どれだけ出すか」を outflow へ書く
//   CsGather: 自分の流出を引き、自分を選んだ近傍の流出を足す
//
// **terrain-editor は 1 パスで、近傍 8 個ぶんの流出をその場で計算し直していた**
// （1 セルあたり 72 回の読み取り）。向きを 1 枚に書いておけば読み取りは 16 回で
// 済み、結果は同じになる。
//
// **合成の Height とは別のグリッドで回す。** 反復回数がそのまま効くので、
// 形が決まる粗さで十分（結果は積雪厚として合成解像度へ足し戻す）。

#include "CompositeCommon.hlsli"

struct SnowConstants
{
    // 書き込み（UAV）: 基盤（下地のハイト）、積雪厚、流出、ならしの作業用
    uint4 indices0;
    // 読み取り（SRV）: 基盤、積雪厚、ならしの作業用、合成の Height
    uint4 indices1;
    // x: グリッドの一辺、y: 合成の Height の UAV、z: 合成解像度、w: マスクの出力 UAV
    uint4 indices2;
    // x: 歩幅（セル）、y: ならしの向き（0 = 横 / 1 = 縦）、z: ならしの半径（セル）、
    // w: 降らせる場所のマスク（SRV。kInvalidTextureIndex なら全面へ一様に降らせる）
    uint4 indices3;
    // x: 安息角ぶんの落差（1 セル距離あたりの正規化ハイト）、y: 1 段あたりの供給量、
    // z: 流動率、w: 雪面のならしの強さ
    float4 params0;
    // x: マスクのしきい値（正規化ハイト）、y: マスクのぼかし（正規化ハイト）、zw: 未使用
    float4 params1;
};

ConstantBuffer<SnowConstants> g_snow : register(b1);

// 8 近傍。前半 4 つが上下左右、後半 4 つが斜め。逆向きは対で並べてある。
static const int2 kSnowOffset[8] = {
    int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
    int2(1, 1), int2(-1, -1), int2(1, -1), int2(-1, 1),
};
static const uint kSnowOpposite[8] = {1u, 0u, 3u, 2u, 5u, 4u, 7u, 6u};

uint SnowResolution() { return g_snow.indices2.x; }
uint SnowStride() { return max(1u, g_snow.indices3.x); }
float SnowTalus() { return g_snow.params0.x; }

bool OutsideSnow(uint2 cell)
{
    const uint resolution = SnowResolution();
    return cell.x >= resolution || cell.y >= resolution;
}

// **範囲外の近傍は「無い」ものとして飛ばす。** terrain-editor は座標を
// クランプしていたが、それだと縁で斜めと真横が同じセルを指し、
// 「どの向きから来たか」で流入を数え直せなくなる。
bool SnowNeighbour(uint2 cell, uint direction, out uint2 neighbour)
{
    const int resolution = int(SnowResolution());
    const int2 at = int2(cell) + kSnowOffset[direction] * int(SnowStride());
    neighbour = uint2(max(at, int2(0, 0)));
    return at.x >= 0 && at.x < resolution && at.y >= 0 && at.y < resolution;
}

// 歩幅ぶんの距離（セル）。斜めは √2 倍。
float SnowDistance(uint direction)
{
    const float diagonal = (direction >= 4u) ? 1.41421356237f : 1.0f;
    return float(SnowStride()) * diagonal;
}

// 積雪厚 → 被覆率。しきい値の前後をぼかし幅ぶんだけなめらかに繋ぐ。
float SnowCoverage(float snow)
{
    const float threshold = g_snow.params1.x;
    const float feather = g_snow.params1.y;
    if (feather <= 0.0f)
    {
        return (snow >= threshold) ? 1.0f : 0.0f;
    }
    const float lo = max(0.0f, threshold - feather);
    const float hi = threshold + feather;
    return smoothstep(lo, hi, snow);
}

// 合成の Height を解析用グリッドへ落とす。雪は下地を削らないので、
// これがそのまま「基盤」になる（堆積のように元の高さを別に持たなくてよい）。
// 落とし方は堆積と同じくセルの平均（間引くと材質の凹凸がエイリアスする）。
[numthreads(8, 8, 1)]
void CsSetup(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSnow(cell)) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_snow.indices1.w];
    RWTexture2D<float> baseHeight = ResourceDescriptorHeap[g_snow.indices0.x];
    RWTexture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices0.y];

    baseHeight[cell] = DownsampleHeight(source, cell, SnowResolution());
    thickness[cell] = 0.0f;
}

// 降らせる場所のマスク。繋いでいなければ 1（全面へ一様に降らせる）。
// マスクの op の結果は解析グリッドと解像度が違うので、添字ではなく UV で引く。
float SampleSnowMask(uint2 cell)
{
    if (g_snow.indices3.w == kInvalidTextureIndex)
    {
        return 1.0f;
    }
    Texture2D<float> mask = ResourceDescriptorHeap[g_snow.indices3.w];
    const float2 uv = (float2(cell) + 0.5f) / float(SnowResolution());
    return saturate(mask.SampleLevel(g_samplerLinearClamp, uv, 0.0f));
}

// 供給。雪を積む。**降る量はマスクの明るさに比例する**（1 の所で指定量）。
// 降った後の滑落はマスクを見ないので、縁からは外へ流れ出る。
[numthreads(8, 8, 1)]
void CsEmit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSnow(cell)) { return; }
    RWTexture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices0.y];
    thickness[cell] += g_snow.params0.y * SampleSnowMask(cell);
}

// 掃引 1。**一番急な下り 1 方向**を選び、そこへ出す量を決める。
//
//   - 安息角より緩い向きは相手にしない（雪は動かない）。
//   - 出す量は「安息角まで均すのに要るぶんの半分」を上限に、
//     手持ちの雪の 流動率 × 急さ ぶん。急なほど早く逃げる。
[numthreads(8, 8, 1)]
void CsFlow(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSnow(cell)) { return; }

    RWTexture2D<float> baseHeight = ResourceDescriptorHeap[g_snow.indices0.x];
    RWTexture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices0.y];
    RWTexture2D<float2> outflow = ResourceDescriptorHeap[g_snow.indices0.z];

    const float snow = max(0.0f, thickness[cell]);
    const float rate = saturate(g_snow.params0.z);
    if (snow <= 0.0f || rate <= 0.0f)
    {
        outflow[cell] = float2(0.0f, 0.0f);
        return;
    }

    const float talus = SnowTalus();
    const float surface = baseHeight[cell] + snow;
    float bestSlope = talus;
    float bestDistance = 1.0f;
    float bestSurface = surface;
    uint bestDirection = 8u;

    [unroll]
    for (uint k = 0u; k < 8u; ++k)
    {
        uint2 neighbour;
        if (!SnowNeighbour(cell, k, neighbour)) { continue; }

        const float distance = SnowDistance(k);
        const float neighbourSurface = baseHeight[neighbour] + max(0.0f, thickness[neighbour]);
        const float slope = (surface - neighbourSurface) / distance;
        if (slope > bestSlope)
        {
            bestSlope = slope;
            bestDistance = distance;
            bestSurface = neighbourSurface;
            bestDirection = k;
        }
    }

    if (bestDirection >= 8u)
    {
        outflow[cell] = float2(0.0f, 0.0f);
        return;
    }

    // 安息角ぶんの落差は残す。行き過ぎて相手より低くならないよう半分にする。
    const float stableDrop = talus * bestDistance;
    const float excess = max(0.0f, surface - bestSurface - stableDrop);
    const float slopeFactor = saturate((bestSlope - talus) / max(bestSlope, 1e-6f));
    const float amount = min(snow, min(excess * 0.5f, snow * rate * slopeFactor));
    // 向きは +1 して書く（0 は「出さない」）。
    outflow[cell] = (amount > 0.0f) ? float2(amount, float(bestDirection) + 1.0f)
                                    : float2(0.0f, 0.0f);
}

// 掃引 2。自分の流出を引き、**自分を選んだ**近傍の流出を足す。
// 読むのは流出の 1 枚だけなので、積雪厚はその場で書き換えてよい。
[numthreads(8, 8, 1)]
void CsGather(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSnow(cell)) { return; }

    RWTexture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices0.y];
    RWTexture2D<float2> outflow = ResourceDescriptorHeap[g_snow.indices0.z];

    const float2 own = outflow[cell];
    float incoming = 0.0f;

    [unroll]
    for (uint k = 0u; k < 8u; ++k)
    {
        uint2 source;
        if (!SnowNeighbour(cell, k, source)) { continue; }
        const float2 sourceFlow = outflow[source];
        if (sourceFlow.x <= 0.0f) { continue; }
        // 向き k の先にいる近傍が自分を選んでいるのは、その向きが逆向きのとき。
        if (uint(sourceFlow.y) == kSnowOpposite[k] + 1u)
        {
            incoming += sourceFlow.x;
        }
    }

    thickness[cell] = max(0.0f, thickness[cell] - own.x + incoming);
}

// 積もった雪面だけをならす（横）。
//
// **被覆率で重み付けする。** 雪の無い所まで混ぜると、雪の縁が地形へ
// 滲み出して薄い雪が広がってしまう。作業用の 2 チャンネルには
// x にならした雪面、y に元の積雪厚を写す（縦のパスが両方を読む）。
[numthreads(8, 8, 1)]
void CsSmoothHorizontal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSnow(cell)) { return; }

    RWTexture2D<float> baseHeight = ResourceDescriptorHeap[g_snow.indices0.x];
    RWTexture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices0.y];
    RWTexture2D<float2> scratch = ResourceDescriptorHeap[g_snow.indices0.w];

    const int resolution = int(SnowResolution());
    const int radius = clamp(int(g_snow.indices3.z), 1, 32);
    const float sigma = max(1.0f, float(radius) * 0.5f);
    const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);

    float sum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const uint2 at = uint2(uint(clamp(int(cell.x) + offset, 0, resolution - 1)), cell.y);
        const float snow = max(0.0f, thickness[at]);
        const float weight = exp(-float(offset * offset) * invTwoSigma2) * SnowCoverage(snow);
        sum += (baseHeight[at] + snow) * weight;
        weightSum += weight;
    }

    const float own = max(0.0f, thickness[cell]);
    const float blurred = (weightSum > 1e-6f) ? (sum / weightSum) : (baseHeight[cell] + own);
    scratch[cell] = float2(blurred, own);
}

// 積もった雪面だけをならす（縦）。ならした雪面から積雪厚へ戻し、
// 被覆率 × ならしの強さ だけ元の厚みと混ぜる。
[numthreads(8, 8, 1)]
void CsSmoothVertical(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSnow(cell)) { return; }

    RWTexture2D<float> baseHeight = ResourceDescriptorHeap[g_snow.indices0.x];
    RWTexture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices0.y];
    RWTexture2D<float2> scratch = ResourceDescriptorHeap[g_snow.indices0.w];

    const int resolution = int(SnowResolution());
    const int radius = clamp(int(g_snow.indices3.z), 1, 32);
    const float sigma = max(1.0f, float(radius) * 0.5f);
    const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);

    float sum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const uint2 at = uint2(cell.x, uint(clamp(int(cell.y) + offset, 0, resolution - 1)));
        const float2 packed = scratch[at];
        const float weight = exp(-float(offset * offset) * invTwoSigma2) * SnowCoverage(packed.y);
        sum += packed.x * weight;
        weightSum += weight;
    }

    const float original = scratch[cell].y;
    const float blurred = (weightSum > 1e-6f) ? (sum / weightSum) : (baseHeight[cell] + original);
    const float target = max(0.0f, blurred - baseHeight[cell]);
    const float blend = saturate(g_snow.params0.w) * SnowCoverage(original);
    thickness[cell] = max(0.0f, lerp(original, target, blend));
}

// 合成の Height へ積雪厚を足す。ここだけ合成解像度で回す。
//
// **積もった所では下の細部を埋める。** 厚み d だけ積もったなら、細部の
// 振れ幅を d だけ 0 へ寄せる（堆積の CsApply と同じ扱い。理屈は向こうの
// コメントにある）。雪が薄い所は元の岩肌が出て、深い所は雪面だけになる。
[numthreads(8, 8, 1)]
void CsApply(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_snow.indices2.z;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> baseHeight = ResourceDescriptorHeap[g_snow.indices1.x];
    Texture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices1.y];
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_snow.indices2.y];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    const float coarseBase = baseHeight.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float snow = max(thickness.SampleLevel(g_samplerLinearClamp, uv, 0.0f), 0.0f);

    // 合成解像度でしか持っていない細部（粗いグリッドとの差）。
    const float detail = heightTarget[texel] - coarseBase;
    const float buried = sign(detail) * max(abs(detail) - snow, 0.0f);

    heightTarget[texel] = saturate(coarseBase + snow + buried);
}

// 積雪厚を被覆率（0〜1）のマスクにする。マスクは合成解像度で出す。
[numthreads(8, 8, 1)]
void CsMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_snow.indices2.z;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> thickness = ResourceDescriptorHeap[g_snow.indices1.y];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_snow.indices2.w];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    mask[texel] = SnowCoverage(max(thickness.SampleLevel(g_samplerLinearClamp, uv, 0.0f), 0.0f));
}
