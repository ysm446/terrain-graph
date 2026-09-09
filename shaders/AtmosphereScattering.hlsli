// terrain-editor の Nishita / Hillaire 散乱モデルから移植。
#ifndef ATMOSPHERE_HLSLI
#define ATMOSPHERE_HLSLI

static const float kAtmEarthRadius      = 6360e3;
static const float kAtmAtmosphereRadius = 6420e3;
static const float kAtmHeightR          = 7994.0;   // Rayleigh scale height (m)
static const float kAtmHeightM          = 1200.0;   // Mie scale height (m)
static const float3 kAtmBetaR           = float3(5.802e-6, 13.558e-6, 33.1e-6);  // Rayleigh β_λ
static const float kAtmBetaM            = 21e-6;
static const float kAtmSunIntensity     = 1.0;
static const int   kAtmNumViewSteps = 32;
static const int   kAtmNumSunSteps  = 8;

// Two intersection ts of a ray (origin, dir) with a sphere centred at the
// world origin of the supplied radius. .x = near, .y = far. Returns -1, -1
// if no intersection. dir must be unit length.
float2 AtmRaySphere(float3 origin, float3 dir, float radius)
{
    float b = dot(origin, dir);
    float c = dot(origin, origin) - radius * radius;
    float d = b * b - c;
    if (d < 0.0)
    {
        return float2(-1.0, -1.0);
    }
    float sq = sqrt(d);
    return float2(-b - sq, -b + sq);
}

// Sample the multi-scatter LUT at (altitude, cos(sun zenith)). The LUT is
// 32×32 float4 with U=cos(sunZenith) in [0,1] and V=altitude/atmosphereHeight
// in [0,1]. Returns 0 if the caller passes a null sampler — used by the
// multi-scatter LUT generator itself which obviously can't sample its own
// output.
float3 AtmSampleMultiScatter(Texture2D<float4> lut, SamplerState samp, float altitude, float cosSunZenith)
{
    float u = saturate(cosSunZenith * 0.5 + 0.5);
    float v = sqrt(saturate(altitude / (kAtmAtmosphereRadius - kAtmEarthRadius)));
    return lut.SampleLevel(samp, float2(u, v), 0).rgb;
}

