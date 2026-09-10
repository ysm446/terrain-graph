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
    float flatCloudBottom; float cloudSkylightIntensity; float cloudBodyOffsetX; float cloudBodyOffsetZ;
    float indirectLight; float ambientLight; float2 lightingPadding;
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
float CloudBottomFade(float h, float shape, float detail) {
    // 同じ高さを基準に、浅い起伏と薄い密度の縁を残す。既存ノイズを共有する。
    float bottom = 0.02 + 0.16*(1-shape) + 0.03*(1-detail);
    return smoothstep(bottom,bottom+0.08,h);
}
float LocalCloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 offset = position - LocalCloudCenter(p);
    float3 q = offset / LocalCloudRadii(p);
    float h = (position.y-p.cloudBottom)/p.cloudThickness;
    if (p.flatCloudBottom != 0) {
        if (h <= 0 || h >= 1) return 0;
        // 雲底を半楕円体の底面に置く。下部ノイズは高さを固定し、底の凹凸を抑える。
        q.y = lerp(q.y,h,p.flatCloudBottom);
        offset.y = (lerp(h,max(h,0.12),p.flatCloudBottom)-0.5)*p.cloudThickness;
    }
    float edge = 1 - length(q);
    if (edge <= 0) return 0;
    float3 wind = p.cloudMotionMode != 0 ? float3(p.windOffsetX,0,p.windOffsetZ) : 0;
    float3 uvw = (offset-wind) / p.cloudScale + 0.5;
    float shape = saturate((SampleCloudNoise(uvw,noiseIndex)-0.5)*3+0.5);
    float detail = saturate((SampleCloudNoise(uvw*3.1+0.173,noiseIndex)-0.5)*3+0.5);
    // 輪郭は交差区間の内側だけを削る。
    float density = saturate((edge - p.shapeStrength*(1-shape)*0.65) / p.edgeSoftness);
    density = saturate(density - p.detailStrength*(1-detail)*(1-density));
    return density * (p.flatCloudBottom != 0 ? CloudBottomFade(h/max(p.flatCloudBottom,1e-5),shape,detail) : 1);
}
// 雲層の塊配置。移流の巻き戻し周期（模様の大きさの 10 倍）に合わせる。
float3 CloudCellRandom(int2 cell, uint seed) {
    uint2 wrapped=uint2((cell%10+10)%10);
    uint value=wrapped.x*1597334677u+wrapped.y*3812015801u+seed*2798796415u;
    value=(value^(value>>16))*2246822519u;
    uint a=(value^(value>>13))*3266489917u;
    uint b=(a^(a>>16))*2246822519u;
    uint c=(b^(b>>13))*3266489917u;
    return float3(a&65535u,b&65535u,c&65535u)/65535.0;
}
float CloudLayerBody(float2 position, float h, AtmosphericParameters p, float distribution) {
    int2 cell=int2(floor(position));
    float body=-1;
    // 半径は 1 セル未満。隣接セルも評価し、セル境界で塊を切らない。
    [unroll] for(int z=-1;z<=1;++z) [unroll] for(int x=-1;x<=1;++x) {
        int2 neighbor=cell+int2(x,z);
        float3 random=CloudCellRandom(neighbor,p.seed);
        float2 center=float2(neighbor)+0.5+(random.xy-0.5)*0.9;
        // 大きさ・縦横比・向き・高さは独立した乱数で変える。
        float3 dimensions=CloudCellRandom(neighbor,p.seed+137u);
        float3 lobes=CloudCellRandom(neighbor,p.seed+719u);
        float radius=(0.65+0.3*sqrt(p.coverage))*lerp(0.38,0.95,sqrt(random.z));
        float angle=dimensions.x*6.2831853;
        float sine,cosine; sincos(angle,sine,cosine);
        float2 delta=position-center;
        float2 rotated=float2(cosine*delta.x+sine*delta.y,-sine*delta.x+cosine*delta.y);
        float2 axes=radius*float2(lerp(0.45,1.0,dimensions.y),1);
        float height=lerp(0.28,1.0,dimensions.z);
        float vertical=lerp(2*h-1,h,p.flatCloudBottom)/height;
        float2 horizontal=rotated/axes;
        float cluster=1-length(float3(horizontal,vertical));
        // 異なる高さの膨らみを重ね、ひとつの半楕円体の頂点を崩す。
        // 各ローブの水平支持範囲は主塊の半径内に収める。
        float2 lobeOffset=float2(0.35,lerp(-0.35,0.35,lobes.x));
        float lobeHeight=lerp(0.45,1.0,lobes.y);
        float lobeVertical=lerp(2*h-1,h,p.flatCloudBottom)/lobeHeight;
        float lobe=1-length(float3((horizontal-lobeOffset)/float2(0.55,0.6),lobeVertical));
        float blend=saturate(0.5+0.5*(lobe-cluster)/0.25);
        cluster=lerp(cluster,lobe,blend)+0.25*blend*(1-blend);
        body=max(body,cluster);
    }
    // マスクの灰色は塊の輪郭を縮める。内部の密度を一律には薄めない。
    return body-(1-sqrt(p.coverage*distribution));
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
        float h = (position.y-p.cloudBottom)/p.cloudThickness;
        float3 noiseOffset = offset;
        if (p.flatCloudBottom != 0)
            noiseOffset.y = (lerp(h,max(h,0.12),p.flatCloudBottom)-0.5)*p.cloudThickness;
        float3 uvw = (noiseOffset-float3(p.windOffsetX,0,p.windOffsetZ))/p.cloudScale+0.5;
        // 低周波の座標変形で大きな輪郭を曲げる。変形後の座標でセルを選ぶ。
        // 周波数 0.5 は既存の 10 周期の巻き戻しと整合する。
        float broadX=SampleCloudNoise(uvw*0.5,noiseIndex);
        float broadZ=SampleCloudNoise(uvw*0.5+float3(0.37,0.19,0.61),noiseIndex);
        float2 bodyPosition=(offset.xz-float2(p.cloudBodyOffsetX,p.cloudBodyOffsetZ))/p.cloudScale;
        bodyPosition+=(float2(broadX,broadZ)-0.5)*1.6;
        float body=CloudLayerBody(bodyPosition,h,p,distribution);
        if(body<=0) return 0;
        // 表面だけでなく中規模の入り江や切れ目も作り、丸い土台を残さない。
        float shape = saturate((SampleCloudNoise(uvw*2+0.317,noiseIndex)-0.5)*3+0.5);
        float detail = saturate((SampleCloudNoise(uvw*3.1+0.173,noiseIndex)-0.5)*3+0.5);
        float profile = saturate((1-q.y)/max(p.edgeSoftness,0.01));
        if (p.flatCloudBottom != 0)
            profile = CloudBottomFade(h/max(p.flatCloudBottom,1e-5),shape,detail)
                *lerp(profile,saturate(2*(1-h)/max(p.edgeSoftness,0.01)),p.flatCloudBottom);
        float edge = saturate((1-max(q.x,q.z))/max(p.edgeSoftness,0.01));
        float density = saturate((body-0.48*(1-shape))/max(p.edgeSoftness,0.01));
        density = saturate(density-p.detailStrength*(1-detail)*(1-density));
        return density * profile * edge;
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
    if (p.localCloud == 2 || (p.localCloud == 1 && p.flatCloudBottom != 0)) {
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
    // 地形と共通の倍率を天空照明にだけ適用。太陽光と光学的厚さは変えない。
    float3 skyAbove=cloudLighting.Load(int3(0,0,0)).rgb*p.cloudSkylightIntensity*p.ambientLight;
    float3 skyBelow=cloudLighting.Load(int3(1,0,0)).rgb*p.cloudSkylightIntensity*p.ambientLight;
    float4 phases=float4(CloudPhase(mu,1),CloudPhase(mu,0.5),CloudPhase(mu,0.25),CloudPhase(mu,0.125));
    // 寄与と消散を分離するオクターブ近似。0.85 は有限次数で失われる光の調整値。
    // 単散乱は維持し、高次の等方化した太陽光を残す（設計資料 atmospheric-sky.md）。
    const float contribution=0.85;
    phases*=float4(1,contribution,contribution*contribution,contribution*contribution*contribution);
    phases.yzw*=p.indirectLight; // 雲自体の明るさを高次散乱だけで調整する。
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
        // 多重散乱のオクターブ近似。太陽光の寄与は上で位相へ適用済み。
        // 地形の直射影にはこの散乱光を使わず、元の Beer 透過率だけを使う。
        float3 light=0;
        [unroll] for(uint order=0;order<4;++order) {
            float attenuation=exp2(-(float)order);
            float weight=attenuation;
            light+=sunlight*phases[order]*exp(-sunDepth*attenuation)
                +weight*0.5*(skyAbove*exp(-topDepth*attenuation)+skyBelow*exp(-bottomDepth*attenuation));
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
