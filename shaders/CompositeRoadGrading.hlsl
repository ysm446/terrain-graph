// 道路の均し（Road Grading）。道路メッシュに合わせて、地形を切土・盛土の形へ変える。
//
// 道路メッシュ（路面と路肩）の三角形を地形平面へ投影したもの（CompositeMaskMesh.hlsl と同じ
// バッファ。頂点の高さ付き）を受け、
//   - 道路の下は、路面の高さから「路面下の余裕」だけ下げる（地形が路面を突き抜けないように）
//   - 道路の外は、道路の端から「平らな幅」だけ同じ高さで延ばし、その先を法面の勾配で元の地形へ
//     すり付ける。元の地形が高ければ切土（上りの法面）、低ければ盛土（下りの法面）。
// 法面は「一番近い道路の端の高さ ± 勾配 × 距離」で作り、元の地形をその 2 枚の面の間へ挟む。
// 法面が元の地形と交わった所から先は、元の地形のまま。
//
//   CsRasterize  道路の三角形の内側のテクセルへ、路面の高さと種（自分の座標）を置く
//   CsJump       ジャンプフラッディング。一番近い道路のテクセルを全テクセルへ伝える
//   CsApply      距離と道路の高さから法面を作り、Height を書き換え、マスクを書く
//   CsMask       路面の下 / 切土 / 盛土のマスクを取り出す
//
// 一番近い道路の端だけで決めるので、上下の道が近いつづら折りでは、近いほうが切り替わる所に
// 段ができる（既知の制約）。

#include "CompositeCommon.hlsli"

// 三角形 1 枚は float 12 個（48 バイト）。CompositeMaskMesh.hlsl と同じ並び。
//   [0..5]: a.xy, b.xy, c.xy（正規化 UV）, [6..8]: 頂点の正規化ハイト, [9..11]: 未使用
#define TG_MESH_TRIANGLE_BYTES 48u

struct RoadGradingConstants
{
    // 合成の Height UAV, 路面の高さ UAV（R32F）, 種の読む側 UAV（R32_UINT）, 種の書く側 UAV
    uint4 indices0;
    // マスク UAV（RGBA8。路面の下 / 切土 / 盛土）, 三角形バッファの SRV, 三角形数, 合成解像度
    uint4 indices1;
    // ジャンプの間隔, マスクのチャンネル（CsMask。3 で 0 を書く）, マスクの出力 UAV, マスクの出力の解像度
    uint4 indices2;
    // 一辺（m）, 標高差（m）, 路面下の余裕（m）, 平らな幅（m）
    float4 params0;
    // 切土の勾配（高さ / 水平）, 盛土の勾配, 法面の最大の長さ（水平 m。0 で無制限）, マスクのぼかし（m）
    float4 params1;
};

ConstantBuffer<RoadGradingConstants> g_grading : register(b1);

static const uint kNoSeed = 0xFFFFFFFFu;

uint PackSeed(uint2 cell) { return cell.x | (cell.y << 16); }
uint2 UnpackSeed(uint seed) { return uint2(seed & 0xFFFFu, seed >> 16); }

#define TG_GRADING_CULL_BATCH 64u
groupshared uint g_gradingCullCount;
groupshared uint g_gradingCullIndices[TG_GRADING_CULL_BATCH];

