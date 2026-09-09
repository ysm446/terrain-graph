// terrain-editor の Nishita / Hillaire 散乱モデルから移植。
#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI
#include "AtmosphereIntegration.hlsli"

static const float kAtmEarthRadius      = 6360e3;
static const float kAtmAtmosphereRadius = 6420e3;
static const float kAtmHeightR          = 7994.0;   // Rayleigh scale height (m)
static const float kAtmHeightM          = 1200.0;   // Mie scale height (m)
static const float3 kAtmBetaR           = float3(5.802e-6, 13.558e-6, 33.1e-6);  // Rayleigh β_λ
static const float kAtmBetaM            = 21e-6;
static const float kAtmSunIntensity     = 1.0;
static const int   kAtmNumViewSteps = 32;
static const int   kAtmNumSunSteps  = 32;

// Two intersection ts of a ray (origin, dir) with a sphere centred at the
// world origin of the supplied radius. .x = near, .y = far. Returns -1, -1
// if no intersection. dir must be unit length.
float2 AtmRaySphere(float3 origin, float3 dir, float radius)
{
    float b = dot(origin, dir);
    float height = length(origin);
    float c = (height-radius)*(height+radius);
    float d = b * b - c;
    if (d < 0.0)
    {
        return float2(-1.0, -1.0);
    }
    float sq = sqrt(d);
    return float2(-b - sq, -b + sq);
}

// LUT は U=(cos(sunZenith)+1)/2、V=sqrt(altitude/atmosphereHeight)。
float3 AtmSampleMultiScatter(Texture2D<float4> lut, SamplerState samp, float altitude, float cosSunZenith)
{
    float u = saturate(cosSunZenith * 0.5 + 0.5);
    float v = sqrt(saturate(altitude / (kAtmAtmosphereRadius - kAtmEarthRadius)));
    return lut.SampleLevel(samp, float2(u, v), 0).rgb;
}

// 太陽光の積分は地表近くへ点を集中させる。CPU の直接光も同じ配置を使う。
float3 AtmSunTransmittanceAtPosition(float3 origin,float3 sunDir,float density,float mieStrength) {
    float2 hit=AtmRaySphere(origin,sunDir,kAtmAtmosphereRadius);
    if(hit.y<=0 || AtmRaySphere(origin,sunDir,kAtmEarthRadius).x>0) return 0;
    float opticalR=0,opticalM=0;
    [loop] for(uint i=0;i<kAtmNumSunSteps;++i) {
        float start=hit.y*AtmosphereRayFraction((float)i/kAtmNumSunSteps);
        float end=hit.y*AtmosphereRayFraction((float)(i+1)/kAtmNumSunSteps);
        float h=length(origin+sunDir*(0.5*(start+end)))-kAtmEarthRadius;
        if(h<0) return 0;
        opticalR+=exp(-h/kAtmHeightR)*(end-start);
        opticalM+=exp(-h/kAtmHeightM)*(end-start);
    }
    return exp(-kAtmBetaR*density*opticalR-kAtmBetaM*mieStrength*1.1*opticalM);
}
float3 AtmComputeSunTransmittance(float3 sunDir,float density,float mieStrength,float observerHeight) {
    return AtmSunTransmittanceAtPosition(float3(0,kAtmEarthRadius+max(observerHeight,1),0),sunDir,density,mieStrength);
}
float3 AtmSegmentWeight(float3 tau) {
    return float3(AtmosphereSegmentWeight(tau.x),AtmosphereSegmentWeight(tau.y),AtmosphereSegmentWeight(tau.z));
}
// 単位大気圏外照度に対する輝度。地表に当たるレイには地面反射の境界条件を加える。
float3 AtmComputeScattering(float3 viewDir,float3 sunDir,float density,float mieStrength,float mieG,
    Texture2D<float4> multiScatterLut,SamplerState multiScatterSampler,bool useMultiScatter,
    float observerHeight,float3 groundRadiance=0) {
    float3 origin=float3(0,kAtmEarthRadius+max(observerHeight,1),0);
    float2 atmHit=AtmRaySphere(origin,viewDir,kAtmAtmosphereRadius);
    if(atmHit.y<=0) return 0;
    float2 earthHit=AtmRaySphere(origin,viewDir,kAtmEarthRadius);
    bool hitGround=earthHit.x>0;
    float marchEnd=hitGround ? min(atmHit.y,earthHit.x) : atmHit.y;
    float3 betaR=kAtmBetaR*density;
    float betaM=kAtmBetaM*mieStrength;
    float mu=dot(viewDir,sunDir);
    float phaseR=0.0596831037*(1+mu*mu);
    float phaseM=0.0795774715*(1-mieG*mieG)/pow(max(1+mieG*mieG-2*mieG*mu,1e-6),1.5);
    float3 transmission=1,radiance=0;
    [loop] for(uint i=0;i<kAtmNumViewSteps;++i) {
        float u0=(float)i/kAtmNumViewSteps,u1=(float)(i+1)/kAtmNumViewSteps;
        float start=marchEnd*(hitGround ? 1-AtmosphereRayFraction(1-u0) : AtmosphereRayFraction(u0));
        float end=marchEnd*(hitGround ? 1-AtmosphereRayFraction(1-u1) : AtmosphereRayFraction(u1));
        float3 position=origin+viewDir*(0.5*(start+end));
        float height=max(length(position)-kAtmEarthRadius,0);
        float3 rayleigh=betaR*exp(-height/kAtmHeightR);
        float mie=betaM*exp(-height/kAtmHeightM);
        float3 tau=(rayleigh+1.1*mie)*(end-start);
        float3 source=AtmSunTransmittanceAtPosition(position,sunDir,density,mieStrength)*(rayleigh*phaseR+mie*phaseM);
        if(useMultiScatter) source+=AtmSampleMultiScatter(multiScatterLut,multiScatterSampler,height,dot(normalize(position),sunDir))*(rayleigh+mie);
        radiance+=transmission*source*(end-start)*AtmSegmentWeight(tau);
        transmission*=exp(-tau);
    }
    if(hitGround) radiance+=transmission*groundRadiance;
    return radiance;
}
#endif
