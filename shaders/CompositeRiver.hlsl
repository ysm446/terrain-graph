// 河川（River）。川筋から河床を掘り、下流へ単調に下がる水面を張る加工。
// 設計は docs/reference/river-node.md。
//
//   水面 = Planchon–Darboux 法の窪み埋め（最小勾配 ε 付き）
//
// 「水面を下流へ単調に下げる」は、端（出口）から内側へ
// `surface = max(地形, min(近傍の surface + ε × 距離))` を伝播させる反復で、
// これは窪み埋めそのもの。埋めた面は流向計算にも水面高にも使えるので、
// 1 回の反復で両方が出る。盆地は出口の高さまで埋まって湖になり、
// 湖の下流へも流量が続く（Mask Fluvial の 8 回固定の窪み埋めだと盆地で途切れる）。
//
// パイプライン（すべて全画面ディスパッチ。タイルには分けない）:
//
//   CsSample          合成の Height → 解析用グリッド
//   CsBlurH/CsBlurV   「最大ディテール」に応じたローパス
//   CsFillInit/CsFillIter  窪み埋め = 水面高（ヤコビ、surface / scratch を ping-pong）
//   CsWeights         埋めた面の上で 8 方向の配分重み
//   CsAccumInit/CsAccumIter  流量のヤコビ反復ゲザー（Seed は雨の量）
//   CsMaxClear/CsMaxReduce   流量の最大値（幅の基準）
//   CsWidth           川を抜き、中心線セルの半幅を決め、JFA の種を置く
//   CsJfaStep         Jump Flooding。「半幅 − 距離」が最大の種を選ぶ
//   CsResolve         水面高 / 水際からの距離 / 掘った地形 / 湖の深さ / 半幅
//   CsApply           合成解像度へ書き戻す（掘りは差分、水面は置き換え）
//   CsMask            Water / Bank / Depth のマスク

#include "CompositeCommon.hlsli"

struct RiverConstants
{
    uint4 indices0;  // UAV: heights, scratch, surface, weights0(k=0..3)
    uint4 indices1;  // UAV: weights1(k=4..7), accumA, accumB, width
    uint4 indices2;  // UAV: jfaA, jfaB, maxScratch(R32_UINT 1x1) / SRV: 種マスク（無ければ無効）
    uint4 indices3;  // UAV: waterLevel, distance, ground, lakeDepth
    uint4 indices4;  // UAV: halfWidth, waterFine, depthFine / SRV: 合成の Height
    uint4 indices5;  // UAV: 合成の Height, マスクの出力 / グリッドの一辺, 合成解像度
    uint4 indices6;  // ヤコビの向き（0 = A を読み B へ）, JFA の歩幅, ぼかし半径（セル）, JFA の読み側
    uint4 indices7;  // マスクのチャンネル, 水を張る, Height へ書く, 合成の Normal の UAV
    uint4 indices8;  // SRV: heights, waterLevel, distance, ground
    uint4 indices9;  // SRV: lakeDepth, halfWidth, waterFine, depthFine
    // x: 集中度、y: しきい値（セル数）、z: 最小勾配（1 セル距離あたりの正規化ハイト）、w: 未使用
    float4 params0;
    // x: 主流の半幅（セル）、y: 最小の半幅（セル）、z: 幅の伸び、w: 河床の深さ（正規化）
    float4 params1;
    // x: 岸の幅（セル）、y: 岸の硬さ、z: 河原の広がり（m）、w: 河原の比高（m）
    float4 params2;
    // x: 河原のぼかし、y: セルの大きさ（m）、z: 標高差（m）、w: 湖とみなす深さ（正規化）
    float4 params3;
    // x: 水際のぼかし（正規化）、y: 主流の半幅（m）、z: 岸の幅（m）、w: 標高差 / 一辺
    float4 params4;
};

ConstantBuffer<RiverConstants> g_river : register(b1);

