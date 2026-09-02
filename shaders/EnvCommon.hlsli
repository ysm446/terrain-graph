#ifndef TG_ENV_COMMON_HLSLI
#define TG_ENV_COMMON_HLSLI

#include "Common.hlsli"

// キューブマップの面の並びは D3D の規約に従う。
//   0: +X, 1: -X, 2: +Y, 3: -Y, 4: +Z, 5: -Z
static const uint kCubeFaceCount = 6;

// 面インデックスと面内 UV（[-1, 1]、v は下向き）から方向ベクトルを得る。
float3 CubeFaceDirection(uint face, float2 uv)
{
    float3 direction;
    switch (face)
    {
        case 0:  direction = float3( 1.0f, -uv.y, -uv.x); break;
        case 1:  direction = float3(-1.0f, -uv.y,  uv.x); break;
        case 2:  direction = float3( uv.x,  1.0f,  uv.y); break;
        case 3:  direction = float3( uv.x, -1.0f, -uv.y); break;
        case 4:  direction = float3( uv.x, -uv.y,  1.0f); break;
        default: direction = float3(-uv.x, -uv.y, -1.0f); break;
    }
    return normalize(direction);
}

// テクセル座標と面サイズから、面内 UV（[-1, 1]）を求める。
float2 CubeFaceUv(uint2 texel, uint faceSize)
{
    return (float2(texel) + 0.5f) / float(faceSize) * 2.0f - 1.0f;
}

// equirectangular マップとの相互変換。u は経度、v は天頂角。
float2 DirectionToEquirectUv(float3 direction)
{
    const float u = atan2(direction.z, direction.x) / (2.0f * kPi) + 0.5f;
    const float v = acos(clamp(direction.y, -1.0f, 1.0f)) / kPi;
    return float2(u, v);
}

float3 EquirectUvToDirection(float2 uv)
{
    const float phi = (uv.x - 0.5f) * 2.0f * kPi;
    const float theta = uv.y * kPi;
    const float sinTheta = sin(theta);
    return float3(sinTheta * cos(phi), cos(theta), sinTheta * sin(phi));
}

// 手続き的な空。単位は cd/m^2 相当で、intensity で全体の輝度を決める。
// 太陽は別途ディレクショナルライトで扱うため、ここには入れない
// （環境マップに入れると二重計上になり、解像度の都合でエイリアスも出る）。
float3 EvaluateProceduralSky(float3 direction, float3 zenithColor, float3 horizonColor,
                             float3 groundColor, float intensity)
{
    if (direction.y >= 0.0f)
    {
        const float t = pow(saturate(direction.y), 0.55f);
        return lerp(horizonColor, zenithColor, t) * intensity;
    }

    // 地面側は下を向くほど暗くする。
    const float t = saturate(-direction.y);
    return groundColor * lerp(1.0f, 0.45f, t) * intensity;
}

#endif  // TG_ENV_COMMON_HLSLI
