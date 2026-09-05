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

// 線分 1 本は float 12 個（48 バイト）。C++ 側の PathSegmentStride と一致させること。
//   [0..3]: ax, ay, bx, by（正規化 UV）
//   [4..7]: widthA, widthB, featherA, featherB（m）
//   [8..11]: intensityA, intensityB, 未使用, 未使用
#define TG_PATH_SEGMENT_BYTES 48u

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

struct PathSegmentData
{
    float2 a;
    float2 b;
    float widthA;
    float widthB;
    float featherA;
    float featherB;
    float intensityA;
    float intensityB;
};

PathSegmentData LoadSegment(ByteAddressBuffer buffer, uint index, float sizeMeters)
{
    const uint base = index * TG_PATH_SEGMENT_BYTES;
    const float4 ends = asfloat(buffer.Load4(base));
    const float4 widths = asfloat(buffer.Load4(base + 16u));
    const float4 intensities = asfloat(buffer.Load4(base + 32u));
    PathSegmentData segment;
    segment.a = ends.xy * sizeMeters;
    segment.b = ends.zw * sizeMeters;
    segment.widthA = widths.x;
    segment.widthB = widths.y;
    segment.featherA = widths.z;
    segment.featherB = widths.w;
    segment.intensityA = intensities.x;
    segment.intensityB = intensities.y;
    return segment;
}

// 中心線からの距離 → 0〜1。幅の半分までは 1、その外側をフェザーで 0 へ。
float PathDistanceValue(float distance, float width, float feather)
{
    const float half = max(width, 0.0f) * 0.5f;
    if (distance <= half)
    {
        return 1.0f;
    }
    if (feather <= 1e-4f)
    {
        return 0.0f;
    }
    return saturate(1.0f - (distance - half) / feather);
}

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

[numthreads(8, 8, 1)]
void CsPath(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_path.indices.y;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_path.indices.x];
    ByteAddressBuffer segments = ResourceDescriptorHeap[g_path.indices.w];

    const float sizeMeters = max(g_path.params.x, 1e-3f);
    const float2 position = ((float2(texel) + 0.5f) / float(resolution)) * sizeMeters;

    float value = 0.0f;
    const uint count = g_path.indices.z;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const PathSegmentData segment = LoadSegment(segments, i, sizeMeters);
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

    output[texel] = FinishPathValue(value);
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
