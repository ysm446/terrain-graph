// メッシュの足跡をマスクにする（Mask Mesh ノード）。
//
// 道路などのメッシュの三角形を地形平面へ投影したもの（正規化 UV。アプリが
// MeshFootprintStore へ置く）をバッファ（ByteAddressBuffer）で受ける。各テクセルから
// 三角形群までの距離を取り、三角形の内側（と外側の余白まで）は 1、その外側はフェザーで
// 0 へ落とす。幅が一様でないメッシュ（車線の増減、駐車場、路肩の張り出し）でも、
// メッシュにある形がそのまま出る。
//
// 線分の間引き（CompositeMaskPath.hlsl の CsPath）と同じく、8×8 テクセルのまとまりごとに
// 「余白 + フェザーの範囲がまとまりに届く」三角形だけを共有メモリの一覧へ集め、テクセルは
// その一覧とだけ比べる。道路 1 本で三角形は数万枚になるので、遠くの三角形はまとめて素通りする。

#include "CompositeCommon.hlsli"

// 三角形 1 枚は float 12 個（48 バイト）。C++ 側の kMeshTriangleStride と一致させること。
//   [0..5]: a.xy, b.xy, c.xy（正規化 UV）
//   [6..8]: 頂点の正規化ハイト（地形の均しが使う。ここでは読まない）
//   [9..11]: 未使用
#define TG_MESH_TRIANGLE_BYTES 48u

struct MeshMaskConstants
{
    // x: 出力 UAV、y: 出力の一辺、z: 三角形数、w: 三角形バッファの SRV
    uint4 indices;
    // x: 一辺の長さ（m）、y: ガンマ、z: 反転（0 / 1）、w: フェザー（m）
    float4 params;
    // x: 余白（m）、yzw: 未使用
    float4 params2;
};

ConstantBuffer<MeshMaskConstants> g_mesh : register(b1);

struct MeshTriangle
{
    float2 a;
    float2 b;
    float2 c;
};

// 座標は一辺の長さ（m）を掛けて実寸にして返す。
MeshTriangle LoadTriangle(ByteAddressBuffer buffer, uint index, float sizeMeters)
{
    const uint base = index * TG_MESH_TRIANGLE_BYTES;
    const float4 ab = asfloat(buffer.Load4(base));
    const float2 c = asfloat(buffer.Load2(base + 16u));
    MeshTriangle tri;
    tri.a = ab.xy * sizeMeters;
    tri.b = ab.zw * sizeMeters;
    tri.c = c * sizeMeters;
    return tri;
}

float SegmentDistance(float2 position, float2 a, float2 b)
{
    const float2 ab = b - a;
    const float lengthSq = dot(ab, ab);
    const float t = (lengthSq > 1e-8f) ? saturate(dot(position - a, ab) / lengthSq) : 0.0f;
    return length(position - (a + ab * t));
}

// 三角形までの距離。内側なら 0。
float TriangleDistance(float2 position, MeshTriangle tri)
{
    const float2 pa = position - tri.a;
    const float2 pb = position - tri.b;
    const float2 pc = position - tri.c;
    const float2 ab = tri.b - tri.a;
    const float2 bc = tri.c - tri.b;
    const float2 ca = tri.a - tri.c;
    // 辺ごとの符号。3 つが同じ符号なら内側（向きはどちらでもよい）。
    const float sa = ab.x * pa.y - ab.y * pa.x;
    const float sb = bc.x * pb.y - bc.y * pb.x;
    const float sc = ca.x * pc.y - ca.y * pc.x;
    const bool inside = (sa >= 0.0f && sb >= 0.0f && sc >= 0.0f) ||
                        (sa <= 0.0f && sb <= 0.0f && sc <= 0.0f);
    if (inside)
    {
        return 0.0f;
    }
    return min(SegmentDistance(position, tri.a, tri.b),
               min(SegmentDistance(position, tri.b, tri.c),
                   SegmentDistance(position, tri.c, tri.a)));
}

#define TG_MESH_CULL_BATCH 64u
groupshared uint g_meshCullCount;
groupshared uint g_meshCullIndices[TG_MESH_CULL_BATCH];

[numthreads(8, 8, 1)]
void CsMesh(uint3 dispatchThreadId : SV_DispatchThreadID, uint3 groupId : SV_GroupID,
            uint groupIndex : SV_GroupIndex)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_mesh.indices.y;
    // 共有メモリの同期をまたぐので、範囲外のテクセルも最後まで一緒に回す（書き込みだけ省く）。
    const bool inside = texel.x < resolution && texel.y < resolution;

    RWTexture2D<float> output = ResourceDescriptorHeap[g_mesh.indices.x];
    ByteAddressBuffer triangles = ResourceDescriptorHeap[g_mesh.indices.w];

    const float sizeMeters = max(g_mesh.params.x, 1e-3f);
    const float texelMeters = sizeMeters / float(resolution);
    const float2 position = ((float2(texel) + 0.5f) / float(resolution)) * sizeMeters;
    const float2 groupMin = float2(groupId.xy * 8u) * texelMeters;
    const float2 groupMax = groupMin + 8.0f * texelMeters;

    const float margin = g_mesh.params2.x;
    const float feather = g_mesh.params.w;
    // 届く範囲。これより遠い三角形は値 0 なので比べなくてよい。
    const float reach = margin + feather;

    float nearest = 1e30f;
    const uint count = g_mesh.indices.z;
    [loop]
    for (uint start = 0; start < count; start += TG_MESH_CULL_BATCH)
    {
        if (groupIndex == 0u)
        {
            g_meshCullCount = 0u;
        }
        GroupMemoryBarrierWithGroupSync();
        const uint candidate = start + groupIndex;
        if (candidate < count)
        {
            const MeshTriangle tri = LoadTriangle(triangles, candidate, sizeMeters);
            const float2 low = min(tri.a, min(tri.b, tri.c)) - reach;
            const float2 high = max(tri.a, max(tri.b, tri.c)) + reach;
            if (all(low <= groupMax) && all(high >= groupMin))
            {
                uint slot;
                InterlockedAdd(g_meshCullCount, 1u, slot);
                g_meshCullIndices[slot] = candidate;
            }
        }
        GroupMemoryBarrierWithGroupSync();
        const uint survivors = g_meshCullCount;
        [loop]
        for (uint k = 0; k < survivors; ++k)
        {
            const MeshTriangle tri = LoadTriangle(triangles, g_meshCullIndices[k], sizeMeters);
            nearest = min(nearest, TriangleDistance(position, tri));
        }
        // 次のまとまりの一覧を書く前に、全員が読み終わるのを待つ。
        GroupMemoryBarrierWithGroupSync();
    }

    if (inside)
    {
        // 余白の内側は 1、その外側をフェザーで 0 へ。
        const float edge = nearest - margin;
        float value = 0.0f;
        if (edge <= 0.0f)
        {
            value = 1.0f;
        }
        else if (feather > 1e-4f)
        {
            value = saturate(1.0f - edge / feather);
        }
        value = pow(saturate(value), max(g_mesh.params.y, 1e-3f));
        if (g_mesh.params.z != 0.0f)
        {
            value = 1.0f - value;
        }
        output[texel] = saturate(value);
    }
}
