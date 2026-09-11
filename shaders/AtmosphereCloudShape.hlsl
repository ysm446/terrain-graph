#define TG_BAKE_CLOUD_SHAPE
#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint outputIndex; uint3 padding;
};
[numthreads(4,4,4)]
void CsMain(uint3 id : SV_DispatchThreadID) {
    if (any(id >= settings.shapeCacheSize)) return;
    float3 uvw=float3(id)/float3(settings.shapeCacheSize-1);
    float3 position=LocalCloudCenter(settings)+(uvw*2-1)*LocalCloudRadii(settings);
    float lower;
    float distance=ProceduralCloudShape(position,settings,lower);
    RWTexture2DArray<float2> output=ResourceDescriptorHeap[outputIndex];
    output[id]=float2(distance,lower);
}
