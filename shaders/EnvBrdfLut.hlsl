// 分割和近似の第 2 項（環境 BRDF）を LUT に焼く。
// x = NdotV、y = roughness。RG に (スケール, バイアス) を入れる。
// シェーディング側では F0 * scale + bias を掛ける。

#include "EnvCommon.hlsli"
#include "Brdf.hlsli"

struct BrdfLutConstants
{
    uint outputIndex;
    uint size;
    uint sampleCount;
};

ConstantBuffer<BrdfLutConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.size || dispatchThreadId.y >= g_constants.size)
    {
        return;
    }

    RWTexture2D<float2> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float(g_constants.size);
    const float nDotV = max(uv.x, 1e-3f);
    // シェーディング側はラフネスを kMinPerceptualRoughness 未満にしないので、
    // LUT の下端の行も同じ下限で焼く。極小ラフネスでの数値不安定も避けられる。
    const float roughness = max(uv.y, kMinPerceptualRoughness);

    const float3 viewDirection = float3(sqrt(1.0f - nDotV * nDotV), 0.0f, nDotV);
    const float3 normal = float3(0.0f, 0.0f, 1.0f);

    float scale = 0.0f;
    float bias = 0.0f;

    for (uint i = 0; i < g_constants.sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, g_constants.sampleCount);
        const float3 halfVector = ImportanceSampleGGX(xi, normal, roughness);
        const float3 lightDirection = normalize(2.0f * dot(viewDirection, halfVector) * halfVector -
                                                viewDirection);

        const float nDotL = lightDirection.z;
        if (nDotL <= 0.0f)
        {
            continue;
        }

        const float nDotH = saturate(halfVector.z);
        const float vDotH = saturate(dot(viewDirection, halfVector));

        const float g = GeometrySmithIbl(nDotV, nDotL, roughness);
        const float gVis = g * vDotH / max(nDotH * nDotV, 1e-5f);
        const float fc = pow(1.0f - vDotH, 5.0f);

        scale += (1.0f - fc) * gVis;
        bias += fc * gVis;
    }

    const float inverseCount = 1.0f / float(max(g_constants.sampleCount, 1u));
    output[dispatchThreadId.xy] = float2(scale * inverseCount, bias * inverseCount);
}
