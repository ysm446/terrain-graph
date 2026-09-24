// パスをマスクにする（Mask Path / Mask Area ノード）。
//
// 線分の列（両端の座標・幅・フェザー・強さ）をバッファ（ByteAddressBuffer）で受ける。
// 座標は地形平面の正規化 UV で、距離は一辺の長さ（m）を掛けて実寸にしてから比べる。
//
//   CsPath: 足跡。各テクセルから最寄りの線分までの距離を取り、幅の内側は 1、外側は
//           フェザーで 0 へ落とし、線分どうしは max で重ねる（terrain-editor の Mask Path）。
//   CsArea: 面。閉じた鎖を多角形とみなし、交差数の偶奇で内側を 1 にする（輪の中の輪は穴）。
//           縁は多角形までの符号付き距離を「縁のずれ」で動かし、「縁のぼかし」で 0 へ落とす。
//           点ごとの幅とフェザーは線用のデータなので読まない。
//
// **以前は定数バッファに 256 本ずつ流していた**が、面の偶奇はバッチをまたいで持ち越せない
// （曲線の輪は簡単に 256 本を超える）ので、バッファに置いて 1 回で読む。

#include "CompositeCommon.hlsli"
#include "CompositePath.hlsli"

struct PathMaskConstants
{
    // x: 出力 UAV、y: 出力の一辺、z: 線分数、w: 線分バッファの SRV
    uint4 indices;
    // x: 一辺の長さ（m）、y: ガンマ、z: 反転（0 / 1）、w: 縁のぼかし（m。Area）
    float4 params;
    // x: 縁のずれ（m。Area。正で広がる）、yzw: 未使用
    float4 params2;
};

ConstantBuffer<PathMaskConstants> g_path : register(b1);

// ガンマと反転。両方のエントリの最後で掛ける。
float FinishPathValue(float value)
{
    value = pow(saturate(value), max(g_path.params.y, 1e-3f));
    if (g_path.params.z != 0.0f)
    {
        value = 1.0f - value;
    }
    return saturate(value);
}

// 線分の間引き。8×8 テクセルのまとまりごとに、線分を 64 本ずつ読んで「幅 + フェザーの
// 範囲がまとまりに届く」ものだけを共有メモリの一覧へ集め、テクセルはその一覧とだけ比べる。
// 線分が数千本（細かく探した登山道や蛇行）になっても、遠くの線分はまとめて素通りできる。
// 結果は全部と比べたときと同じ（届かない線分は値 0 なので max に効かない）。
#define TG_PATH_CULL_BATCH 64u
groupshared uint g_pathCullCount;
groupshared uint g_pathCullIndices[TG_PATH_CULL_BATCH];

