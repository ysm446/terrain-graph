// 拡散 IBL 用の irradiance キューブマップを作る。
//
// 余弦重み付きサンプリングでは pdf = cos / pi なので、
//   E = (1/N) * sum( L_i * cos_i / pdf_i ) = pi * mean(L)
// となる。ここには E / pi = mean(L)、つまり平均放射輝度を格納する。
// シェーディング側では diffuseColor を掛けるだけでよい。

#include "EnvCommon.hlsli"
#include "Brdf.hlsli"

struct IrradianceConstants
{
    uint sourceIndex;  // 元のキューブの SRV
    uint outputIndex;  // irradiance キューブの UAV
    uint faceSize;
    uint sampleCount;
    uint sourceMip;    // ノイズを抑えるため粗いミップから引く
};

ConstantBuffer<IrradianceConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.faceSize || dispatchThreadId.y >= g_constants.faceSize)
    {
        return;
    }

    TextureCube<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const uint face = dispatchThreadId.z;
    const float3 normal = CubeFaceDirection(face, CubeFaceUv(dispatchThreadId.xy,
                                                             g_constants.faceSize));

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    for (uint i = 0; i < g_constants.sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, g_constants.sampleCount);
        const float3 direction = ImportanceSampleCosine(xi, normal);
        sum += source.SampleLevel(g_samplerLinearClamp, direction,
                                  float(g_constants.sourceMip)).rgb;
    }

    const float3 averageRadiance = sum / float(max(g_constants.sampleCount, 1u));
    output[uint3(dispatchThreadId.xy, face)] = float4(averageRadiance, 1.0f);
}
