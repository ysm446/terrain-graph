#ifndef TG_TONEMAP_HLSLI
#define TG_TONEMAP_HLSLI

#include "Common.hlsli"

// 露出は物理カメラのパラメータから求める。
//   EV100    = log2(N^2 / t) - log2(ISO / 100)
//   exposure = 1 / (1.2 * 2^EV100)
// シェーダ側は算出済みの exposure を受け取り、線形放射輝度に掛けるだけにする。

static const uint kTonemapNone = 0;
static const uint kTonemapReinhard = 1;
static const uint kTonemapAces = 2;

float3 TonemapReinhard(float3 color)
{
    return color / (1.0f + color);
}

// Stephen Hill による ACES のフィット。
static const float3x3 kAcesInputMatrix = float3x3(
    0.59719f, 0.35458f, 0.04823f,
    0.07600f, 0.90834f, 0.01566f,
    0.02840f, 0.13383f, 0.83777f);

static const float3x3 kAcesOutputMatrix = float3x3(
     1.60475f, -0.53108f, -0.07367f,
    -0.10208f,  1.10813f, -0.00605f,
    -0.00327f, -0.07276f,  1.07602f);

float3 RrtAndOdtFit(float3 v)
{
    const float3 a = v * (v + 0.0245786f) - 0.000090537f;
    const float3 b = v * (0.983729f * v + 0.4329510f) + 0.238081f;
    return a / b;
}

float3 TonemapAces(float3 color)
{
    color = mul(kAcesInputMatrix, color);
    color = RrtAndOdtFit(color);
    color = mul(kAcesOutputMatrix, color);
    return saturate(color);
}

float3 ApplyTonemap(float3 color, uint mode)
{
    if (mode == kTonemapReinhard)
    {
        return TonemapReinhard(color);
    }
    if (mode == kTonemapAces)
    {
        return TonemapAces(color);
    }
    return saturate(color);
}

#endif  // TG_TONEMAP_HLSLI
