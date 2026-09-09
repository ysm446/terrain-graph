#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    float4x4 inverseViewProjection;
    float3 camera; uint showSky;
    AtmosphericParameters settings;
    uint depthIndex; uint lutIndex; uint noiseIndex; uint environmentIndex;
};
struct Vertex { float4 position:SV_Position; float2 ndc:TEXCOORD0; };
Vertex VsMain(uint id:SV_VertexID) {
    Vertex v; float2 uv=float2((id<<1)&2,id&2);
    v.ndc=uv*float2(2,-2)+float2(-1,1); v.position=float4(v.ndc,0,1); return v;
}
float4 PsMain(Vertex v):SV_Target {
    Texture2D<float> depth=ResourceDescriptorHeap[depthIndex];
    float z=depth.Load(int3(v.position.xy,0));
    float4 world=mul(inverseViewProjection,float4(v.ndc,z < 1 ? z : 0.99999,1));
    world.xyz/=world.w;
    float3 ray=normalize(world.xyz-camera);
    float limit=z<1 ? length(world.xyz-camera) : 1e9;
    float3 origin=camera;
    Texture2D<float4> skyView=ResourceDescriptorHeap[lutIndex];
    float4 cloud=IntegrateCloud(origin,ray,limit,settings,noiseIndex,environmentIndex);
    if(z>=1 && showSky!=0) {
        float3 sky=skyView.SampleLevel(g_samplerEquirect,DirectionToEquirectUv(ray),0).rgb;
        float3 sun=AtmosphereSun(settings);
        float angularRadius=0.00465;
        float angle=acos(clamp(dot(ray,sun),-1,1));
        float disc=1-smoothstep(angularRadius-fwidth(angle),angularRadius+fwidth(angle),angle);
        float3 sunlight=AtmComputeSunTransmittance(sun,settings.density,settings.mie,settings.altitude);
        // RGBA16F の範囲を守る。直接光・IBL の積分値はクランプしない。
        sky+=min(sunlight*settings.illuminance/(3.14159265*angularRadius*angularRadius),60000)*disc;
        return float4(min(sky*cloud.a+cloud.rgb,65000),1);
    }
    float opacity=1-cloud.a;
    return float4(cloud.rgb/max(opacity,1e-5),opacity);
}
