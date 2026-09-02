// 鏡面 IBL 用のプリフィルタ済みキューブマップを、ラフネス別のミップとして作る。
// GGX の重点サンプリングを使い、サンプルの立体角に応じたミップから引くことで
// ファイアフライを抑える。

#include "EnvCommon.hlsli"
#include "Brdf.hlsli"

struct PrefilterConstants
{
    uint sourceIndex;  // 元のキューブの SRV（ミップ付き）
    uint outputIndex;  // 出力ミップの UAV
    uint faceSize;     // 出力ミップの面サイズ
    uint sourceSize;   // 元キューブのミップ 0 の面サイズ
    uint sampleCount;
    float roughness;
};

ConstantBuffer<PrefilterConstants> g_constants : register(b0);

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

    // 視線方向は法線と一致すると仮定する（一般的な近似）。
    const float3 viewDirection = normal;

    // 1 テクセルが張る立体角。ミップ選択に使う。
    const float texelSolidAngle =
        4.0f * kPi / (6.0f * float(g_constants.sourceSize) * float(g_constants.sourceSize));

    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;

    for (uint i = 0; i < g_constants.sampleCount; ++i)
    {
        const float2 xi = Hammersley(i, g_constants.sampleCount);
        const float3 halfVector = ImportanceSampleGGX(xi, normal, g_constants.roughness);
        const float3 lightDirection = normalize(2.0f * dot(viewDirection, halfVector) * halfVector -
                                                viewDirection);

        const float nDotL = dot(normal, lightDirection);
        if (nDotL <= 0.0f)
        {
            continue;
        }

        const float nDotH = saturate(dot(normal, halfVector));
        const float vDotH = saturate(dot(viewDirection, halfVector));

        const float d = DistributionGGX(nDotH, g_constants.roughness);
        const float pdf = (d * nDotH / max(4.0f * vDotH, 1e-5f)) + 1e-5f;
        const float sampleSolidAngle = 1.0f / (float(g_constants.sampleCount) * pdf);

        // ミップ 0（roughness = 0）は鏡面をそのまま写す意図的な特別扱い。
        // シェーディング側の下限 kMinPerceptualRoughness とは独立で、
        // 下限未満のラフネスはミップ 0 と 1 の間の補間としてしか参照されない。
        const float mipLevel = (g_constants.roughness <= 0.0f)
                                   ? 0.0f
                                   : 0.5f * log2(sampleSolidAngle / texelSolidAngle);

        sum += source.SampleLevel(g_samplerLinearClamp, lightDirection, max(mipLevel, 0.0f)).rgb *
               nDotL;
        weightSum += nDotL;
    }

    const float3 prefiltered = (weightSum > 0.0f) ? (sum / weightSum) : float3(0.0f, 0.0f, 0.0f);
    output[uint3(dispatchThreadId.xy, face)] = float4(prefiltered, 1.0f);
}
