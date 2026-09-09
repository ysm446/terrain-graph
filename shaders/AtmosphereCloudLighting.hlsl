#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint outputIndex; uint lutIndex; uint2 pad;
};
// 雲の高さで上下半球の平均輝度を求める。天頂 1 点で空全体を代表させない。
[numthreads(1,1,1)]
void CsMain(uint3 id:SV_DispatchThreadID) {
    AtmosphericParameters observer=settings;
    observer.altitude+=settings.cloudBottom+0.5*settings.cloudThickness;
    float3 upper=0,lower=0;
    [loop] for(uint i=0;i<32;++i) {
        float y=(i+0.5)/32.0;
        float phi=i*2.39996323;
        float r=sqrt(1-y*y);
        float3 direction=float3(r*cos(phi),y,r*sin(phi));
        upper+=AtmosphericSky(direction,observer,lutIndex);
        direction.y=-direction.y;
        lower+=AtmosphericSky(direction,observer,lutIndex);
    }
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    output[uint2(0,0)]=float4(upper/32,1);
    output[uint2(1,0)]=float4(lower/32,1);
}
