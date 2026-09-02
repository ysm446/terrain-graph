#ifndef TG_BRDF_HLSLI
#define TG_BRDF_HLSLI

#include "Common.hlsli"

// Cook-Torrance の各項。roughness は知覚的な値（perceptual roughness）で受け取り、
// 内部で alpha = roughness^2 に変換する。

// 知覚ラフネスの下限。**全経路（直接光 / サムネイル / BRDF LUT）でこの値に揃える。**
// 0 に近づくと GGX の D 項が発散して half のシーンカラーに Inf が出るうえ、
// alpha^2 = roughness^4 が fp16 で表現できる範囲を割って 0 に潰れる。
// 値は業界で通っている 0.045（Filament / Frostbite と同じ）。
// 根拠は docs/design/rendering.md の「ラフネスの下限」を参照。
static const float kMinPerceptualRoughness = 0.045f;

float DistributionGGX(float nDotH, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSq = alpha * alpha;
    const float denom = nDotH * nDotH * (alphaSq - 1.0f) + 1.0f;
    return alphaSq / max(kPi * denom * denom, 1e-7f);
}

// Smith の可視性項（G / (4 NoL NoV) を畳み込んだ形）。
float VisibilitySmithGgxCorrelated(float nDotV, float nDotL, float roughness)
{
    const float alpha = roughness * roughness;
    const float alphaSq = alpha * alpha;
    const float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0f - alphaSq) + alphaSq);
    const float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0f - alphaSq) + alphaSq);
    return 0.5f / max(lambdaV + lambdaL, 1e-7f);
}

float3 FresnelSchlick(float3 f0, float vDotH)
{
    const float f = pow(1.0f - vDotH, 5.0f);
    return f0 + (1.0f - f0) * f;
}

float3 FresnelSchlickRoughness(float3 f0, float nDotV, float roughness)
{
    const float3 fr = max(float3(1.0f - roughness, 1.0f - roughness, 1.0f - roughness), f0);
    // nDotV が 1 をわずかに超えると pow の底が負になり NaN が出る。saturate で守る。
    return f0 + (fr - f0) * pow(saturate(1.0f - nDotV), 5.0f);
}

float DiffuseLambert()
{
    return 1.0f / kPi;
}

// 金属度から拡散色と F0 を求める。誘電体の反射率は 0.04 で固定する。
void SplitBaseColor(float3 baseColor, float metallic, out float3 diffuseColor, out float3 f0)
{
    diffuseColor = baseColor * (1.0f - metallic);
    f0 = lerp(float3(0.04f, 0.04f, 0.04f), baseColor, metallic);
}

// 1 灯ぶんの直接光。lightDirection は「サーフェスから光源へ向かう」正規化ベクトル。
// illuminance は光源の照度（lux 相当）。
float3 ShadeDirectionalLight(float3 normal, float3 viewDirection, float3 lightDirection,
                             float3 lightColor, float illuminance, float3 diffuseColor,
                             float3 f0, float roughness)
{
    const float nDotL = saturate(dot(normal, lightDirection));
    if (nDotL <= 0.0f)
    {
        return float3(0.0f, 0.0f, 0.0f);
    }

    const float3 halfVector = normalize(viewDirection + lightDirection);
    // 上限を超えないよう clamp する（saturate + 加算だと最大 1.00001 になり、
    // 1 - nDotV を底に取る pow が NaN を作る）。
    const float nDotV = clamp(dot(normal, viewDirection), 1e-5f, 1.0f);
    const float nDotH = saturate(dot(normal, halfVector));
    const float vDotH = saturate(dot(viewDirection, halfVector));

    const float d = DistributionGGX(nDotH, roughness);
    const float vis = VisibilitySmithGgxCorrelated(nDotV, nDotL, roughness);
    const float3 f = FresnelSchlick(f0, vDotH);

    const float3 specular = d * vis * f;
    const float3 diffuse = diffuseColor * DiffuseLambert() * (1.0f - f);

    return (diffuse + specular) * lightColor * illuminance * nDotL;
}

// --- IBL 用 --------------------------------------------------------------

// Hammersley 点列。低食い違い量列で、環境マップの畳み込みに使う。
float2 Hammersley(uint index, uint count)
{
    uint bits = index;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    const float radicalInverse = float(bits) * 2.3283064365386963e-10f;
    return float2(float(index) / float(count), radicalInverse);
}

// 法線 n を軸とする接空間の基底。
void BuildOrthonormalBasis(float3 n, out float3 tangent, out float3 bitangent)
{
    const float3 up = (abs(n.z) < 0.999f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    tangent = normalize(cross(up, n));
    bitangent = cross(n, tangent);
}

// GGX の法線分布に沿ってハーフベクトルをサンプリングする。
float3 ImportanceSampleGGX(float2 xi, float3 normal, float roughness)
{
    const float alpha = roughness * roughness;

    const float phi = 2.0f * kPi * xi.x;
    const float cosTheta = sqrt((1.0f - xi.y) / (1.0f + (alpha * alpha - 1.0f) * xi.y));
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));

    const float3 local = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

// 余弦重み付きの半球サンプリング。irradiance の畳み込みに使う。
float3 ImportanceSampleCosine(float2 xi, float3 normal)
{
    const float phi = 2.0f * kPi * xi.x;
    const float cosTheta = sqrt(1.0f - xi.y);
    const float sinTheta = sqrt(xi.y);

    const float3 local = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);

    float3 tangent;
    float3 bitangent;
    BuildOrthonormalBasis(normal, tangent, bitangent);
    return normalize(tangent * local.x + bitangent * local.y + normal * local.z);
}

// IBL 用の Smith 遮蔽項（k = alpha / 2）。直接光とは k の定義が異なる。
float GeometrySmithIbl(float nDotV, float nDotL, float roughness)
{
    const float alpha = roughness * roughness;
    const float k = alpha * 0.5f;
    const float gv = nDotV / (nDotV * (1.0f - k) + k);
    const float gl = nDotL / (nDotL * (1.0f - k) + k);
    return gv * gl;
}

#endif  // TG_BRDF_HLSLI
