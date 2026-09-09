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
    float windOffsetZ; uint lowerHemisphere; float noiseSpeedRatio; uint distributionMask;
    uint localCloud; float radiusX; float radiusZ; float edgeSoftness;
    float shapeStrength; float detailStrength; uint cloudMotionMode; uint cloudSource;
};
float3 AtmosphereSun(AtmosphericParameters p) {
    return float3(cos(p.elevation) * sin(p.azimuth), sin(p.elevation), cos(p.elevation) * cos(p.azimuth));
}
// ローカル雲は全軸で同じワールド周期を使う。Y も厚さから独立した 3D ノイズ。
float SampleCloudNoise(float3 uvw, uint noiseIndex) {
    Texture2DArray<float> noise = ResourceDescriptorHeap[noiseIndex];
    float z = frac(uvw.z) * 64 - 0.5;
    float a = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+64)%64), 0);
    float b = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+65)%64), 0);
    return lerp(a,b,frac(z));
}
float3 LocalCloudCenter(AtmosphericParameters p) {
    return float3(p.fieldCenterX, p.cloudBottom + p.cloudThickness * 0.5, p.fieldCenterZ);
}
float3 LocalCloudRadii(AtmosphericParameters p) {
    return float3(p.radiusX, p.cloudThickness * 0.5, p.radiusZ);
}
float LocalCloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 offset = position - LocalCloudCenter(p);
    float edge = 1 - length(offset / LocalCloudRadii(p));
    if (edge <= 0) return 0;
    float3 wind = p.cloudMotionMode != 0 ? float3(p.windOffsetX,0,p.windOffsetZ) : 0;
    float3 uvw = (offset-wind) / p.cloudScale + 0.5;
    float shape = saturate((SampleCloudNoise(uvw,noiseIndex)-0.5)*3+0.5);
    float detail = saturate((SampleCloudNoise(uvw*3.1+0.173,noiseIndex)-0.5)*3+0.5);
    // 外接楕円体の内側だけを削るため、輪郭は交差区間からはみ出さない。
    float density = saturate((edge - p.shapeStrength*(1-shape)*0.65) / p.edgeSoftness);
    return saturate(density - p.detailStrength*(1-detail)*(1-density));
}
// 64 枚の 2D 配列で周期 3D 密度を持つ。XY はハードウェア補間、Z のみ手動補間。
float CloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    if (p.clouds == 0) return 0;
    if (p.localCloud == 1) return LocalCloudDensity(position,p,noiseIndex);
    if (p.localCloud == 2) {
        float3 offset = position - LocalCloudCenter(p);
        float3 q = abs(offset / LocalCloudRadii(p));
        if (any(q >= 1)) return 0;
        if (p.distributionMask == 0xfffffffe) return 0; // 評価待ち。
        float distribution = 1;
        if (p.distributionMask != 0xffffffff) {
            Texture2D<float3> mask = ResourceDescriptorHeap[p.distributionMask];
            float2 uv = offset.xz / (2 * float2(p.radiusX,p.radiusZ)) + 0.5;
            distribution = saturate(mask.SampleLevel(g_samplerLinearClamp,uv,0).r);
        }
        if (distribution <= 0.001 || p.coverage <= 0) return 0;
        float3 uvw = (offset-float3(p.windOffsetX,0,p.windOffsetZ))/p.cloudScale+0.5;
        float shape = SampleCloudNoise(uvw,noiseIndex);
        float detail = SampleCloudNoise(uvw*3.1+0.173,noiseIndex);
        float profile = saturate((1-q.y)/max(p.edgeSoftness,0.01));
        float edge = saturate((1-max(q.x,q.z))/max(p.edgeSoftness,0.01));
        float density = saturate((shape-(1-p.coverage))/max(p.coverage,0.001));
        density = saturate(density-p.detailStrength*(1-detail)*(1-density));
        return density * profile * edge * distribution;
    }
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
    if (p.localCloud == 2) {
        float3 offset = origin-LocalCloudCenter(p);
        float3 radii = LocalCloudRadii(p);
        [unroll] for (uint axis=0; axis<3; ++axis) {
            if (abs(ray[axis]) < 1e-7) {
                if (abs(offset[axis]) >= radii[axis]) return false;
            } else {
                float a=(-radii[axis]-offset[axis])/ray[axis];
                float b=(radii[axis]-offset[axis])/ray[axis];
                start=max(start,min(a,b)); end=min(end,max(a,b));
            }
        }
        return end>start;
    }
    if (p.localCloud == 1) {
        float3 offset = (origin-LocalCloudCenter(p))/LocalCloudRadii(p);
        float3 direction = ray/LocalCloudRadii(p);
        float a=dot(direction,direction), b=dot(offset,direction), c=dot(offset,offset)-1;
        float discriminant=b*b-a*c;
        if (discriminant<0 || a<=0) return false;
        float root=sqrt(discriminant);
        start=max(0,(-b-root)/a); end=min(limit,(-b+root)/a);
        return end>start;
    }
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
// 雲層専用。shapeStrength と同じ領域をキャッシュ SRV として使用する。
bool HasCloudOpticalCache(AtmosphericParameters p) {
    return p.localCloud==2 && (asuint(p.shapeStrength)&0x80000000)!=0 && asuint(p.shapeStrength)!=0xffffffff;
}
float3 SampleCloudOpticalCache(float3 position, AtmosphericParameters p) {
    float3 uvw=saturate((position-LocalCloudCenter(p))/LocalCloudRadii(p)*0.5+0.5);
    float2 uv=(uvw.xy*float2(63,31)+0.5)/float2(64,32);
    float z=uvw.z*63;
    Texture2DArray<float4> cache=ResourceDescriptorHeap[asuint(p.shapeStrength)&0x7fffffff];
    return lerp(cache.SampleLevel(g_samplerLinearClamp,float3(uv,floor(z)),0).rgb,
                cache.SampleLevel(g_samplerLinearClamp,float3(uv,min(floor(z)+1,63)),0).rgb,frac(z));
}
float CloudShadow(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 sun=AtmosphereSun(p);
    if(p.clouds==0 || sun.y<=0.001) return 1;
    if (HasCloudOpticalCache(p)) {
        float start,end;
        if (!CloudInterval(position,sun,1e9,p,start,end)) return 1;
        return exp(-SampleCloudOpticalCache(position+sun*start,p).x);
    }
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
        float3 depths;
        if (HasCloudOpticalCache(p)) depths=SampleCloudOpticalCache(pos,p);
        else depths=float3(CloudOpticalDepth(pos,sun,p,noiseIndex,max(p.samples/2,16u)),
            CloudOpticalDepth(pos,float3(0,1,0),p,noiseIndex,8),
            CloudOpticalDepth(pos,float3(0,-1,0),p,noiseIndex,8));
        float sunDepth=depths.x, topDepth=depths.y, bottomDepth=depths.z;
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
float3 AtmosphericSky(float3 ray, AtmosphericParameters p, uint lutIndex, float3 groundRadiance=0) {
    Texture2D<float4> lut=ResourceDescriptorHeap[lutIndex];
    // 空の延長は初期実装と同じ上半球の折り返し。背景・IBL・雲照明で共用する。
    float3 sampleRay=ray;
    if(p.lowerHemisphere==0 && ray.y<0)
        sampleRay=normalize(float3(ray.x,max(-ray.y,0.005),ray.z));
    float3 sky=AtmComputeScattering(sampleRay,AtmosphereSun(p),p.density,p.mie,p.eccentricity,
        lut,g_samplerLinearClamp,true,p.altitude,groundRadiance);
    if(p.lowerHemisphere==0 && ray.y<0)
        sky*=lerp(1,p.groundAlbedo,smoothstep(0,0.08,-ray.y));
    return max(0,sky*p.illuminance);
}
#endif
