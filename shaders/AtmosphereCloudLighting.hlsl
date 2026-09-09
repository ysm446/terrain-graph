#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    AtmosphericParameters settings;
    uint outputIndex; uint lutIndex; uint2 pad;
};
// 雲の高さで上下半球の平均輝度を求める。天頂 1 点で空全体を代表させない。
[numthreads(1,1,1)]
void CsMain(uint3 id:SV_DispatchThreadID) {
    // 惑星地表の Lambert 反射。単位太陽照度で先に計算し、空・IBL・雲へ共通で渡す。
    AtmosphericParameters ground=settings;
    ground.altitude=0; ground.illuminance=1;
    float3 skyIrradiance=0;
    [loop] for(uint i=0;i<64;++i) {
        float y=(i+0.5)/64.0,phi=i*2.39996323,r=sqrt(1-y*y);
        float3 direction=float3(r*cos(phi),y,r*sin(phi));
        skyIrradiance+=AtmosphericSky(direction,ground,lutIndex)*y*(6.283185307/64);
    }
    float3 sun=AtmosphereSun(settings);
    float3 direct=AtmComputeSunTransmittance(sun,settings.density,settings.mie,0)*max(sun.y,0);
    // 遠景の一様な惑星地表に局所的な雲影を適用しない。
    // 代表点の遮蔽を全下半球へ広げると、雲移流とキャッシュ更新で背景全体が点滅する。
    // 地形の直接光に対する雲影は MeshPbr で位置ごとに評価する。
    float3 groundRadiance=settings.groundAlbedo*(skyIrradiance+direct)/3.141592654;
    AtmosphericParameters observer=settings;
    observer.altitude+=settings.cloudBottom+0.5*settings.cloudThickness;
    float3 upper=0,lower=0;
    [loop] for(uint i=0;i<32;++i) {
        float y=(i+0.5)/32.0;
        float phi=i*2.39996323;
        float r=sqrt(1-y*y);
        float3 direction=float3(r*cos(phi),y,r*sin(phi));
        upper+=AtmosphericSky(direction,observer,lutIndex,groundRadiance);
        direction.y=-direction.y;
        lower+=AtmosphericSky(direction,observer,lutIndex,groundRadiance);
    }
    RWTexture2D<float4> output=ResourceDescriptorHeap[outputIndex];
    output[uint2(0,0)]=float4(upper/32,1);
    output[uint2(1,0)]=float4(lower/32,1);
    output[uint2(2,0)]=float4(groundRadiance,1);
}
