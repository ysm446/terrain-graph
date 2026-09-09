// Multi-scattering LUT (Hillaire 2020).
//
// Single-scatter Nishita produces a noticeably warm horizon at noon
// because it doesn't model the blue light that reaches a viewer after
// being scattered multiple times. UE5's Sky Atmosphere fixes this by
// pre-computing an isotropic multi-scattering term per (altitude, sun
// zenith) and adding it to the per-pixel single-scatter integral.
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
// V=altitude/atmosphere_height mapped to [0,1].

#include "AtmosphereScattering.hlsli"

cbuffer MultiScatterConstants : register(b0)
{
    float atmosphereDensity;
    float mieStrength;
    float mieEccentricity;
    uint outputIndex;
};



static const uint  kLutWidth        = 32u;
static const uint  kLutHeight       = 32u;
static const uint  kSphereSamples   = 64u;
static const uint  kRayMarchSteps   = 20u;
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

// Single-scatter ray-march with two outputs (Hillaire 2020 multi-scatter
// LUT formulation):
//   Lscat = single-scatter radiance arriving at `origin` from direction
//           `viewDir`, from the sun in direction `sunDir`. Rayleigh uses
//           its proper phase function (1+cos²θ); Mie is treated as
//           isotropic (Hillaire's MS LUT runs with MieRayPhase=false).
//   Fscat = fraction of a unit-radiance isotropic source that would
//           in-scatter back along this ray. No phase factor; this is just
//           the integrated transmittance × scattering × density along
//           the ray.
void SingleScatterAlongRay(float3 origin, float3 viewDir, float3 sunDir,
                           float density, float mieS, out float3 Lscat, out float3 Fscat)
{
    Lscat = float3(0.0, 0.0, 0.0);
    Fscat = float3(0.0, 0.0, 0.0);

    float2 atmHit = AtmRaySphere(origin, viewDir, kAtmAtmosphereRadius);
    if (atmHit.y <= 0.0) return;
    float marchEnd = atmHit.y;

    float2 earthHit = AtmRaySphere(origin, viewDir, kAtmEarthRadius);
    if (earthHit.x > 0.0) marchEnd = min(marchEnd, earthHit.x);
    if (marchEnd <= 0.0) return;

    float3 betaR = kAtmBetaR * density;
    float  betaM = kAtmBetaM * mieS;

    float stepLen = marchEnd / (float)kRayMarchSteps;
    float opticalR = 0.0;
    float opticalM = 0.0;

    float cosTheta = dot(viewDir, sunDir);
    float phaseR = 0.0596831 * (1.0 + cosTheta * cosTheta);  // 3 / (16π)
    float phaseM = 0.0795775;                                // isotropic

    [loop]
    for (uint i = 0; i < kRayMarchSteps; ++i)
    {
        float t = ((float)i + 0.5) * stepLen;
        float3 p = origin + viewDir * t;
        float h = length(p) - kAtmEarthRadius;
        if (h < 0.0) break;

        float dR = exp(-h / kAtmHeightR) * stepLen;
        float dM = exp(-h / kAtmHeightM) * stepLen;
        opticalR += dR;
        opticalM += dM;

        float3 tauView = betaR * opticalR + betaM * 1.1 * opticalM;
        float3 transmittanceView = exp(-tauView);

        // Feedback factor accumulates regardless of sun visibility — this
        // is the "if every point were a unit isotropic emitter, how much
        // would reach the ray origin" term.
        Fscat += transmittanceView * (betaR * dR + betaM * dM);

        // Sun light reaching this sample (proper Rayleigh phase, isotropic Mie).
        float2 sunHit = AtmRaySphere(p, sunDir, kAtmAtmosphereRadius);
        if (sunHit.y <= 0.0) continue;
        if (AtmRaySphere(p, sunDir, kAtmEarthRadius).x > 0.0) continue;
        float sunStep = sunHit.y / (float)kAtmNumSunSteps;
        float sunR = 0.0;
        float sunM = 0.0;
        bool sunBlocked = false;
        [loop]
        for (uint j = 0; j < kAtmNumSunSteps; ++j)
        {
            float st = ((float)j + 0.5) * sunStep;
            float3 sp = p + sunDir * st;
            float sh = length(sp) - kAtmEarthRadius;
            if (sh < 0.0) { sunBlocked = true; break; }
            sunR += exp(-sh / kAtmHeightR) * sunStep;
            sunM += exp(-sh / kAtmHeightM) * sunStep;
        }
        if (sunBlocked) continue;

        float3 tauSun = betaR * sunR + betaM * 1.1 * sunM;
        float3 transmittanceSun = exp(-tauSun);
        float3 rayleighInScat = betaR * dR * phaseR;
        float3 mieInScat      = betaM * dM * phaseM;
        Lscat += transmittanceView * transmittanceSun *
                 (rayleighInScat + mieInScat) * kAtmSunIntensity;
    }
}

[numthreads(8, 8, 1)]
void CSGenerate(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= kLutWidth || dtid.y >= kLutHeight) return;

    float u = ((float)dtid.x + 0.5) / (float)kLutWidth;
    float v = ((float)dtid.y + 0.5) / (float)kLutHeight;

    // U axis: cos(sun zenith) ∈ [-1, 1]. Use a slight bias toward the
    // horizon-visible range where multi-scatter contribution changes
    // fastest.
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
