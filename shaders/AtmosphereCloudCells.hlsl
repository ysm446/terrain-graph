#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b0) { uint seed; uint outputIndex; uint2 padding; };
[numthreads(8,8,1)]
void CsMain(uint3 id:SV_DispatchThreadID) {
    if (any(id.xy>=10)) return;
    CloudCellData data=BuildCloudCell(int2(id.xy),seed);
    RWTexture2DArray<float4> output=ResourceDescriptorHeap[outputIndex];
    output[uint3(id.xy,0)]=data.placement;
    output[uint3(id.xy,1)]=data.shape;
    output[uint3(id.xy,2)]=data.lobe;
}