// 方向の並び。0:NW 1:N 2:NE / 3:W 4:E / 5:SW 6:S 7:SE（CompositeFluvial と同じ）
static const int2 kFlowOffset[8] = {
    int2(-1, -1), int2(0, -1), int2(1, -1),
    int2(-1, 0),               int2(1, 0),
    int2(-1, 1),  int2(0, 1),  int2(1, 1),
};
static const float kFlowDistance[8] = {
    1.41421356f, 1.0f, 1.41421356f,
    1.0f,              1.0f,
    1.41421356f, 1.0f, 1.41421356f,
};
static const uint kFlowOpposite[8] = {7, 6, 5, 4, 3, 2, 1, 0};

uint RiverResolution() { return g_river.indices5.z; }
uint FineResolution() { return g_river.indices5.w; }

bool OutsideGrid(uint2 cell)
{
    const uint resolution = RiverResolution();
    return cell.x >= resolution || cell.y >= resolution;
}

bool IsEdge(uint2 cell)
{
    const uint resolution = RiverResolution();
    return cell.x == 0u || cell.y == 0u || cell.x == resolution - 1u || cell.y == resolution - 1u;
}

float LoadWeight(uint2 cell, uint k)
{
    RWTexture2D<float4> weights0 = ResourceDescriptorHeap[g_river.indices0.w];
    RWTexture2D<float4> weights1 = ResourceDescriptorHeap[g_river.indices1.x];
    const float4 packed = (k < 4u) ? weights0[cell] : weights1[cell];
    const uint lane = k & 3u;
    if (lane == 0u) { return packed.x; }
    if (lane == 1u) { return packed.y; }
    if (lane == 2u) { return packed.z; }
    return packed.w;
}

// 雨の量。Seed が繋がっていればその明るさ、無ければ全面 1。
float Rain(uint2 cell)
{
    const uint seedIndex = g_river.indices2.w;
    if (seedIndex == kInvalidTextureIndex)
    {
        return 1.0f;
    }
    Texture2D<float> seed = ResourceDescriptorHeap[seedIndex];
    const float2 uv = (float2(cell) + 0.5f) / float(RiverResolution());
    return max(0.0f, seed.SampleLevel(g_samplerLinearClamp, uv, 0.0f));
}

// --- 解析用グリッドへ落とす / ローパス -----------------------------------------

[numthreads(8, 8, 1)]
void CsSample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_river.indices4.w];
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_river.indices0.x];

    const float2 uv = (float2(cell) + 0.5f) / float(RiverResolution());
    heights[cell] = source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

float BlurAlongAxis(uint2 cell, int2 step, uint sourceIndex)
{
    RWTexture2D<float> source = ResourceDescriptorHeap[sourceIndex];
    const int resolution = int(RiverResolution());
    const int radius = clamp(int(g_river.indices6.z), 1, 64);
    const float sigma = max(1.0f, float(radius) * 0.5f);
    const float invTwoSigma2 = 1.0f / (2.0f * sigma * sigma);

    float sum = 0.0f;
    float weightSum = 0.0f;
    [loop]
    for (int offset = -radius; offset <= radius; ++offset)
    {
        const int2 sample = clamp(int2(cell) + step * offset, int2(0, 0),
                                  int2(resolution - 1, resolution - 1));
        const float weight = exp(-float(offset * offset) * invTwoSigma2);
        sum += source[uint2(sample)] * weight;
        weightSum += weight;
    }
    return sum / max(weightSum, 1e-6f);
}

[numthreads(8, 8, 1)]
void CsBlurH(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> scratch = ResourceDescriptorHeap[g_river.indices0.y];
    scratch[cell] = BlurAlongAxis(cell, int2(1, 0), g_river.indices0.x);
}

[numthreads(8, 8, 1)]
void CsBlurV(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_river.indices0.x];
    heights[cell] = BlurAlongAxis(cell, int2(0, 1), g_river.indices0.y);
}

// --- 窪み埋め = 水面高 ----------------------------------------------------------
//
// Planchon–Darboux 法。端は出口として地形高で固定し、内側は +∞ から始めて
//   surface = max(地形, min_k(surface[近傍 k] + ε × 距離 k))
// を収束するまで下げる。情報は 1 反復に 1 セル進むので、2 × 解像度 回まわす。
// 結果はどこでも地形以上で、盆地の中だけ地形より高い（= 湖）。

[numthreads(8, 8, 1)]
void CsFillInit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_river.indices0.x];
    RWTexture2D<float> surface = ResourceDescriptorHeap[g_river.indices0.z];
    surface[cell] = IsEdge(cell) ? heights[cell] : 1e30f;
}

