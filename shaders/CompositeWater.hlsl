// 水際からの距離（Liquid）。
//
// 水の場（合成解像度、RGBA16F）は x = 水際からの符号付き距離（m。水の中が正、陸が負）、
// y = 符号付きの水深（m。水位 − 下地の高さ。水の中が正、陸が負）、z / w は波の強さと波打ち際の度合い。
// y は Liquid（CompositeLayer.hlsl）と Lake（CompositeWaterPaint.hlsl）が書き、
// ここは y の符号が変わる所（汀線）からの距離を x へ書く（z / w はそのまま残す）。地形の描画が、岸へ向かって
// 進む泡の筋の位相に使う（MeshPbr.hlsl）。
//
// 進め方はジャンプフラッディング。作業用の格子（合成解像度以下）に「一番近い汀線の位置」を伝播し、
// 最後に合成解像度で距離へ直す。汀線の位置は格子の中心ではなく、隣との水深の線形補間で求めた
// 符号の変わる点（格子より細かい）。
//
//   CsSeed     汀線に接する格子へ、汀線の位置（作業用の格子の座標）を置く
//   CsJump     間隔 step の 9 近傍から、一番近い汀線の位置を取る（step を半分ずつにして繰り返す）
//   CsResolve  合成解像度で、周りの 4 格子が持つ汀線の位置までの距離を取り、水の場の x へ書く
#include "CompositeCommon.hlsli"

struct WaterConstants
{
    uint4 indices;  // 水の場 UAV, 読む側の作業用 UAV, 書く側の作業用 UAV（CsMask ではマスクの出力）, 合成解像度
    uint4 params;   // 作業用（CsMask では出力）の解像度, ジャンプの間隔, CsMask のチャンネル, 未使用
    float4 scale;   // 地形の一辺（m）, 距離の上限（m）, CsMask: Depth が 1 になる水深（m）, Shore の帯の幅（m）
};
ConstantBuffer<WaterConstants> g_water : register(b1);

// CompositeLayer.hlsl の kWaterNone と揃える。これより小さい水深は「Liquid が書いていない」。
static const float kWaterNone = -10000.0f;
static const float2 kNoSeed = float2(-1.0f, -1.0f);

float DepthAtCell(RWTexture2D<float4> water, int2 cell)
{
    const int work = int(g_water.params.x);
    const int full = int(g_water.indices.w);
    const int2 clamped = clamp(cell, int2(0, 0), int2(work - 1, work - 1));
    // 作業用の格子の中心にあたる合成テクセル。
    const int2 texel = min(int2((float2(clamped) + 0.5f) * float(full) / float(work)), int2(full - 1, full - 1));
    return water[texel].y;
}

[numthreads(8, 8, 1)]
void CsSeed(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_water.params.xx))
    {
        return;
    }
    RWTexture2D<float4> water = ResourceDescriptorHeap[g_water.indices.x];
    RWTexture2D<float2> target = ResourceDescriptorHeap[g_water.indices.z];

    const int2 cell = int2(id.xy);
    const float depth = DepthAtCell(water, cell);
    float2 seed = kNoSeed;
    if (depth > kWaterNone * 0.5f)
    {
        static const int2 kOffsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};
        float best = 2.0f;
        for (int i = 0; i < 4; ++i)
        {
            const float other = DepthAtCell(water, cell + kOffsets[i]);
            if (other <= kWaterNone * 0.5f || (depth > 0.0f) == (other > 0.0f))
            {
                continue;
            }
            // 隣との間で水深が 0 になる点（0〜1）。一番近いものを汀線の位置にする。
            const float t = saturate(depth / (depth - other));
            if (t < best)
            {
                best = t;
                seed = float2(cell) + float2(kOffsets[i]) * t;
            }
        }
    }
    target[id.xy] = seed;
}

