// 手続き的な空を equirectangular マップへ書き出す。
// HDRI を読み込んでいないときの既定環境として使う。

#include "EnvCommon.hlsli"

struct SkyConstants
{
    uint outputIndex;
    uint width;
    uint height;
    float intensity;   // cd/m^2 相当

    float3 zenithColor;
    float pad0;

    float3 horizonColor;
    float pad1;

    float3 groundColor;
    float pad2;
};

ConstantBuffer<SkyConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.width || dispatchThreadId.y >= g_constants.height)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) /
                      float2(g_constants.width, g_constants.height);
    const float3 direction = EquirectUvToDirection(uv);

    const float3 radiance = EvaluateProceduralSky(direction, g_constants.zenithColor,
                                                  g_constants.horizonColor,
                                                  g_constants.groundColor, g_constants.intensity);

    output[dispatchThreadId.xy] = float4(radiance, 1.0f);
}
