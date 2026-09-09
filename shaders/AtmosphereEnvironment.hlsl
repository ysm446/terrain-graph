#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b0) {
    AtmosphericParameters settings;
    uint outputIndex; uint lutIndex; uint noiseIndex; uint pad;
};
[numthreads(8,8,1)]
void CsMain(uint3 id:SV_DispatchThreadID) {
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    float3 ray=EquirectUvToDirection((id.xy+0.5)/float2(512,256));
    float3 sky=AtmosphericSky(ray,settings,lutIndex);
    RWTexture2D<float4> skyOutput=ResourceDescriptorHeap[pad];
    skyOutput[id.xy]=float4(sky,1);
    float3 ambient=AtmosphericSky(float3(0,1,0),settings,lutIndex);
    float4 cloud=IntegrateCloud(float3(0,0,0),ray,100000,settings,noiseIndex,ambient);
    // 太陽ディスクは直接光と二重計上しない。背景パスだけで描く。
    output[id.xy]=float4(sky*cloud.a+cloud.rgb,1);
}
