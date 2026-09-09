#ifndef TG_ATMOSPHERE_COMMON
#define TG_ATMOSPHERE_COMMON
#include "EnvCommon.hlsli"
#include "AtmosphereScattering.hlsli"
#include "CloudScattering.hlsli"
struct AtmosphericParameters {
    float azimuth; float elevation; float illuminance; float density;
    float mie; float eccentricity; float altitude; float groundAlbedo;
    uint clouds; float coverage; float extinction; float cloudBottom;
    float cloudThickness; float cloudScale; uint seed; uint samples;
    float fieldCenterX; float fieldCenterZ; float fieldRadius; float fieldFalloff;
    float windSpeed; float windDirection; uint animateClouds; float windOffsetX;
    float windOffsetZ; float3 padding;
};
float3 AtmosphereSun(AtmosphericParameters p) {
    return float3(cos(p.elevation) * sin(p.azimuth), sin(p.elevation), cos(p.elevation) * cos(p.azimuth));
}
// 64 枚の 2D 配列で周期 3D 密度を持つ。XY はハードウェア補間、Z のみ手動補間。
float CloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float h = (position.y - p.cloudBottom) / p.cloudThickness;
    if (h <= 0 || h >= 1 || p.clouds == 0 || p.coverage <= 0) return 0;
    Texture2DArray<float> noise = ResourceDescriptorHeap[noiseIndex];
    float2 wind = float2(p.windOffsetX,p.windOffsetZ);
    float3 uvw = float3((position.x-wind.x) / p.cloudScale, h, (position.z-wind.y) / p.cloudScale);
    float z = frac(uvw.z) * 64 - 0.5;
    float a = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+64)%64), 0);
    float b = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+65)%64), 0);
    float n = lerp(a,b,frac(z));
    float falloff = min(p.fieldFalloff, p.fieldRadius);
    float fieldFade = saturate((p.fieldRadius-length(position.xz-float2(p.fieldCenterX,p.fieldCenterZ)))/max(falloff,1));
    // 移植元の積雲プロファイル。高さの分布を適用してから雲量の閾値を引く。
    float profile = saturate(h*5)*saturate(1-h)*1.6;
    // 雲量の閾値で残った密度を 0〜1 へ戻す。本体と影の消散係数は共通。
    return saturate((saturate(n*profile)-(1-p.coverage))/max(p.coverage,1e-4))*fieldFade;
}
// 視線・自己遮蔽・地形の雲影は同じ有限円柱との交差を使う。
bool CloudInterval(float3 origin, float3 ray, float limit, AtmosphericParameters p,
                   out float start, out float end) {
    start=0; end=limit;
    if (abs(ray.y)<1e-6) {
        if (origin.y<=p.cloudBottom || origin.y>=p.cloudBottom+p.cloudThickness) return false;
    } else {
        float a=(p.cloudBottom-origin.y)/ray.y, b=(p.cloudBottom+p.cloudThickness-origin.y)/ray.y;
        start=max(start,min(a,b)); end=min(end,max(a,b));
    }
    float2 offset=origin.xz-float2(p.fieldCenterX,p.fieldCenterZ);
    float a=dot(ray.xz,ray.xz), b=dot(offset,ray.xz);
    float c=dot(offset,offset)-p.fieldRadius*p.fieldRadius;
    if(a<1e-12) {
        if(c>0) return false;
    } else {
        float discriminant=b*b-a*c;
        if(discriminant<0) return false;
        float root=sqrt(discriminant);
        start=max(start,(-b-root)/a); end=min(end,(-b+root)/a);
    }
    return end>start;
}
float CloudOpticalDepth(float3 origin, float3 ray, AtmosphericParameters p, uint noiseIndex, uint baseSamples) {
    float start,end;
    if(!CloudInterval(origin,ray,1e9,p,start,end)) return 0;
    // 長い斜光路でも雲層の出口まで積分し、自己遮蔽と地形影の減衰を揃える。
    uint count=(uint)clamp(ceil((end-start)/(p.cloudThickness/baseSamples)),baseSamples,baseSamples*4);
    float stepLength=(end-start)/count, optical=0;
    [loop] for(uint i=0;i<count;++i)
        optical+=CloudDensity(origin+ray*(start+(i+0.5)*stepLength),p,noiseIndex)*stepLength;
    return optical*p.extinction;
}
float CloudShadow(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 sun=AtmosphereSun(p);
    if(p.clouds==0 || sun.y<=0.001) return 1;
    return exp(-CloudOpticalDepth(position,sun,p,noiseIndex,max(p.samples/2,16u)));
}
float4 IntegrateCloud(float3 origin, float3 ray, float limit, AtmosphericParameters p,
                      uint noiseIndex, uint lightingIndex) {
    if(p.clouds==0) return float4(0,0,0,1);
    float start,end;
    if(!CloudInterval(origin,ray,limit,p,start,end)) return float4(0,0,0,1);
    uint count=(uint)clamp(ceil((end-start)/(p.cloudThickness/p.samples)),p.samples,p.samples*4);
    float stepLength=(end-start)/count;
    float3 sun=AtmosphereSun(p);
    float3 sunlight=AtmComputeSunTransmittance(sun,p.density,p.mie,p.altitude+p.cloudBottom)*p.illuminance;
    float mu=dot(ray,sun);
    Texture2D<float4> cloudLighting=ResourceDescriptorHeap[lightingIndex];
    float3 skyAbove=cloudLighting.Load(int3(0,0,0)).rgb;
    float3 skyBelow=cloudLighting.Load(int3(1,0,0)).rgb;
    float4 phases=float4(CloudPhase(mu,1),CloudPhase(mu,0.5),CloudPhase(mu,0.25),CloudPhase(mu,0.125));
    float transmission=1; float3 radiance=0;
    [loop] for(uint i=0;i<count && transmission>0.005;++i) {
        float3 pos=origin+ray*(start+(i+0.5)*stepLength);
        float density=CloudDensity(pos,p,noiseIndex);
        if(density<=0) continue;
        float sunDepth=CloudOpticalDepth(pos,sun,p,noiseIndex,max(p.samples/2,16u));
        float topDepth=CloudOpticalDepth(pos,float3(0,1,0),p,noiseIndex,8);
        float bottomDepth=CloudOpticalDepth(pos,float3(0,-1,0),p,noiseIndex,8);
        // 多重散乱のオクターブ近似。高次ほど寄与・消散・方向性を弱める。
        // 地形の直射影にはこの散乱光を使わず、元の Beer 透過率だけを使う。
        float3 light=0;
        [unroll] for(uint order=0;order<4;++order) {
            float attenuation=exp2(-(float)order);
            float weight=attenuation;
            light+=weight*(sunlight*phases[order]*exp(-sunDepth*attenuation)
                +0.5*(skyAbove*exp(-topDepth*attenuation)+skyBelow*exp(-bottomDepth*attenuation)));
        }
        // 区間内一定の密度・光源に対する解析積分（Beer-Lambert）。
        float opacity=1-exp(-density*p.extinction*stepLength);
        radiance+=transmission*opacity*light;
        transmission*=1-opacity;
    }
    return float4(radiance,transmission);
}
float3 AtmosphericSky(float3 ray, AtmosphericParameters p, uint lutIndex) {
    Texture2D<float4> lut=ResourceDescriptorHeap[lutIndex];
    float3 sun=AtmosphereSun(p);
    // 下半球は地面反射の近似。地表へすぐ衝突する極短レイの積分を避け、地平線を連続にする。
    float3 sampleRay=ray.y < 0 ? normalize(float3(ray.x,max(-ray.y,0.005),ray.z)) : ray;
    float3 sky=AtmComputeScattering(sampleRay,sun,p.density,p.mie,p.eccentricity,lut,g_samplerLinearClamp,true,p.altitude);
    if(ray.y<0) sky*=lerp(1,p.groundAlbedo,smoothstep(0,0.08,-ray.y));
    return max(0,sky*p.illuminance);
}
#endif