[numthreads(8, 8, 1)]
void CsFillIter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_river.indices0.x];
    RWTexture2D<float> surface = ResourceDescriptorHeap[g_river.indices0.z];
    RWTexture2D<float> scratch = ResourceDescriptorHeap[g_river.indices0.y];
    const bool readSurface = (g_river.indices6.x == 0u);

    const float ground = heights[cell];
    float result = ground;
    if (!IsEdge(cell))
    {
        const float current = readSurface ? surface[cell] : scratch[cell];
        result = current;
        if (current > ground)
        {
            const float epsilon = g_river.params0.z;
            float best = current;
            [unroll]
            for (int k = 0; k < 8; ++k)
            {
                const uint2 neighbour = uint2(int2(cell) + kFlowOffset[k]);
                const float candidate =
                    (readSurface ? surface[neighbour] : scratch[neighbour]) +
                    epsilon * kFlowDistance[k];
                if (ground >= candidate)
                {
                    best = ground;
                    break;
                }
                best = min(best, candidate);
            }
            result = best;
        }
    }

    if (readSurface) { scratch[cell] = result; }
    else             { surface[cell] = result; }
}

// --- 流量 -----------------------------------------------------------------------

// 8 方向への配分重み。**埋めた面（水面）の上で**取る。ε の勾配が付いているので
// 平坦面でも重みが 0 にならない。
[numthreads(8, 8, 1)]
void CsWeights(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> surface = ResourceDescriptorHeap[g_river.indices0.z];
    RWTexture2D<float4> weights0 = ResourceDescriptorHeap[g_river.indices0.w];
    RWTexture2D<float4> weights1 = ResourceDescriptorHeap[g_river.indices1.x];

    const int resolution = int(RiverResolution());
    const float height = surface[cell];
    const float exponent = g_river.params0.x;

    float weight[8];
    float total = 0.0f;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        const int2 neighbour = int2(cell) + kFlowOffset[k];
        float value = 0.0f;
        if (neighbour.x >= 0 && neighbour.x < resolution && neighbour.y >= 0 &&
            neighbour.y < resolution)
        {
            const float slope = (height - surface[uint2(neighbour)]) / kFlowDistance[k];
            if (slope > 0.0f)
            {
                value = pow(slope, exponent);
            }
        }
        weight[k] = value;
        total += value;
    }

    const float inverse = (total > 0.0f) ? (1.0f / total) : 0.0f;
    weights0[cell] = float4(weight[0], weight[1], weight[2], weight[3]) * inverse;
    weights1[cell] = float4(weight[4], weight[5], weight[6], weight[7]) * inverse;
}

[numthreads(8, 8, 1)]
void CsAccumInit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_river.indices1.y];
    accumA[cell] = Rain(cell);
}

// ヤコビ反復ゲザー。雨は毎回足し直すので、Seed は初期化と反復の両方に掛かる。
[numthreads(8, 8, 1)]
void CsAccumIter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_river.indices1.y];
    RWTexture2D<float> accumB = ResourceDescriptorHeap[g_river.indices1.z];
    const bool readA = (g_river.indices6.x == 0u);

    const int resolution = int(RiverResolution());
    float total = Rain(cell);
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        const int2 neighbour = int2(cell) + kFlowOffset[k];
        if (neighbour.x < 0 || neighbour.x >= resolution || neighbour.y < 0 ||
            neighbour.y >= resolution)
        {
            continue;
        }
        const uint2 donor = uint2(neighbour);
        const float weight = LoadWeight(donor, kFlowOpposite[k]);
        if (weight > 0.0f)
        {
            total += (readA ? accumA[donor] : accumB[donor]) * weight;
        }
    }

    if (readA) { accumB[cell] = total; }
    else       { accumA[cell] = total; }
}

[numthreads(1, 1, 1)]
void CsMaxClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_river.indices2.z];
    maxScratch[uint2(0, 0)] = 0u;
}

// 流量の最大値（幅の基準）。非負の float はビット列の大小が uint と同じ。
[numthreads(8, 8, 1)]
void CsMaxReduce(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_river.indices1.y];
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_river.indices2.z];
    uint previous;
    InterlockedMax(maxScratch[uint2(0, 0)], asuint(max(0.0f, accumA[cell])), previous);
}

