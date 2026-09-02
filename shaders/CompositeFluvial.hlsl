// 川筋（フロー累積）マスク。terrain-editor の Mask Fluvial を移植したもの。
//
// 下地の Height から**どこへ水が集まるか**を求め、その量をマスクにする。
// MFD（Multiple Flow Direction）: 各セルは下り勾配のある 8 近傍すべてへ、
// 勾配の集中度乗で重み付けして流量を配る。
//
// CPU 実装（terrain-editor）は「標高の降順にソートして上流から順に足す」
// という**本質的に逐次**の手順だが、GPU ではそれができない。代わりに
// **ヤコビ反復ゲザー**（毎回すべてのセルが上流の値を集め直す）を使う。
// 情報は 1 反復につき 1 セルずつ下流へ伝わるので、収束に要る反復回数は
// おおよそ最長流路長。安全側で 2 × 解像度 回まわす。
// 絵としての川筋は CPU 版と同等になるが、値はビット一致しない。
//
// パイプライン（すべて全画面ディスパッチ。タイルには分けない）:
//
//   CsSampleHeight  合成の Height（別解像度）→ 解析用グリッドへ落とす
//   CsBlurH/CsBlurV 「最大ディテール」に応じたローパス（流向を読む前の平滑化）
//   CsPitFill/CsCommit  局所窪みの埋め立て（ヤコビ二重バッファ）
//   CsWeights       8 方向の配分重みを 1 回だけ作る
//   CsAccumIter     ヤコビ反復ゲザー（AccumA / AccumB を ping-pong）
//   CsMaxClear/CsMaxReduce  正規化に使う最大値（uint の InterlockedMax）
//   CsToMask        出力カーブ（Log / しきい値 / 線形）でマスクにする

#include "CompositeCommon.hlsli"

#define TG_FLUVIAL_CURVE_LOG       0
#define TG_FLUVIAL_CURVE_THRESHOLD 1
#define TG_FLUVIAL_CURVE_LINEAR    2

struct FluvialConstants
{
    // 作業用テクスチャの UAV。すべて解析用グリッド（resolution^2）。
    uint4 indices0;  // heights, heightsScratch, weights0(k=0..3), weights1(k=4..7)
    uint4 indices1;  // accumA, accumB, mask, maxScratch(R32_UINT の 1x1)
    // x: 入力 Height の SRV（合成解像度）、y: 解析用グリッドの一辺、
    // z: ヤコビの向き（0 = A を読み B へ書く）、w: ぼかし半径（セル）
    uint4 indices2;
    // x: 集中度（MFD の指数）、y: しきい値（セル数）、z: ガンマ、w: やわらかさ
    float4 params0;
    // x: 川縁の強さ、y: 出力カーブ、zw: 未使用
    float4 params1;
};

ConstantBuffer<FluvialConstants> g_fluvial : register(b1);

// 方向の並び。0:NW 1:N 2:NE / 3:W 4:E / 5:SW 6:S 7:SE
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
// 近傍から自分へ向かう向き。k の逆向き。
static const uint kFlowOpposite[8] = {7, 6, 5, 4, 3, 2, 1, 0};

uint FluvialResolution() { return g_fluvial.indices2.y; }

bool OutsideGrid(uint2 cell)
{
    const uint resolution = FluvialResolution();
    return cell.x >= resolution || cell.y >= resolution;
}

// 8 方向の重みは 2 枚の RGBA へ分けて持つ（k=0..3 と k=4..7）。
float LoadWeight(uint2 cell, uint k)
{
    RWTexture2D<float4> weights0 = ResourceDescriptorHeap[g_fluvial.indices0.z];
    RWTexture2D<float4> weights1 = ResourceDescriptorHeap[g_fluvial.indices0.w];
    const float4 packed = (k < 4u) ? weights0[cell] : weights1[cell];
    const uint lane = k & 3u;
    if (lane == 0u) { return packed.x; }
    if (lane == 1u) { return packed.y; }
    if (lane == 2u) { return packed.z; }
    return packed.w;
}

// 合成の Height（合成解像度）を解析用グリッドへ落とす。
[numthreads(8, 8, 1)]
void CsSampleHeight(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_fluvial.indices2.x];
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_fluvial.indices0.x];

    const float2 uv = (float2(cell) + 0.5f) / float(FluvialResolution());
    heights[cell] = source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

// 分離型ガウスのローパス。**流向を読むための解析用ハイトだけ**を平滑化する
// （合成の Height には触らない）。小さな凹凸で流路が乱れるのを防ぐ。
float BlurAlongAxis(uint2 cell, int2 step, uint sourceIndex)
{
    RWTexture2D<float> source = ResourceDescriptorHeap[sourceIndex];
    const int resolution = int(FluvialResolution());
    const int radius = clamp(int(g_fluvial.indices2.w), 1, 64);
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
    RWTexture2D<float> scratch = ResourceDescriptorHeap[g_fluvial.indices0.y];
    scratch[cell] = BlurAlongAxis(cell, int2(1, 0), g_fluvial.indices0.x);
}

