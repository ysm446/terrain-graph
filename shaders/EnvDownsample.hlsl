// キューブマップのミップを 1 段作る。
// プリフィルタの際に粗いミップから引くことで、サンプル数を抑えつつノイズを減らせる。

#include "EnvCommon.hlsli"

struct DownsampleConstants
{
    uint sourceIndex;  // 参照元ミップだけを見るキューブ SRV
    uint outputIndex;  // 出力ミップの UAV
    uint outputSize;   // 出力ミップの面サイズ
};

ConstantBuffer<DownsampleConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.outputSize || dispatchThreadId.y >= g_constants.outputSize)
    {
        return;
    }

    TextureCube<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const uint face = dispatchThreadId.z;

    // 出力テクセルに対応する 2x2 の方向を平均する。
    const float texel = 1.0f / float(g_constants.outputSize);
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            const float2 offset = (float2(x, y) + 0.5f) * 0.5f - 0.5f;
            const float2 uv = ((float2(dispatchThreadId.xy) + 0.5f) * texel + offset * texel) *
                                  2.0f - 1.0f;
            const float3 direction = CubeFaceDirection(face, uv);
            // SRV が参照元ミップだけを見ているので、レベルは 0 でよい。
            sum += source.SampleLevel(g_samplerLinearClamp, direction, 0.0f).rgb;
        }
    }

    output[uint3(dispatchThreadId.xy, face)] = float4(sum * 0.25f, 1.0f);
}
