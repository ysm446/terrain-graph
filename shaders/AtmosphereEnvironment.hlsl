#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint outputIndex; uint lutIndex; uint noiseIndex; uint skyOutputIndex;
    uint lightingIndex;
};
// 出力は 512×256 の正距円筒（C++ 側の CreateTargets / Dispatch と揃えること）。
static const uint2 kEnvironmentSize = uint2(512, 256);
[numthreads(8,8,1)]
void CsMain(uint3 id:SV_DispatchThreadID) {
    if (any(id.xy >= kEnvironmentSize)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    float3 ray=EquirectUvToDirection((id.xy+0.5)/float2(kEnvironmentSize));
    Texture2D<float4> lighting=ResourceDescriptorHeap[lightingIndex];
    float3 sky=AtmosphericSky(ray,settings,lutIndex,lighting.Load(int3(2,0,0)).rgb);
    RWTexture2D<float4> skyOutput=ResourceDescriptorHeap[skyOutputIndex];
    skyOutput[id.xy]=float4(sky,1);
    float4 cloud=IntegrateCloud(float3(0,0,0),ray,1e9,settings,noiseIndex,lightingIndex);
    // 太陽ディスクは直接光と二重計上しない。背景パスだけで描く。
    output[id.xy]=float4(sky*cloud.a+cloud.rgb,1);
}
