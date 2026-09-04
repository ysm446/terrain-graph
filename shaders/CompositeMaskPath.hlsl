// パスの足跡をマスクにする（Mask Path ノード）。
//
// 線分の列（両端の座標・幅・フェザー・強さ）を定数バッファで受け、各テクセルから
// 最寄りの線分までの距離を取る。幅の内側は 1、外側はフェザーで 0 へ落とし、
// 線分どうしは max で重ねる。terrain-editor の Mask Path と同じ形。
//
// 線分は定数バッファに入るぶんずつ（TG_PATH_MAX_SEGMENTS 本）流し、
// 2 回目以降は前の結果と max を取る（indices.w）。座標は地形平面の正規化 UV で、
// 距離は一辺の長さ（m）を掛けて実寸にしてから幅と比べる。

#include "CompositeCommon.hlsli"

// C++ 側の kMaxPathSegments と一致させること。
#define TG_PATH_MAX_SEGMENTS 256

struct PathMaskConstants
{
    // x: 出力 UAV、y: 出力の一辺、z: 線分数、w: 0 = 上書き / 1 = 前の結果と max
    uint4 indices;
    // x: 一辺の長さ（m）、y: ガンマ、z: 反転（0 / 1）、w: 未使用
    float4 params;
    // 線分 1 本につき 3 つ。
    //   [0]: ax, ay, bx, by（正規化 UV）
    //   [1]: widthA, widthB, featherA, featherB（m）
    //   [2]: intensityA, intensityB, 未使用, 未使用
    float4 segments[TG_PATH_MAX_SEGMENTS * 3];
};

ConstantBuffer<PathMaskConstants> g_path : register(b1);

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

[numthreads(8, 8, 1)]
void CsPath(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_path.indices.y;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_path.indices.x];

    const float sizeMeters = max(g_path.params.x, 1e-3f);
    const float2 position = ((float2(texel) + 0.5f) / float(resolution)) * sizeMeters;

    float value = 0.0f;
    const uint count = min(g_path.indices.z, uint(TG_PATH_MAX_SEGMENTS));
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const float4 ends = g_path.segments[i * 3u + 0u] * sizeMeters;
        const float4 widths = g_path.segments[i * 3u + 1u];
        const float4 intensities = g_path.segments[i * 3u + 2u];

        const float2 a = ends.xy;
        const float2 ab = ends.zw - a;
        const float lengthSq = dot(ab, ab);
        // 長さ 0（孤立した点）は円。t = 0 で a との距離になる。
        const float t = (lengthSq > 1e-8f) ? saturate(dot(position - a, ab) / lengthSq) : 0.0f;
        const float distance = length(position - (a + ab * t));

        const float width = lerp(widths.x, widths.y, t);
        const float feather = lerp(widths.z, widths.w, t);
        const float intensity = lerp(intensities.x, intensities.y, t);
        value = max(value, PathDistanceValue(distance, width, feather) * saturate(intensity));
    }

    // 2 回目以降のバッチは前の結果と重ねる（ガンマと反転は最後のバッチで掛ける。
    // 呼び出し側は途中のバッチでガンマ 1 / 反転なしを渡す）。
    if (g_path.indices.w != 0u)
    {
        value = max(value, output[texel]);
    }
    value = pow(saturate(value), max(g_path.params.y, 1e-3f));
    if (g_path.params.z != 0.0f)
    {
        value = 1.0f - value;
    }
    output[texel] = saturate(value);
}