// --- 川を抜き、幅を決める ----------------------------------------------------------
//
//   半幅 = max(最小の半幅, 主流の半幅 × (Q / Qmax)^幅の伸び)
//
// 基準を主流側（Qmax）に置く。しきい値側を基準にすると、主流はその数百倍の
// 流量を持つので、支流の大半まで上限に張り付いて幅の差が消える。
[numthreads(8, 8, 1)]
void CsWidth(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_river.indices1.y];
    RWTexture2D<float> width = ResourceDescriptorHeap[g_river.indices1.w];
    RWTexture2D<float4> jfaA = ResourceDescriptorHeap[g_river.indices2.x];
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_river.indices2.z];

    const float flow = accumA[cell];
    const float flowMax = asfloat(maxScratch[uint2(0, 0)]);
    const float threshold = max(g_river.params0.y, 1e-6f);
    if (flow >= threshold && flow > 0.0f && flowMax > 0.0f)
    {
        const float ratio = saturate(flow / flowMax);
        const float half = max(g_river.params1.y, g_river.params1.x * pow(ratio, g_river.params1.z));
        width[cell] = half;
        jfaA[cell] = float4(float2(cell), half, 1.0f);
    }
    else
    {
        width[cell] = 0.0f;
        jfaA[cell] = float4(0.0f, 0.0f, -1e30f, 0.0f);
    }
}

// Jump Flooding。伝播する値は距離ではなく **「半幅(種) − 距離」の最大**。
// 単純な最近傍だと、太い本流と細い支流の合流点で支流のセルが最近傍になり、
// 本流の縁が欠ける。非ユークリッド計量なので JFA は近似になるが、この用途には足りる。
[numthreads(8, 8, 1)]
void CsJfaStep(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float4> jfaA = ResourceDescriptorHeap[g_river.indices2.x];
    RWTexture2D<float4> jfaB = ResourceDescriptorHeap[g_river.indices2.y];
    RWTexture2D<float> width = ResourceDescriptorHeap[g_river.indices1.w];
    const bool readA = (g_river.indices6.x == 0u);
    const int step = max(1, int(g_river.indices6.y));
    const int resolution = int(RiverResolution());

    float4 best = readA ? jfaA[cell] : jfaB[cell];
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            if (dx == 0 && dy == 0) { continue; }
            const int2 at = int2(cell) + int2(dx, dy) * step;
            if (at.x < 0 || at.x >= resolution || at.y < 0 || at.y >= resolution) { continue; }
            const float4 candidate = readA ? jfaA[uint2(at)] : jfaB[uint2(at)];
            if (candidate.w <= 0.0f) { continue; }
            const float distance = length(float2(cell) - candidate.xy);
            const float score = width[uint2(candidate.xy)] - distance;
            if (best.w <= 0.0f || score > best.z)
            {
                best = float4(candidate.xy, score, 1.0f);
            }
        }
    }

    if (readA) { jfaB[cell] = best; }
    else       { jfaA[cell] = best; }
}

