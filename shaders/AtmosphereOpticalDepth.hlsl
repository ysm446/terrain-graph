#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint noiseIndex; uint outputIndex; uint2 padding;
};
[numthreads(8,4,4)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    uint n=max(settings.opticalCacheSize&0xffffu,2u);
    uint ny=max(settings.opticalCacheSize>>16,2u);
    if (any(id >= uint3(n,ny,n))) return;
    // 格子点を範囲の両端へ置き、特に雲底の影をクランプで薄めない。
    float3 uvw=float3(id)/float3(n-1,ny-1,n-1);
    float3 position=CloudRenderCenter(settings)+(uvw*2-1)*CloudRenderRadii(settings);
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[outputIndex];
    float sun=CloudOpticalDepth(position,AtmosphereSun(settings),settings,noiseIndex,max(settings.samples/2,16u));
    float top=CloudOpticalDepth(position,float3(0,1,0),settings,noiseIndex,8);
    float bottom=CloudOpticalDepth(position,float3(0,-1,0),settings,noiseIndex,8);
    output[id]=float4(sun,top,bottom,0);
}
