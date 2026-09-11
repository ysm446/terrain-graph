#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint noiseIndex; uint outputIndex; uint2 padding;
};
[numthreads(8,4,4)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    if (any(id >= uint3(64,32,64))) return;
    // 格子点を範囲の両端へ置き、特に雲底の影をクランプで薄めない。
    float3 uvw=float3(id)/float3(63,31,63);
    float3 position=CloudRenderCenter(settings)+(uvw*2-1)*CloudRenderRadii(settings);
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[outputIndex];
    float sun=CloudOpticalDepth(position,AtmosphereSun(settings),settings,noiseIndex,max(settings.samples/2,16u));
    float top=CloudOpticalDepth(position,float3(0,1,0),settings,noiseIndex,8);
    float bottom=CloudOpticalDepth(position,float3(0,-1,0),settings,noiseIndex,8);
    output[id]=float4(sun,top,bottom,0);
}