// --- 水面高 / 距離 / 掘り ------------------------------------------------------
//
// 各セルについて、最寄りの中心線セル（種）から
//   - 水際からの距離 dw = 距離 − 半幅(種)（内側は負）
//   - 水面高 = 種の surface（断面を水平にするため、ローカルの surface ではない）
// を出す。湖（surface − 地形 > ε）はローカルの surface を水面高にする。
// 河床は中心線の周りだけ掘り、岸を `岸の硬さ` で外側の地形へ繋ぐ。湖底は掘らない。
[numthreads(8, 8, 1)]
void CsResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_river.indices0.x];
    RWTexture2D<float> surface = ResourceDescriptorHeap[g_river.indices0.z];
    RWTexture2D<float> width = ResourceDescriptorHeap[g_river.indices1.w];
    RWTexture2D<float4> jfaA = ResourceDescriptorHeap[g_river.indices2.x];
    RWTexture2D<float4> jfaB = ResourceDescriptorHeap[g_river.indices2.y];
    RWTexture2D<float> waterLevel = ResourceDescriptorHeap[g_river.indices3.x];
    RWTexture2D<float> distanceOut = ResourceDescriptorHeap[g_river.indices3.y];
    RWTexture2D<float> groundOut = ResourceDescriptorHeap[g_river.indices3.z];
    RWTexture2D<float> lakeOut = ResourceDescriptorHeap[g_river.indices3.w];
    RWTexture2D<float> halfWidthOut = ResourceDescriptorHeap[g_river.indices4.x];

    const float4 seed = (g_river.indices6.w == 0u) ? jfaA[cell] : jfaB[cell];
    const float ground = heights[cell];
    const float filled = surface[cell];
    const float lake = max(0.0f, filled - ground);
    const float cellMeters = g_river.params3.y;

    float dw = 1e6f;
    float half = 0.0f;
    float riverLevel = filled;
    float carved = ground;
    if (seed.w > 0.0f)
    {
        const uint2 seedCell = uint2(seed.xy);
        half = width[seedCell];
        dw = length(float2(cell) - seed.xy) - half;
        riverLevel = surface[seedCell];

        const float bedLevel = riverLevel - g_river.params1.w;
        const float bankWidth = g_river.params2.x;
        if (dw < 0.0f)
        {
            carved = min(ground, bedLevel);
        }
        else if (dw < bankWidth)
        {
            // 0 で直線の土手、1 で水際から一気に立ち上がる崖。
            const float t = saturate(dw / max(bankWidth, 1e-3f));
            const float hardness = saturate(g_river.params2.y);
            const float rise = 1.0f - pow(1.0f - t, 1.0f + 6.0f * hardness);
            carved = min(ground, lerp(bedLevel, ground, rise));
        }
    }

    const float level = (lake > g_river.params3.w) ? filled : riverLevel;
    waterLevel[cell] = level;
    distanceOut[cell] = dw * cellMeters;
    groundOut[cell] = carved;
    lakeOut[cell] = lake;
    halfWidthOut[cell] = half * cellMeters;
}

// --- 合成解像度へ書き戻す ----------------------------------------------------------
//
// 掘りは**差分**で足し戻す（粗いグリッドの結果で置き換えると細部が消える）。
// 水は「川の帯（岸を含む）か湖」の中で `max(地形, 水面高)`。岸は河床から
// 立ち上がるので、水面が岸と交わる所が自然に汀線になる。細部が水面より
// 高い所は島として残る。水面の被覆と水深はここで合成解像度のまま焼く。
[numthreads(8, 8, 1)]
void CsApply(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = FineResolution();
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> heights = ResourceDescriptorHeap[g_river.indices8.x];
    Texture2D<float> waterLevel = ResourceDescriptorHeap[g_river.indices8.y];
    Texture2D<float> distance = ResourceDescriptorHeap[g_river.indices8.z];
    Texture2D<float> ground = ResourceDescriptorHeap[g_river.indices8.w];
    Texture2D<float> lakeDepth = ResourceDescriptorHeap[g_river.indices9.x];
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_river.indices5.x];
    RWTexture2D<float> waterFine = ResourceDescriptorHeap[g_river.indices4.y];
    RWTexture2D<float> depthFine = ResourceDescriptorHeap[g_river.indices4.z];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    const float coarseOriginal = heights.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float coarseCarved = ground.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float delta = coarseCarved - coarseOriginal;

    const float fineGround = heightTarget[texel] + delta;
    const float dw = distance.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float level = waterLevel.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float lake = lakeDepth.SampleLevel(g_samplerLinearClamp, uv, 0.0f);

    const bool inRegion = (dw < g_river.params4.z) || (lake > g_river.params3.w);
    const float watered = inRegion ? max(fineGround, level) : fineGround;
    const float depth = watered - fineGround;

    waterFine[texel] = saturate(depth / max(g_river.params4.x, 1e-6f));
    depthFine[texel] = saturate(depth / max(g_river.params1.w, 1e-6f));

    if (g_river.indices7.z != 0u)
    {
        const bool fill = (g_river.indices7.y != 0u);
        heightTarget[texel] = saturate(fill ? watered : fineGround);
    }
}