[numthreads(8, 8, 1)]
void CsPath(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
            uint groupIndex : SV_GroupIndex)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_path.indices.y;
    // 共有メモリの同期をまたぐので、範囲外のテクセルも最後まで一緒に回す（書き込みだけ省く）。
    const bool inside = texel.x < resolution && texel.y < resolution;

    RWTexture2D<float> output = ResourceDescriptorHeap[g_path.indices.x];
    ByteAddressBuffer segments = ResourceDescriptorHeap[g_path.indices.w];

    const float sizeMeters = max(g_path.params.x, 1e-3f);
    const float texelMeters = sizeMeters / float(resolution);
    const float2 position = ((float2(texel) + 0.5f) / float(resolution)) * sizeMeters;
    const float2 groupMin = float2(groupId.xy * 8u) * texelMeters;
    const float2 groupMax = groupMin + 8.0f * texelMeters;

    float value = 0.0f;
    const uint count = g_path.indices.z;
    [loop]
    for (uint start = 0; start < count; start += TG_PATH_CULL_BATCH)
    {
        if (groupIndex == 0u)
        {
            g_pathCullCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        const uint candidate = start + groupIndex;
        if (candidate < count)
        {
            const PathSegmentData segment = LoadSegment(segments, candidate, sizeMeters);
            const float reach = max(segment.widthA, segment.widthB) * 0.5f +
                                max(segment.featherA, segment.featherB);
            const float2 low = min(segment.a, segment.b) - reach;
            const float2 high = max(segment.a, segment.b) + reach;
            if (all(low <= groupMax) && all(high >= groupMin))
            {
                uint slot;
                InterlockedAdd(g_pathCullCount, 1u, slot);
                g_pathCullIndices[slot] = candidate;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        const uint survivors = g_pathCullCount;
        [loop]
        for (uint k = 0; k < survivors; ++k)
        {
            const PathSegmentData segment = LoadSegment(segments, g_pathCullIndices[k], sizeMeters);
            const float2 ab = segment.b - segment.a;
            const float lengthSq = dot(ab, ab);
            // 長さ 0（孤立した点）は円。t = 0 で a との距離になる。
            const float t = (lengthSq > 1e-8f) ? saturate(dot(position - segment.a, ab) / lengthSq)
                                               : 0.0f;
            const float distance = length(position - (segment.a + ab * t));

            const float width = lerp(segment.widthA, segment.widthB, t);
            const float feather = lerp(segment.featherA, segment.featherB, t);
            const float intensity = lerp(segment.intensityA, segment.intensityB, t);
            value = max(value, PathDistanceValue(distance, width, feather) * saturate(intensity));
        }
        // 次のまとまりの一覧を書く前に、全員が読み終わるのを待つ。
        GroupMemoryBarrierWithGroupSync();
    }

    if (inside)
    {
        output[texel] = FinishPathValue(value);
    }
}

// 面。閉じた鎖の多角形の内側を 1 にする。
//
// 内側の判定は交差数の偶奇（テクセルから +x へ伸ばした半直線が線分を横切る回数）。
// 輪の中に輪があれば穴になるので、「この範囲からここを除く」がパスだけで表現できる。
// 縁は多角形までの距離を符号付きにして（内側が負）、縁のずれを引き、ぼかしで 0 へ落とす。
[numthreads(8, 8, 1)]
void CsArea(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_path.indices.y;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_path.indices.x];
    ByteAddressBuffer segments = ResourceDescriptorHeap[g_path.indices.w];

    const float sizeMeters = max(g_path.params.x, 1e-3f);
    const float2 position = ((float2(texel) + 0.5f) / float(resolution)) * sizeMeters;

    uint crossings = 0u;
    float nearest = 1e30f;
    const uint count = g_path.indices.z;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const PathSegmentData segment = LoadSegment(segments, i, sizeMeters);
        const float2 a = segment.a;
        const float2 b = segment.b;
        const float2 ab = b - a;
        const float lengthSq = dot(ab, ab);
        if (lengthSq <= 1e-8f)
        {
            continue;
        }
        // 最寄りの距離（縁のぼかし用）。
        const float t = saturate(dot(position - a, ab) / lengthSq);
        nearest = min(nearest, length(position - (a + ab * t)));
        // 半直線との交差。片端だけを含める（頂点をちょうど通ったとき二重に数えない）。
        if ((a.y > position.y) != (b.y > position.y))
        {
            const float x = a.x + (position.y - a.y) / (b.y - a.y) * (b.x - a.x);
            if (x > position.x)
            {
                ++crossings;
            }
        }
    }

    const bool inside = (crossings & 1u) != 0u;
    const float signedDistance = inside ? -nearest : nearest;
    // 縁のずれ。正で広がる（外側の距離がそのぶん 0 扱いになる）。
    const float edge = signedDistance - g_path.params2.x;
    const float feather = g_path.params.w;
    float value = 0.0f;
    if (edge <= 0.0f)
    {
        value = 1.0f;
    }
    else if (feather > 1e-4f)
    {
        value = saturate(1.0f - edge / feather);
    }

    output[texel] = FinishPathValue(value);
}
