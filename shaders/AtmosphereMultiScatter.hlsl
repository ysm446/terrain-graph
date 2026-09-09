// Multi-scattering LUT (Hillaire 2020).
//
// 等方的な高次散乱を高度・太陽天頂角ごとに事前積分する。
//
// Algorithm: at each LUT texel (mu_s, h), pick a position at altitude h
// in the atmosphere and integrate single-scatter from N=64 directions
// uniformly over the sphere. Two accumulators:
//   * L_2nd  — incoming radiance from those N directions
//   * F_avg  — fraction of light that scatters again (feedback)
// Geometric series: L_inf = L_2nd / (1 - F_avg) approximates the sum of
// all higher-order bounces. We assume isotropic phase for the multi-scatter
// term, which is the standard Hillaire approximation.
//
// LUT layout: 32×32 R16G16B16A16_FLOAT, U=cos(sun zenith) mapped to [0,1],
// V=sqrt(altitude/atmosphere_height) mapped to [0,1].

#include "AtmosphereScattering.hlsli"

cbuffer MultiScatterConstants : register(b0)
{
    float atmosphereDensity;
    float mieStrength;
    float groundAlbedo;
    uint outputIndex;
};



static const uint  kLutWidth        = 32u;
static const uint  kLutHeight       = 32u;
static const uint  kSphereSamples   = 64u;
static const uint  kRayMarchSteps   = 32u;
static const float kPi              = 3.14159265358979323846f;
static const float kAtmThickness    = kAtmAtmosphereRadius - kAtmEarthRadius;

// Uniform sample over the sphere using a spiral (sunflower-like) pattern.
// Cheap, deterministic, and gives a good distribution for low N.
float3 SphereSample(uint i)
{
    float a = (float)i + 0.5;
    float n = (float)kSphereSamples;
    float cosTheta = 1.0 - 2.0 * a / n;
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    float phi = a * 2.39996322972865332f;     // golden-angle in radians
    return float3(sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
}

// 等方散乱の光源項と、再散乱する割合を同じ解析積分で求める。
void SingleScatterAlongRay(float3 origin,float3 viewDir,float3 sunDir,
                          float density,float mieS,out float3 Lscat,out float3 Fscat) {
    Lscat=0; Fscat=0;
    float2 hit=AtmRaySphere(origin,viewDir,kAtmAtmosphereRadius);
    if(hit.y<=0) return;
    float2 earth=AtmRaySphere(origin,viewDir,kAtmEarthRadius);
    bool hitGround=earth.x>0;
    float distance=hitGround ? min(hit.y,earth.x) : hit.y;
    float3 transmission=1;
    [loop] for(uint i=0;i<kRayMarchSteps;++i) {
        float u0=(float)i/kRayMarchSteps,u1=(float)(i+1)/kRayMarchSteps;
        float start=distance*(hitGround ? 1-AtmosphereRayFraction(1-u0) : AtmosphereRayFraction(u0));
        float end=distance*(hitGround ? 1-AtmosphereRayFraction(1-u1) : AtmosphereRayFraction(u1));
        float3 position=origin+viewDir*(0.5*(start+end));
        float height=max(length(position)-kAtmEarthRadius,0);
        float3 rayleigh=kAtmBetaR*density*exp(-height/kAtmHeightR);
        float mie=kAtmBetaM*mieS*exp(-height/kAtmHeightM);
        float3 tau=(rayleigh+1.1*mie)*(end-start);
        float3 scattered=transmission*(rayleigh+mie)*(end-start)*AtmSegmentWeight(tau);
        Fscat+=scattered;
        Lscat+=scattered*AtmSunTransmittanceAtPosition(position,sunDir,density,mieS)/(4*kPi);
        transmission*=exp(-tau);
    }
    if(hitGround) {
        float3 normal=normalize(origin+viewDir*distance);
        float3 atGround=normal*(kAtmEarthRadius+1);
        Lscat+=transmission*groundAlbedo/kPi*saturate(dot(normal,sunDir))
            *AtmSunTransmittanceAtPosition(atGround,sunDir,density,mieS);
    }
}

[numthreads(8, 8, 1)]
void CSGenerate(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= kLutWidth || dtid.y >= kLutHeight) return;

    float u = ((float)dtid.x + 0.5) / (float)kLutWidth;
    float v = ((float)dtid.y + 0.5) / (float)kLutHeight;

    // U axis: cos(sun zenith) ∈ [-1, 1].
    float cosSunZenith = u * 2.0 - 1.0;
    // V axis: altitude in atmosphere.
    float altitude = v * v * kAtmThickness;

    float3 origin = float3(0.0, kAtmEarthRadius + altitude, 0.0);
    float sinSunZenith = sqrt(saturate(1.0 - cosSunZenith * cosSunZenith));
    float3 sunDir = float3(sinSunZenith, cosSunZenith, 0.0);

    float3 sumL = float3(0.0, 0.0, 0.0);
    float3 sumF = float3(0.0, 0.0, 0.0);

    [loop]
    for (uint i = 0; i < kSphereSamples; ++i)
    {
        float3 viewDir = SphereSample(i);
        float3 Lscat, Fscat;
        SingleScatterAlongRay(origin, viewDir, sunDir, atmosphereDensity, mieStrength, Lscat, Fscat);
        sumL += Lscat;
        sumF += Fscat;
    }

    // Average over the sphere. The 4π solid angle factor cancels with the
    // 1/4π isotropic phase used inside SingleScatterAlongRay.
    float3 Lavg = sumL / (float)kSphereSamples;
    float3 Favg = sumF / (float)kSphereSamples;

    // Geometric series for infinite-order isotropic feedback: each scatter
    // contributes Lavg, with feedback fraction Favg. Saturate Favg to keep
    // the denominator stable when atmospheric params get extreme.
    float3 multiScatter = Lavg / max(1.0 - Favg, float3(1e-3, 1e-3, 1e-3));

    RWTexture2D<float4> Output = ResourceDescriptorHeap[outputIndex];
    Output[dtid.xy] = float4(multiScatter, 1.0);
}
