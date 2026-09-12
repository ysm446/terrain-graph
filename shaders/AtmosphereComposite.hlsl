#include "AtmosphereCommon.hlsli"
cbuffer Constants : register(b1) {
    float4x4 inverseViewProjection;
    float3 camera; uint showSky;
    AtmosphericParameters settings;
    uint depthIndex; uint lutIndex; uint noiseIndex; uint environmentIndex;
    uint halfCloudIndex; uint halfDepthIndex; uint2 fullSize;
    uint halfCloudOutput; uint halfDepthOutput; uint farCloudIndex; uint farCloudOutput;
    uint godRays; float rayDensity; float rayDistance; float farDistance;
    float4x4 lightViewProjection;
    uint shadowIndex; float shadowTexelSize; float shadowBias; float shadowPadding;
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
// 空中のサンプルには面の法線がないため、傾斜バイアスを使わない。
float TerrainRayVisibility(float3 position) {
    if (shadowIndex==0xffffffff) return 1;
    float4 clip=mul(lightViewProjection,float4(position,1));
    if (clip.w<=0) return 1;
    float3 ndc=clip.xyz/clip.w;
    float2 uv=ndc.xy*float2(0.5,-0.5)+0.5;
    if (any(uv<0) || any(uv>1) || ndc.z<0 || ndc.z>1) return 1;
    Texture2D<float> shadowMap=ResourceDescriptorHeap[shadowIndex];
    float visibility=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
        float depth=shadowMap.SampleLevel(g_samplerPointClamp,uv+float2(x,y)*shadowTexelSize,0);
        visibility+=(ndc.z-shadowBias<=depth) ? 1.0 : 0.0;
    }
    return visibility/9;
}
// 遠景パスの結果を手前の結果の後ろに合成する。遠景テクスチャは 1/4 解像度でバイリニア補間。
float4 CombineFarCloud(float4 near, float2 pixel, float limit) {
    if (farCloudIndex==0xffffffff || limit<=farDistance || near.a<=0.001) return near;
    Texture2D<float4> far=ResourceDescriptorHeap[farCloudIndex];
    uint2 farSize=(fullSize+3)/4;
    float2 uv=(pixel/float2(fullSize));
    float4 f=far.SampleLevel(g_samplerLinearClamp,uv,0);
    return float4(near.rgb+near.a*f.rgb,near.a*f.a);
}
float4 IntegrateCloudAndRays(float3 ray, float limit, float2 pixel) {
    // 遠景パスがあるときは手前の積分を遠景の開始距離で打ち切り、後で合成する。
    float nearLimit=farCloudIndex!=0xffffffff ? min(limit,farDistance) : limit;
    float4 cloud=IntegrateCloud(camera,ray,nearLimit,settings,noiseIndex,environmentIndex);
    cloud=CombineFarCloud(cloud,pixel,limit);
    float3 sun=AtmosphereSun(settings);
    if (godRays==0 || rayDensity<=0 || sun.y<=0.001) return cloud;
    float start=0, end=min(limit,rayDistance);
    // 雲なしでは視線全体を積分する。雲ありは従来の高さでの分離を保つ。
    if (settings.clouds!=0) {
        if (abs(ray.y)<1e-6) {
            if (camera.y>=settings.cloudBottom) return cloud;
        } else {
            float crossing=(settings.cloudBottom-camera.y)/ray.y;
            if (ray.y>0) end=min(end,crossing);
            else start=max(start,crossing);
        }
    }
    if (end<=start) return cloud;
    float stepLength=(end-start)/48;
    float opacity=1-exp(-rayDensity*stepLength);
    float3 sunlight=AtmComputeSunTransmittance(sun,settings.density,settings.mie,
        settings.altitude+max(camera.y,0))*settings.illuminance;
    float phase=CloudHg(dot(ray,sun),0.65);
    float transmission=1;
    float3 radiance=0;
    [loop] for(uint i=0;i<48;++i) {
        float3 pos=camera+ray*(start+(i+0.5)*stepLength);
        radiance+=transmission*opacity*sunlight*phase*CloudShadow(pos,settings,noiseIndex)*TerrainRayVisibility(pos);
        transmission*=1-opacity;
    }
    if (settings.clouds==0 || camera.y<settings.cloudBottom)
        return float4(radiance+transmission*cloud.rgb,transmission*cloud.a);
    return float4(cloud.rgb+cloud.a*radiance,cloud.a*transmission);
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
    output[id.xy]=IntegrateCloudAndRays(ray,limit,float2(pixel)+0.5);
    outputDepth[id.xy]=limit;
}
// 遠景の雲を 1/4 解像度で描く。遠景の開始距離より手前は積分しない。
[numthreads(8,8,1)]
void CsCloudFar(uint3 id:SV_DispatchThreadID) {
    uint2 farSize=(fullSize+3)/4;
    if (any(id.xy>=farSize)) return;
    Texture2D<float> depth=ResourceDescriptorHeap[depthIndex];
    uint2 pixel=min(id.xy*4+1,fullSize-1);
    float z=depth.Load(int3(pixel,0));
    float limit;
    float3 ray=CloudViewRay(float2(pixel)+0.5,z,limit);
    RWTexture2D<float4> output=ResourceDescriptorHeap[farCloudOutput];
    output[id.xy]=limit>farDistance ? IntegrateCloud(camera,ray,limit,settings,noiseIndex,environmentIndex,farDistance) : float4(0,0,0,1);
}
float4 ReconstructCloud(float2 pixel, float3 ray, float limit) {
    if (settings.clouds==0 && godRays==0) return float4(0,0,0,1);
    if (halfCloudIndex==0xffffffff)
        return IntegrateCloudAndRays(ray,limit,pixel);
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
    if (!compatible) return IntegrateCloudAndRays(ray,limit,pixel);
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
