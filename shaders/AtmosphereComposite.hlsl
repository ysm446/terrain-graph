#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    float4x4 inverseViewProjection;
    float3 camera; uint showSky;
    AtmosphericParameters settings;
    uint depthIndex; uint lutIndex; uint noiseIndex; uint environmentIndex;
    uint halfCloudIndex; uint halfDepthIndex; uint2 fullSize;
    uint halfCloudOutput; uint halfDepthOutput; uint2 padding;
};
struct Vertex { float4 position:SV_Position; float2 ndc:TEXCOORD0; };
Vertex VsMain(uint id:SV_VertexID) {
    Vertex v; float2 uv=float2((id<<1)&2,id&2);
    v.ndc=uv*float2(2,-2)+float2(-1,1); v.position=float4(v.ndc,0,1); return v;
}
float3 CloudViewRay(float2 pixel, float z, out float limit) {
    float2 ndc=pixel/float2(fullSize)*float2(2,-2)+float2(-1,1);
    float4 world=mul(inverseViewProjection,float4(ndc,z<1 ? z : 0.99999,1));
    world.xyz/=world.w;
    limit=z<1 ? length(world.xyz-camera) : 1e9;
    return normalize(world.xyz-camera);
}
// 2x2 画素の左上を代表点とする。合成側も同じ格子で補間する。
[numthreads(8,8,1)]
void CsCloudHalf(uint3 id:SV_DispatchThreadID) {
    if (any(id.xy>=(fullSize+1)/2)) return;
    Texture2D<float> depth=ResourceDescriptorHeap[depthIndex];
    uint2 pixel=min(id.xy*2,fullSize-1);
    float z=depth.Load(int3(pixel,0));
    float limit;
    float3 ray=CloudViewRay(float2(pixel)+0.5,z,limit);
    RWTexture2D<float4> output=ResourceDescriptorHeap[halfCloudOutput];
    RWTexture2D<float> outputDepth=ResourceDescriptorHeap[halfDepthOutput];
    output[id.xy]=IntegrateCloud(camera,ray,limit,settings,noiseIndex,environmentIndex);
    outputDepth[id.xy]=limit;
}
float4 ReconstructCloud(float2 pixel, float3 ray, float limit) {
    if (settings.clouds==0) return float4(0,0,0,1);
    if (halfCloudIndex==0xffffffff)
        return IntegrateCloud(camera,ray,limit,settings,noiseIndex,environmentIndex);
    Texture2D<float4> clouds=ResourceDescriptorHeap[halfCloudIndex];
    Texture2D<float> depths=ResourceDescriptorHeap[halfDepthIndex];
    float2 low=(pixel-0.5)*0.5;
    int2 base=int2(floor(low));
    float2 fraction=frac(low);
    float4 result=0;
    bool compatible=true;
    [unroll] for(int y=0;y<2;++y) [unroll] for(int x=0;x<2;++x) {
        int2 coord=clamp(base+int2(x,y),int2(0,0),int2((fullSize+1)/2)-1);
        float sampleDepth=depths.Load(int3(coord,0));
        // 空と地形の境界、または奥行きの急変は補間せずフル解像度で再評価。
        compatible=compatible && ((sampleDepth==1e9)==(limit==1e9))
            && abs(sampleDepth-limit)<=max(1.0,limit*0.01);
        float weight=(x ? fraction.x : 1-fraction.x)*(y ? fraction.y : 1-fraction.y);
        result+=clouds.Load(int3(coord,0))*weight;
    }
    if (!compatible) return IntegrateCloud(camera,ray,limit,settings,noiseIndex,environmentIndex);
    return result;
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
    float4 cloud=ReconstructCloud(v.position.xy,ray,limit);
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