// Atmospheric in-scattering integrated along the ray (origin, viewDir).
// `density` multiplies the Rayleigh β coefficients — overall atmospheric
//           thickness (1.0 = Earth-like).
// `mieStrength` multiplies the Mie scattering coefficient (haze).
// `mieG` is the Henyey-Greenstein eccentricity (sun glow tightness).
// 出力は単位照度に対する RGB 放射輝度。呼び出し側で大気圏外照度を掛ける。
//
// The multi-scatter LUT, when valid, adds a Hillaire-style isotropic
// second-order term at each ray-march step so the noon horizon comes out
// closer to a UE5-like blue/white instead of single-scatter's warm cast.
float3 AtmComputeScattering(float3 viewDir, float3 sunDir, float density, float mieStrength, float mieG,
                            Texture2D<float4> multiScatterLut, SamplerState multiScatterSampler,
                            bool useMultiScatter, float observerHeight)
{
    float3 origin = float3(0.0, kAtmEarthRadius + max(observerHeight, 1.0), 0.0);

    float2 atmHit = AtmRaySphere(origin, viewDir, kAtmAtmosphereRadius);
    if (atmHit.y <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    float marchEnd = atmHit.y;

    // If the view ray hits the planet (e.g. looking straight down past the
    // horizon) end the march at the surface — keeps the lower hemisphere
    // dark instead of integrating noise from clipped samples.
    float2 earthHit = AtmRaySphere(origin, viewDir, kAtmEarthRadius);
    if (earthHit.x > 0.0)
    {
        marchEnd = min(marchEnd, earthHit.x);
    }

    float3 betaR = kAtmBetaR * density;
    float  betaM = kAtmBetaM * mieStrength;

    float stepLen = marchEnd / (float)kAtmNumViewSteps;
    float opticalR = 0.0;
    float opticalM = 0.0;
    float3 sumR = float3(0.0, 0.0, 0.0);
    float3 sumM = float3(0.0, 0.0, 0.0);
    float3 sumMS = float3(0.0, 0.0, 0.0);

    [loop]
    for (int i = 0; i < kAtmNumViewSteps; ++i)
    {
        float t = ((float)i + 0.5) * stepLen;
        float3 p = origin + viewDir * t;
        float h = length(p) - kAtmEarthRadius;
        if (h < 0.0) break;

        float dR = exp(-h / kAtmHeightR) * stepLen;
        float dM = exp(-h / kAtmHeightM) * stepLen;
        opticalR += dR;
        opticalM += dM;

        // Multi-scatter contribution accumulates at every step, even where
        // the sun is blocked (in shadow regions multiple scatter still
        // brings light from elsewhere in the sky — that's the whole point).
        if (useMultiScatter)
        {
            float cosSunZenith = dot(normalize(p), sunDir);
            float3 multi = AtmSampleMultiScatter(multiScatterLut, multiScatterSampler, h, cosSunZenith);
            float3 viewTau = betaR * (opticalR - 0.5 * dR) + betaM * 1.1 * (opticalM - 0.5 * dM);
            float3 viewTransmit = exp(-viewTau);
            sumMS += viewTransmit * multi * (betaR * dR + betaM * dM);
        }

        float2 sunHit = AtmRaySphere(p, sunDir, kAtmAtmosphereRadius);
        if (sunHit.y <= 0.0) continue;
        if (AtmRaySphere(p, sunDir, kAtmEarthRadius).x > 0.0) continue;
        float sunStep = sunHit.y / (float)kAtmNumSunSteps;
        float sunR = 0.0;
        float sunM = 0.0;
        bool sunBlocked = false;
        [loop]
        for (int j = 0; j < kAtmNumSunSteps; ++j)
        {
            float st = ((float)j + 0.5) * sunStep;
            float3 sp = p + sunDir * st;
            float sh = length(sp) - kAtmEarthRadius;
            if (sh < 0.0) { sunBlocked = true; break; }
            sunR += exp(-sh / kAtmHeightR) * sunStep;
            sunM += exp(-sh / kAtmHeightM) * sunStep;
        }
        if (sunBlocked) continue;

        float3 tau = betaR * (opticalR - 0.5 * dR + sunR) +
                     betaM * 1.1 * (opticalM - 0.5 * dM + sunM);
        float3 atten = exp(-tau);
        sumR += atten * dR;
        sumM += atten * dM;
    }

    float cosTheta = dot(viewDir, sunDir);
    float phaseR = 0.0596831 * (1.0 + cosTheta * cosTheta);  // 3 / (16π)
    float g = mieG;
    float gg = g * g;
    float phaseM = 0.0795775 * (1.0 - gg) /
                   pow(max(1.0 + gg - 2.0 * g * cosTheta, 1e-6), 1.5);  // 1 / (4π)

    return kAtmSunIntensity *
           (sumR * betaR * phaseR + sumM * betaM * phaseM) + sumMS;
}

// Atmospheric transmittance from sea-level along the sun direction —
// gives the sun colour as seen from the ground. Multiply by the desired
// sun base spectrum (typically white) to get a sun colour that warms up
// at sunset and dims at horizon. `density` scales the Rayleigh β and
// `mieStrength` scales the Mie β.
float3 AtmComputeSunTransmittance(float3 sunDir, float density, float mieStrength, float observerHeight)
{
    float3 origin = float3(0.0, kAtmEarthRadius + max(observerHeight, 1.0), 0.0);
    float2 hit = AtmRaySphere(origin, sunDir, kAtmAtmosphereRadius);
    if (hit.y <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    if (AtmRaySphere(origin, sunDir, kAtmEarthRadius).x > 0.0) return 0.0;
    float stepLen = hit.y / (float)kAtmNumSunSteps;
    float opticalR = 0.0;
    float opticalM = 0.0;
    [loop]
    for (int j = 0; j < kAtmNumSunSteps; ++j)
    {
        float st = ((float)j + 0.5) * stepLen;
        float3 sp = origin + sunDir * st;
        float sh = length(sp) - kAtmEarthRadius;
        if (sh < 0.0) return float3(0.0, 0.0, 0.0);
        opticalR += exp(-sh / kAtmHeightR) * stepLen;
        opticalM += exp(-sh / kAtmHeightM) * stepLen;
    }
    float3 tau = kAtmBetaR * density * opticalR + kAtmBetaM * mieStrength * 1.1 * opticalM;
    return exp(-tau);
}

#endif // ATMOSPHERE_HLSLI
