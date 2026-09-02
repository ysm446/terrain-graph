// 2D テクスチャのミップを 1 段作る。
// 参照元ミップだけを見る SRV を渡すので、サンプルレベルは 0 でよい。

#include "Common.hlsli"

struct MipConstants
{
    uint sourceIndex;
    uint outputIndex;
    uint outputWidth;
    uint outputHeight;
};

ConstantBuffer<MipConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.outputWidth ||
        dispatchThreadId.y >= g_constants.outputHeight)
    {
        return;
    }

    Texture2D<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) /
                      float2(g_constants.outputWidth, g_constants.outputHeight);

    // 線形サンプラで 2x2 を平均する。
    output[dispatchThreadId.xy] = source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}