[numthreads(8, 8, 1)]
void CsJump(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_water.params.xx))
    {
        return;
    }
    RWTexture2D<float2> source = ResourceDescriptorHeap[g_water.indices.y];
    RWTexture2D<float2> target = ResourceDescriptorHeap[g_water.indices.z];

    const int work = int(g_water.params.x);
    const int step = int(g_water.params.y);
    const float2 position = float2(id.xy);
    float2 best = kNoSeed;
    float bestDistance = 1e30f;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 cell = int2(id.xy) + int2(x, y) * step;
            if (any(cell < 0) || any(cell >= work))
            {
                continue;
            }
            const float2 seed = source[cell];
            if (seed.x < -0.5f)
            {
                continue;
            }
            const float2 delta = seed - position;
            const float distance = dot(delta, delta);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = seed;
            }
        }
    }
    target[id.xy] = best;
}

[numthreads(8, 8, 1)]
void CsResolve(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_water.indices.ww))
    {
        return;
    }
    RWTexture2D<float4> water = ResourceDescriptorHeap[g_water.indices.x];
    RWTexture2D<float2> source = ResourceDescriptorHeap[g_water.indices.y];

    const float depth = water[id.xy].y;
    const float limit = g_water.scale.y;
    if (depth <= kWaterNone * 0.5f)
    {
        // Liquid が書いていない所（マスクの外）。水際から遠い陸として扱う。
        water[id.xy] = float4(-limit, depth, water[id.xy].zw);
        return;
    }

    const int work = int(g_water.params.x);
    const float full = float(g_water.indices.w);
    // このテクセルの位置（作業用の格子の座標。格子の中心が整数）。
    const float2 position = (float2(id.xy) + 0.5f) * float(work) / full - 0.5f;
    const int2 base = int2(floor(position));
    float nearest = 1e30f;
    for (int y = 0; y <= 1; ++y)
    {
        for (int x = 0; x <= 1; ++x)
        {
            const int2 cell = clamp(base + int2(x, y), int2(0, 0), int2(work - 1, work - 1));
            const float2 seed = source[cell];
            if (seed.x < -0.5f)
            {
                continue;
            }
            const float2 delta = seed - position;
            nearest = min(nearest, dot(delta, delta));
        }
    }
    const float cellMeters = g_water.scale.x / float(work);
    const float distance = (nearest < 1e29f) ? min(sqrt(nearest) * cellMeters, limit) : limit;
    water[id.xy] = float4((depth > 0.0f) ? distance : -distance, depth, water[id.xy].zw);
}

// --- マスク ------------------------------------------------------------------------
// Liquid の Mask 出力。水の場から焼く。
//   Water : 水面の範囲。水の中が 1、陸が 0（水際を 5 cm の水深でぼかす）
//   Depth : 水深。指定した深さで 1
//   Shore : 水際の帯。水際で 1、陸側・水側とも指定した距離で 0
// チャンネルが 3 のとき（出どころの Liquid が無効）は 0 を書く。
[numthreads(8, 8, 1)]
void CsMask(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_water.params.xx))
    {
        return;
    }
    RWTexture2D<float4> water = ResourceDescriptorHeap[g_water.indices.x];
    RWTexture2D<float> target = ResourceDescriptorHeap[g_water.indices.z];

    const uint channel = g_water.params.z;
    const uint full = g_water.indices.w;
    // 出力のテクセルの中心にあたる合成テクセル。
    const uint2 texel = min(uint2((float2(id.xy) + 0.5f) * float(full) / float(g_water.params.x)),
                            uint2(full - 1u, full - 1u));
    const float4 field = water[texel];
    float value = 0.0f;
    if (channel < 3u && field.y > kWaterNone * 0.5f)
    {
        if (channel == 0u)
        {
            value = smoothstep(0.0f, 0.05f, field.y);
        }
        else if (channel == 1u)
        {
            value = saturate(field.y / max(g_water.scale.z, 1e-3f));
        }
        else
        {
            const float t = saturate(1.0f - abs(field.x) / max(g_water.scale.w, 1e-3f));
            value = t * t * (3.0f - 2.0f * t);
        }
    }
    target[id.xy] = value;
}