// 道路の三角形の内側のテクセルへ、路面の高さ（正規化）と種を置く。外は「道路なし」。
// 三角形の間引きは CompositeMaskMesh.hlsl と同じ（8×8 のまとまりごとに、まとまりに掛かる
// 三角形だけを共有メモリへ集める）。
[numthreads(8, 8, 1)]
void CsRasterize(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
                 uint groupIndex : SV_GroupIndex)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_grading.indices1.w;
    const bool inside = texel.x < resolution && texel.y < resolution;

    RWTexture2D<float> roadHeight = ResourceDescriptorHeap[g_grading.indices0.y];
    RWTexture2D<uint> seeds = ResourceDescriptorHeap[g_grading.indices0.w];
    ByteAddressBuffer triangles = ResourceDescriptorHeap[g_grading.indices1.y];

    const float2 position = (float2(texel) + 0.5f) / float(resolution);
    const float2 groupMin = float2(groupId.xy * 8u) / float(resolution);
    const float2 groupMax = groupMin + 8.0f / float(resolution);

    float height = -1.0f;
    const uint count = g_grading.indices1.z;
    [loop]
    for (uint start = 0; start < count; start += TG_GRADING_CULL_BATCH)
    {
        if (groupIndex == 0u)
        {
            g_gradingCullCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        const uint candidate = start + groupIndex;
        if (candidate < count)
        {
            const uint base = candidate * TG_MESH_TRIANGLE_BYTES;
            const float4 ab = asfloat(triangles.Load4(base));
            const float2 c = asfloat(triangles.Load2(base + 16u));
            const float2 low = min(ab.xy, min(ab.zw, c));
            const float2 high = max(ab.xy, max(ab.zw, c));
            if (all(low <= groupMax) && all(high >= groupMin))
            {
                uint slot;
                InterlockedAdd(g_gradingCullCount, 1u, slot);
                g_gradingCullIndices[slot] = candidate;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        const uint survivors = g_gradingCullCount;
        [loop]
        for (uint k = 0; k < survivors; ++k)
        {
            const uint base = g_gradingCullIndices[k] * TG_MESH_TRIANGLE_BYTES;
            const float4 ab = asfloat(triangles.Load4(base));
            const float2 c = asfloat(triangles.Load2(base + 16u));
            const float3 heights = asfloat(triangles.Load3(base + 24u));
            const float2 a = ab.xy;
            const float2 b = ab.zw;
            // 辺ごとの符号付き面積。3 つが同じ符号なら内側で、そのまま重心座標の重みになる。
            const float wc = (b.x - a.x) * (position.y - a.y) - (b.y - a.y) * (position.x - a.x);
            const float wa = (c.x - b.x) * (position.y - b.y) - (c.y - b.y) * (position.x - b.x);
            const float wb = (a.x - c.x) * (position.y - c.y) - (a.y - c.y) * (position.x - c.x);
            const bool within = (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f) ||
                                (wa <= 0.0f && wb <= 0.0f && wc <= 0.0f);
            const float area = wa + wb + wc;
            if (within && abs(area) > 1e-20f)
            {
                // 重なった所（路面と路肩の継ぎ目など）は高いほうを取る。
                height = max(height, (wa * heights.x + wb * heights.y + wc * heights.z) / area);
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (inside)
    {
        roadHeight[texel] = height;
        seeds[texel] = (height >= 0.0f) ? PackSeed(texel) : kNoSeed;
    }
}

[numthreads(8, 8, 1)]
void CsJump(uint3 id : SV_DispatchThreadID)
{
    const int resolution = int(g_grading.indices1.w);
    if (any(int2(id.xy) >= resolution))
    {
        return;
    }
    RWTexture2D<uint> source = ResourceDescriptorHeap[g_grading.indices0.z];
    RWTexture2D<uint> target = ResourceDescriptorHeap[g_grading.indices0.w];

    const int step = int(g_grading.indices2.x);
    uint best = kNoSeed;
    float bestDistance = 1e30f;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 cell = int2(id.xy) + int2(x, y) * step;
            if (any(cell < 0) || any(cell >= resolution))
            {
                continue;
            }
            const uint seed = source[cell];
            if (seed == kNoSeed)
            {
                continue;
            }
            const float2 delta = float2(UnpackSeed(seed)) - float2(id.xy);
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
void CsApply(uint3 id : SV_DispatchThreadID)
{
    const uint resolution = g_grading.indices1.w;
    if (any(id.xy >= resolution))
    {
        return;
    }
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_grading.indices0.x];
    RWTexture2D<float> roadHeight = ResourceDescriptorHeap[g_grading.indices0.y];
    RWTexture2D<uint> seeds = ResourceDescriptorHeap[g_grading.indices0.z];
    RWTexture2D<float4> masks = ResourceDescriptorHeap[g_grading.indices1.x];

    const uint seed = seeds[id.xy];
    if (seed == kNoSeed)
    {
        // 道路が無い。地形はそのまま。
        masks[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float heightMeters = max(g_grading.params0.y, 1e-3f);
    const float texelMeters = g_grading.params0.x / float(resolution);
    const uint2 seedCell = UnpackSeed(seed);
    const float distance = length(float2(seedCell) - float2(id.xy)) * texelMeters;
    // 道路の面（路面の高さ − 余裕）。m で扱う。
    const float road = roadHeight[seedCell] * heightMeters - g_grading.params0.z;
    const float original = heightTarget[id.xy] * heightMeters;

    const float limit = g_grading.params1.z;
    // 平らな幅の先から法面。
    const float run = max(distance - g_grading.params0.w, 0.0f);
    float graded = original;
    float roadMask = 0.0f;
    if (limit <= 0.0f || run <= limit)
    {
        // 元の地形を、上りの法面（切土）と下りの法面（盛土）の間へ挟む。
        const float cutSurface = road + run * g_grading.params1.x;
        const float fillSurface = road - run * g_grading.params1.y;
        graded = clamp(original, fillSurface, cutSurface);
        // 路面の下（平らな幅まで）。縁を 1 テクセルぶんぼかす。
        roadMask = saturate(1.0f - (distance - g_grading.params0.w) / max(texelMeters, 1e-3f));
    }

    // 切土・盛土のマスク。削った / 盛った量が「マスクのぼかし」で 1 になる。路面の下は入れない。
    const float soft = max(g_grading.params1.w, 1e-3f);
    const float cut = saturate((original - graded) / soft) * (1.0f - roadMask);
    const float fill = saturate((graded - original) / soft) * (1.0f - roadMask);

    heightTarget[id.xy] = saturate(graded / heightMeters);
    masks[id.xy] = float4(roadMask, cut, fill, 1.0f);
}

// 路面の下 / 切土 / 盛土のマスク。チャンネルが 3 のとき（出どころが無効）は 0。
[numthreads(8, 8, 1)]
void CsMask(uint3 id : SV_DispatchThreadID)
{
    const uint outputResolution = g_grading.indices2.w;
    if (any(id.xy >= outputResolution))
    {
        return;
    }
    RWTexture2D<float4> masks = ResourceDescriptorHeap[g_grading.indices1.x];
    RWTexture2D<float> target = ResourceDescriptorHeap[g_grading.indices2.z];
    const uint channel = g_grading.indices2.y;
    if (channel >= 3u)
    {
        target[id.xy] = 0.0f;
        return;
    }
    const uint full = g_grading.indices1.w;
    const uint2 texel = min(uint2((float2(id.xy) + 0.5f) * float(full) / float(outputResolution)),
                            uint2(full - 1u, full - 1u));
    target[id.xy] = masks[texel][channel];
}
