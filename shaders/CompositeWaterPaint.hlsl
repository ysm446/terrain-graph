// 水チャンネルへ書く（River / Lake）。
//
// 水を張る加工（River / Lake）は、地形の形（Height / Normal）を自分のパスで作る。ここはその後に、
// 水の見た目と水チャンネルを合成解像度で書く。Liquid は合成レイヤーなので CompositeLayer.hlsl が
// 同じことをする（色の式はそちらと揃える）。
//
// 水チャンネルは 2 枚:
//   水の場（RGBA16F）  x = 水際からの符号付き距離（m。CompositeWater.hlsl が後から書く）、
//                      y = 符号付きの水深（m。水の中が正、陸は −水位からの高さ）、
//                      z = 波の強さ（0〜1）、w = 波打ち際を出す度合い（0〜1）
//   流れの場（RGBA16F）xy = 流れの速度（m/s）、z = 水面の被覆、w = 早瀬の度合い
//
// 後から重なる水は、前の水を消さずに引き継ぐ: 被覆と水深は大きいほうを取り、流れは上に乗った水の
// 深さで弱める（浅い所では川の流れが残り、深くなるほど消える。河口や、湖へ注ぐ川がつながる）。
#include "CompositeCommon.hlsli"

struct WaterPaintConstants
{
    uint4 targets;   // BaseColor UAV, Surface UAV, 流れの場 UAV, 水の場 UAV
    uint4 sources;   // 被覆（River）/ 湖の出力（Lake）UAV, 水深（River）UAV, Height UAV, 合成解像度
    uint4 mode;      // 0 = River, 1 = Lake / 色を書く（0 / 1）/ 未使用 x2
    float4 shallow;  // 浅瀬の色 rgb, 色の変わる深さ（m。0 で深い所の色だけ）
    float4 deep;     // 深い所の色 rgb, 底が透ける深さ（m。0 で透けない）
    float4 params;   // ラフネス, 水深の倍率（River: 値 → m）, 波の強さ, ハイト 0〜1 の全幅（m）
};
ConstantBuffer<WaterPaintConstants> g_paint : register(b1);

// 流れを弱める深さ（m）。上に乗った水がこの深さで、流れは約 37% になる。
static const float kFlowFadeDepthMeters = 1.5f;

// 水深（m）と被覆から、色とサーフェスを書く（CompositeLayer.hlsl の Liquid と同じ式）。
void PaintWaterColor(uint2 texel, float cover, float depthMeters)
{
    RWTexture2D<float4> baseColorTarget = ResourceDescriptorHeap[g_paint.targets.x];
    RWTexture2D<float4> surfaceTarget = ResourceDescriptorHeap[g_paint.targets.y];

    float3 color = g_paint.deep.rgb;
    if (g_paint.shallow.w > 0.0f)
    {
        color = lerp(color, g_paint.shallow.rgb, exp(-depthMeters / g_paint.shallow.w));
    }
    const float bedVisibility = (g_paint.deep.w > 0.0f) ? exp(-depthMeters / g_paint.deep.w) : 0.0f;

    baseColorTarget[texel] =
        float4(lerp(baseColorTarget[texel].rgb, color, cover * (1.0f - bedVisibility)), 1.0f);
    const float4 surface = surfaceTarget[texel];
    surfaceTarget[texel] = float4(lerp(surface.rgb, float3(g_paint.params.x, 0.0f, 1.0f), cover), surface.a);
}

[numthreads(8, 8, 1)]
void CsPaint(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= g_paint.sources.ww))
    {
        return;
    }
    const uint2 texel = id.xy;

    if (g_paint.mode.x == 0u)
    {
        // --- River: 被覆と水深は River が合成解像度で焼いたもの。流れの場は CsFlow が書く。 ---
        RWTexture2D<float> coverSource = ResourceDescriptorHeap[g_paint.sources.x];
        RWTexture2D<float> depthSource = ResourceDescriptorHeap[g_paint.sources.y];
        const float cover = saturate(coverSource[texel]);
        if (cover > 0.0f && g_paint.mode.y != 0u)
        {
            PaintWaterColor(texel, cover, max(depthSource[texel], 0.0f) * g_paint.params.y);
        }
        return;
    }

    // --- Lake: 出力は x = 湖の範囲、y = 水深（m）、z = 周囲へ延長した水位（正規化） -------------
    RWTexture2D<float4> lake = ResourceDescriptorHeap[g_paint.sources.x];
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_paint.sources.z];
    RWTexture2D<float4> flowTarget = ResourceDescriptorHeap[g_paint.targets.z];
    RWTexture2D<float4> waterTarget = ResourceDescriptorHeap[g_paint.targets.w];

    const float4 state = lake[texel];
    const float depthMeters = max(state.y, 0.0f);
    // 水際を 3 cm でぼかす（範囲は 0 / 1 なので、そのままだと縁が階段になる）。
    const float cover = smoothstep(0.0f, 0.03f, depthMeters);

    // 符号付きの水深。陸は「延長した水位」からの高さ（寄せる波が這い上がる範囲に使う）。
    const float above = (heightTarget[texel] - state.z) * g_paint.params.w;
    const float signedDepth = clamp((depthMeters > 0.0f) ? depthMeters : -max(above, 0.0f), -9000.0f, 9000.0f);

    float4 water = waterTarget[texel];
    water.y = max(water.y, signedDepth);
    water.z = lerp(water.z, g_paint.params.z, cover);
    water.w = lerp(water.w, 1.0f, cover);
    waterTarget[texel] = water;

    if (cover > 0.0f)
    {
        float4 flow = flowTarget[texel];
        const float keep = lerp(1.0f, exp(-depthMeters / kFlowFadeDepthMeters), cover);
        flow.xy *= keep;
        flow.w *= keep;
        flow.z = max(flow.z, cover);
        flowTarget[texel] = flow;
        if (g_paint.mode.y != 0u)
        {
            PaintWaterColor(texel, cover, depthMeters);
        }
    }
}
