#ifndef TG_ATMOSPHERE_COMMON
#define TG_ATMOSPHERE_COMMON
#include "EnvCommon.hlsli"
#include "AtmosphereScattering.hlsli"
struct AtmosphericParameters {
    float azimuth; float elevation; float illuminance; float density;
    float mie; float eccentricity; float altitude; float groundAlbedo;
    uint clouds; float coverage; float extinction; float cloudBottom;
    float cloudThickness; float cloudScale; uint seed; uint samples;
};
float3 AtmosphereSun(AtmosphericParameters p) {
    return float3(cos(p.elevation) * sin(p.azimuth), sin(p.elevation), cos(p.elevation) * cos(p.azimuth));
}
// 64 枚の 2D 配列で周期 3D 密度を持つ。XY はハードウェア補間、Z のみ手動補間。
float CloudDensity(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float h = (position.y - p.cloudBottom) / p.cloudThickness;
    if (h <= 0 || h >= 1 || p.clouds == 0) return 0;
    Texture2DArray<float> noise = ResourceDescriptorHeap[noiseIndex];
    float3 uvw = float3(position.x / p.cloudScale, h, position.z / p.cloudScale);
    float z = frac(uvw.z) * 64 - 0.5;
    float a = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+64)%64), 0);
    float b = noise.SampleLevel(g_samplerLinearWrap, float3(uvw.xy, ((int)floor(z)+65)%64), 0);
    float n = lerp(a,b,frac(z));
    return max(0, n - (1 - p.coverage)) * smoothstep(0,0.15,h) * (1-smoothstep(0.7,1,h));
}
float CloudShadow(float3 position, AtmosphericParameters p, uint noiseIndex) {
    float3 sun = AtmosphereSun(p);
    if (p.clouds == 0 || sun.y <= 0.001) return 1;
    float start = max(0, (p.cloudBottom-position.y)/sun.y);
    float end = max(0, (p.cloudBottom+p.cloudThickness-position.y)/sun.y);
    float stepLength = (end-start)/12;
    float optical = 0;
    [loop] for (uint i=0;i<12;++i)
        optical += CloudDensity(position+sun*(start+(i+0.5)*stepLength),p,noiseIndex)*stepLength;
    return exp(-optical*p.extinction);
}
// terrain-editor と同じ密度レイマーチと太陽方向の自己遮蔽。HDR 放射輝度で積分する。
float4 IntegrateCloud(float3 origin, float3 ray, float limit, AtmosphericParameters p,
                      uint noiseIndex, float3 ambient) {
    if (p.clouds == 0) return float4(0,0,0,1);
    float start=0, end=limit;
    if (abs(ray.y)<1e-5) {
        if (origin.y<=p.cloudBottom || origin.y>=p.cloudBottom+p.cloudThickness) return float4(0,0,0,1);
    } else {
        float a=(p.cloudBottom-origin.y)/ray.y, b=(p.cloudBottom+p.cloudThickness-origin.y)/ray.y;
        start=max(0,min(a,b)); end=min(end,max(a,b));
    }
    // 遠方は滑らかに消し、水平線の固定距離での切断を避ける。
    end=min(end,100000); if(end<=start) return float4(0,0,0,1);
    float stepLength=(end-start)/p.samples;
    float3 sun=AtmosphereSun(p);
    float3 sunlight=AtmComputeSunTransmittance(sun,p.density,p.mie,p.altitude+p.cloudBottom)*p.illuminance;
    float mu=dot(ray,sun), g=0.65;
    float phase=(1-g*g)/(12.5663706*pow(max(1+g*g-2*g*mu,0.01),1.5));
    float transmission=1; float3 radiance=0;
    [loop] for(uint i=0;i<p.samples && transmission>0.005;++i) {
        float distance=start+(i+0.5)*stepLength;
        float3 pos=origin+ray*distance;
        float density=CloudDensity(pos,p,noiseIndex)*(1-smoothstep(70000,100000,distance));
        if(density<=0) continue;
        float lightOptical=0;
        [loop] for(uint j=0;j<6;++j)
            lightOptical+=CloudDensity(pos+sun*((j+0.5)*p.cloudThickness/6),p,noiseIndex)*p.cloudThickness/6;
        float visibility=exp(-lightOptical*p.extinction);
        float3 light=ambient*lerp(0.3,1,visibility)+sunlight*phase*visibility;
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
