// グラフのノードに出す、合成結果のサムネイル。
//
// そのレイヤーまで合成した BaseColor（アルベド）に、同じ時点の Height の勾配で陰影を
// 付けて 64² へ落とす。繋ぎ替えずに「見た目がどうなったか」を読むためのもので、
// ラフネスや Normal チャンネルの細部、天球の照明は使わない（64px では読めないし、
// 固定の光のほうがノードどうしを見比べやすい）。
//
// 合成解像度 → 64² はセルの平均で落とす（DownsampleHeight と同じ。間引くと材質の
// 凹凸がエイリアスする）。法線は落とした後の粗いハイトの隣接差から作る。

#include "CompositeCommon.hlsli"

struct ThumbnailConstants
{
    // x: BaseColor の SRV、y: Height の SRV、z: 出力 UAV、w: 出力の一辺
    uint4 indices;
    // x: 一辺の長さ（m）、y: 標高差（m）、zw: 未使用
    float4 params;
};

ConstantBuffer<ThumbnailConstants> g_thumbnail : register(b1);

// セルの平均色。DownsampleHeight の色版。
float3 DownsampleColor(Texture2D<float3> source, uint2 cell, uint gridResolution)
{
    uint sourceWidth = 0u;
    uint sourceHeight = 0u;
    source.GetDimensions(sourceWidth, sourceHeight);
    if (gridResolution >= sourceWidth)
    {
        const float2 uv = (float2(cell) + 0.5f) / float(gridResolution);
        return source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    }
    const float ratio = float(sourceWidth) / float(gridResolution);
    const int2 begin = int2(floor(float2(cell) * ratio));
    int2 end = int2(floor(float2(cell + 1u) * ratio));
    end = max(end, begin + int2(1, 1));
    end = min(end, int2(sourceWidth, sourceHeight));
    float3 sum = 0.0f;
    [loop]
    for (int y = begin.y; y < end.y; ++y)
    {
        [loop]
        for (int x = begin.x; x < end.x; ++x)
        {
            sum += source.Load(int3(x, y, 0));
        }
    }
    const int2 extent = end - begin;
    return sum / float(max(extent.x * extent.y, 1));
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    const uint resolution = g_thumbnail.indices.w;
    if (cell.x >= resolution || cell.y >= resolution) { return; }

    Texture2D<float3> baseColor = ResourceDescriptorHeap[g_thumbnail.indices.x];
    Texture2D<float> height = ResourceDescriptorHeap[g_thumbnail.indices.y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_thumbnail.indices.z];

    const float3 albedo = DownsampleColor(baseColor, cell, resolution);

    // 粗いハイトの隣接差から法線。u が +x、v が +z（合成の法線と同じ向き）。
    const int limit = int(resolution) - 1;
    const uint2 left = uint2(uint(max(int(cell.x) - 1, 0)), cell.y);
    const uint2 right = uint2(uint(min(int(cell.x) + 1, limit)), cell.y);
    const uint2 up = uint2(cell.x, uint(max(int(cell.y) - 1, 0)));
    const uint2 down = uint2(cell.x, uint(min(int(cell.y) + 1, limit)));
    const float sizeMeters = max(g_thumbnail.params.x, 1e-3f);
    const float heightMeters = max(g_thumbnail.params.y, 1e-3f);
    const float cellMeters = sizeMeters / float(resolution);
    const float dhdx = (DownsampleHeight(height, right, resolution) -
                        DownsampleHeight(height, left, resolution)) * heightMeters /
                       (float(right.x - left.x) * cellMeters);
    const float dhdz = (DownsampleHeight(height, down, resolution) -
                        DownsampleHeight(height, up, resolution)) * heightMeters /
                       (float(down.y - up.y) * cellMeters);
    const float3 normal = normalize(float3(-dhdx, 1.0f, -dhdz));

    // 固定の 1 灯。左上（画像の上 = -z）の前方から。少しの環境光で影を潰さない。
    const float3 lightDirection = normalize(float3(-0.50f, 0.72f, -0.48f));
    const float shade = saturate(dot(normal, lightDirection)) * 0.85f + 0.15f;

    output[cell] = float4(LinearToSrgb(saturate(albedo * shade)), 1.0f);
}