[numthreads(8, 8, 1)]
void CsBlurV(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_fluvial.indices0.x];
    heights[cell] = BlurAlongAxis(cell, int2(0, 1), g_fluvial.indices0.y);
}

// 局所窪みの埋め立て。8 近傍すべてが自分以上のセルを min(近傍) + ε まで持ち上げる。
// 窪みを残すとそこで流れが止まり、川筋が途切れる。
// **端のセルは出口**として扱い、動かさない。
[numthreads(8, 8, 1)]
void CsPitFill(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_fluvial.indices0.x];
    RWTexture2D<float> scratch = ResourceDescriptorHeap[g_fluvial.indices0.y];

    const uint resolution = FluvialResolution();
    const float height = heights[cell];
    if (cell.x == 0u || cell.y == 0u || cell.x == resolution - 1u || cell.y == resolution - 1u)
    {
        scratch[cell] = height;
        return;
    }

    float lowest = 1e30f;
    [unroll]
    for (int k = 0; k < 8; ++k)
    {
        const float neighbour = heights[uint2(int2(cell) + kFlowOffset[k])];
        lowest = min(lowest, neighbour);
    }

    const float kPitEpsilon = 1e-5f;
    scratch[cell] = (height <= lowest) ? (lowest + kPitEpsilon) : height;
}

[numthreads(8, 8, 1)]
void CsCommit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_fluvial.indices0.x];
    RWTexture2D<float> scratch = ResourceDescriptorHeap[g_fluvial.indices0.y];
    heights[cell] = scratch[cell];
}

// 8 方向への配分重み。勾配^集中度 を、正のぶんの合計で正規化する。
// 集中度を上げるほど主流へ集まり、下げるほど面的に広がる。
[numthreads(8, 8, 1)]
void CsWeights(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_fluvial.indices0.x];
    RWTexture2D<float4> weights0 = ResourceDescriptorHeap[g_fluvial.indices0.z];
    RWTexture2D<float4> weights1 = ResourceDescriptorHeap[g_fluvial.indices0.w];

    const int resolution = int(FluvialResolution());
    const float height = heights[cell];
    const float exponent = g_fluvial.params0.x;

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
            const float slope = (height - heights[uint2(neighbour)]) / kFlowDistance[k];
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
    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_fluvial.indices1.x];
    // どのセルも自分自身のぶんの 1（＝雨）を持つ。
    accumA[cell] = 1.0f;
}

// ヤコビ反復ゲザー。自分へ流し込んでいる近傍の値を集め直す。
//   total = 1 + Σ accum_prev[n] * weight[n → 自分]
[numthreads(8, 8, 1)]
void CsAccumIter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_fluvial.indices1.x];
    RWTexture2D<float> accumB = ResourceDescriptorHeap[g_fluvial.indices1.y];
    const bool readA = (g_fluvial.indices2.z == 0u);

    const int resolution = int(FluvialResolution());
    float total = 1.0f;
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
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_fluvial.indices1.w];
    maxScratch[uint2(0, 0)] = 0u;
}

// 非負の float はビット列の大小が uint と同じ順序になるので、
// そのまま InterlockedMax に掛けられる。
[numthreads(8, 8, 1)]
void CsMaxReduce(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_fluvial.indices1.x];
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_fluvial.indices1.w];

    const float adjusted = max(0.0f, accumA[cell] - g_fluvial.params0.y);
    uint previous;
    InterlockedMax(maxScratch[uint2(0, 0)], asuint(adjusted), previous);
}

[numthreads(8, 8, 1)]
void CsToMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideGrid(cell)) { return; }

    RWTexture2D<float> accumA = ResourceDescriptorHeap[g_fluvial.indices1.x];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_fluvial.indices1.z];
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_fluvial.indices1.w];

    const float accumulation = accumA[cell];
    const float threshold = g_fluvial.params0.y;
    const float gamma = g_fluvial.params0.z;
    const uint curve = uint(g_fluvial.params1.y);

    float result = 0.0f;
    if (curve == TG_FLUVIAL_CURVE_THRESHOLD)
    {
        // 閾値の前後を smoothstep で繋ぎ、川縁を pow でテーパーする。
        const float low = max(1.0f, threshold);
        const float softness = clamp(g_fluvial.params0.w, 0.001f, 4.0f);
        const float high = low * (1.0f + 4.0f * softness);
        const float t = saturate((accumulation - low) / max(high - low, 1e-3f));
        result = pow(t * t * (3.0f - 2.0f * t), g_fluvial.params1.x);
    }
    else
    {
        const float adjusted = max(0.0f, accumulation - threshold);
        const float maxAdjusted = asfloat(maxScratch[uint2(0, 0)]);
        if (curve == TG_FLUVIAL_CURVE_LINEAR)
        {
            result = pow(saturate(adjusted / max(maxAdjusted, 1e-3f)), gamma);
        }
        else
        {
            // 流量は下流で桁違いに大きくなるので、対数で圧縮すると
            // 細い支流から主流までが 1 枚のマスクに収まる。
            const float invLogMax = 1.0f / max(log(1.0f + maxAdjusted), 1e-3f);
            result = pow(saturate(log(1.0f + adjusted) * invLogMax), gamma);
        }
    }

    mask[cell] = result;
}