// --- 水面の法線 --------------------------------------------------------------------
//
// 合成の Height は R16 なので、緩く傾いた水面は約 0.3 m ごとの階段になり、
// そこから作った法線は縞になる。水面の法線は **R32 の水面高（解析グリッド）**の
// 勾配から作り直し、水の被覆で混ぜる。Height から作った法線の後に掛けること。
// 勾配のスケールは CompositeBlur.hlsl の CsNormalFromHeight と同じ考え方。
[numthreads(8, 8, 1)]
void CsWaterNormal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = FineResolution();
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> waterLevel = ResourceDescriptorHeap[g_river.indices8.y];
    RWTexture2D<float> waterFine = ResourceDescriptorHeap[g_river.indices4.y];
    RWTexture2D<float2> normalTarget = ResourceDescriptorHeap[g_river.indices7.w];

    const float cover = waterFine[texel];
    if (cover <= 0.0f) { return; }

    const float2 texelSize = 1.0f / float(resolution);
    const float2 uv = (float2(texel) + 0.5f) * texelSize;
    const float lx0 = waterLevel.SampleLevel(g_samplerLinearClamp, uv - float2(texelSize.x, 0.0f), 0.0f);
    const float lx1 = waterLevel.SampleLevel(g_samplerLinearClamp, uv + float2(texelSize.x, 0.0f), 0.0f);
    const float ly0 = waterLevel.SampleLevel(g_samplerLinearClamp, uv - float2(0.0f, texelSize.y), 0.0f);
    const float ly1 = waterLevel.SampleLevel(g_samplerLinearClamp, uv + float2(0.0f, texelSize.y), 0.0f);

    const float dx = (lx1 - lx0) * 0.5f * float(resolution);
    const float dy = (ly1 - ly0) * 0.5f * float(resolution);
    const float scale = g_river.params4.w;
    const float3 water = normalize(float3(-dx * scale, -dy * scale, 1.0f));

    const float2 existing = normalTarget[texel];
    normalTarget[texel] = lerp(existing, EncodeTangentNormal(water), cover);
}

// --- マスク ------------------------------------------------------------------------
//
//   Water : 水面の被覆（合成解像度で焼いたもの）
//   Bank  : 河原。水際からの距離の帯 × 水面からの比高の帯。水の中は 0
//   Depth : 水深（河床の深さで 1）
[numthreads(8, 8, 1)]
void CsMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = FineResolution();
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> waterFine = ResourceDescriptorHeap[g_river.indices9.z];
    Texture2D<float> depthFine = ResourceDescriptorHeap[g_river.indices9.w];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_river.indices5.y];

    const uint channel = g_river.indices7.x;
    if (channel == 0u)
    {
        mask[texel] = waterFine[texel];
        return;
    }
    if (channel == 2u)
    {
        mask[texel] = depthFine[texel];
        return;
    }

    Texture2D<float> waterLevel = ResourceDescriptorHeap[g_river.indices8.y];
    Texture2D<float> distance = ResourceDescriptorHeap[g_river.indices8.z];
    Texture2D<float> ground = ResourceDescriptorHeap[g_river.indices8.w];
    Texture2D<float> halfWidth = ResourceDescriptorHeap[g_river.indices9.y];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    const float dw = distance.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float half = halfWidth.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float level = waterLevel.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float carved = ground.SampleLevel(g_samplerLinearClamp, uv, 0.0f);

    // 河原の広がりは流量で伸びる（半幅と同じ比）。源流の細い沢に主流と同じ河原は作らない。
    const float mainHalf = max(g_river.params4.y, 1e-3f);
    const float shore = g_river.params2.z * saturate(half / mainHalf);
    const float shoreHeight = g_river.params2.w;
    const float feather = saturate(g_river.params3.x);
    const float featherDistance = max(feather * shore, 1e-3f);
    const float featherHeight = max(feather * shoreHeight, 1e-3f);

    const float distanceBand =
        smoothstep(0.0f, featherDistance, dw) *
        (1.0f - smoothstep(max(shore - featherDistance, 0.0f), max(shore, 1e-3f), dw));
    const float relief = (carved - level) * g_river.params3.z;
    const float heightBand = 1.0f - smoothstep(shoreHeight, shoreHeight + featherHeight, relief);

    mask[texel] = distanceBand * heightBand * (1.0f - waterFine[texel]);
}
