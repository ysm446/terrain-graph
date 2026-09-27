// CPU で作ったユニークなメッシュ（Road Mesh の路面など）。Mesh Output が描く。
//
// 材質は焼かずに**画素ごとに評価する**（LayerMaterial.hlsli）。UV は実寸（m）の道路座標
// （x = 左端からの横位置、y = 始点からの道のり）で、通常の Material も 1 層の Layered Material
// として同じ関数で塗る。焼いた合成テクスチャ（地形の 1 テクセル 1〜2 m）に頼らないので、
// 区画線のような細かい模様も潰れない。
//
// 陰影は ModelPreview.hlsl の ShadeModel と同じ式（直射光 × シャドウマップ × 雲影 + IBL）。
// シーンの HDR へ書くので、トーンマップはしない。
#include "AtmosphereCommon.hlsli"
#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "LayerMaterial.hlsli"

// ShadowCascades.h の SceneShadowData と同じ並び。
struct SceneShadowData {
    float4x4 view;
    float4x4 matrices[4];
    uint4 indices;
    float4 splits, biases;
    float nearDistance, texel, blend; uint count;
};
// GeneratedMeshes.cpp の GeneratedMeshConstants と同じ並び。
struct GeneratedMeshConstants {
    float4x4 viewProjection;  // 転置済み（mul(float4(p, 1), M)）
    float3 cameraPosition; float iblIntensity;
    float3 lightDirection; float lightIlluminance;
    float3 lightColor; uint hasMaterial;
    uint irradianceIndex, prefilteredIndex, brdfLutIndex, prefilteredMipCount;
    float3 fallbackColor; float fallbackRoughness;
    // 環境光を雲あり / 雲なしで混ぜる高さの範囲（m）と遮蔽の強さ（ModelPreview と同じ）。
    float ambientLow, ambientHigh, ambientOcclusion; uint clearIrradianceIndex;
    uint cloudNoiseIndex, atmosphericMode; float roadWidth; float padding;
    SceneShadowData shadows;
    AtmosphericParameters atmosphere;
    LayerMaterialData material;
};
ConstantBuffer<GeneratedMeshConstants> g_generated : register(b1);

float GeneratedShadow(float3 position, float nDotL, uint cascade) {
    const uint index = g_generated.shadows.indices[cascade];
    if (index == kInvalidTextureIndex) return 1;
    const float4 clip = mul(g_generated.shadows.matrices[cascade], float4(position, 1));
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (clip.w <= 0 || any(uv < 0) || any(uv > 1) || ndc.z > 1) return 1;
    Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(index)];
    float visibility = 0;
    const float bias = g_generated.shadows.biases[cascade] * (1 + 3 * (1 - saturate(nDotL)));
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x)
        visibility += ndc.z - bias <= shadowMap.SampleLevel(g_samplerPointClamp, uv + float2(x, y) * g_generated.shadows.texel, 0) ? 1 : 0;
    return visibility / 9;
}
float GeneratedVisibility(float3 position, float nDotL) {
    if (g_generated.shadows.count == 0) return 1;
    if (g_generated.shadows.count == 1) return GeneratedShadow(position, nDotL, 0);
    const float distance = -mul(g_generated.shadows.view, float4(position, 1)).z;
    const uint lastCascade = g_generated.shadows.count - 1;
    if (distance > g_generated.shadows.splits[lastCascade]) return 1;
    uint cascade = 0;
    while (cascade < lastCascade && distance > g_generated.shadows.splits[cascade]) ++cascade;
    const float visibility = GeneratedShadow(position, nDotL, cascade);
    const float start = cascade == 0 ? g_generated.shadows.nearDistance : g_generated.shadows.splits[cascade - 1];
    const float end = g_generated.shadows.splits[cascade];
    const float blendStart = end - (end - start) * g_generated.shadows.blend;
    if (distance <= blendStart) return visibility;
    const float next = cascade < lastCascade ? GeneratedShadow(position, nDotL, min(cascade + 1, lastCascade)) : 1;
    return lerp(visibility, next, smoothstep(blendStart, end, distance));
}

struct VertexInput { float3 position : POSITION; float3 normal : NORMAL; float4 tangent : TANGENT; float2 uv : TEXCOORD0; };
struct PixelInput { float4 clip : SV_POSITION; float3 position : POSITION; float3 normal : NORMAL; float4 tangent : TANGENT; float2 uv : TEXCOORD0; };

PixelInput VsMain(VertexInput input) {
    PixelInput output;
    output.clip = mul(float4(input.position, 1), g_generated.viewProjection);
    output.position = input.position;
    output.normal = input.normal;
    output.tangent = input.tangent;
    output.uv = input.uv;
    return output;
}

float3 ShadeGenerated(float3 position, float3 normal, float3 viewDirection, float3 baseColor,
                      float roughness, float metallic, float ambientOcclusion) {
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);
    const float3 lightDirection = normalize(g_generated.lightDirection);
    float visibility = GeneratedVisibility(position, dot(normal, lightDirection));
    if (g_generated.atmosphericMode != 0)
        visibility *= CloudShadow(position, g_generated.atmosphere, g_generated.cloudNoiseIndex);
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection, g_generated.lightColor,
                                            g_generated.lightIlluminance, diffuseColor, f0, clampedRoughness) * visibility;
    if (g_generated.irradianceIndex != kInvalidTextureIndex) {
        const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);
        TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_generated.prefilteredIndex];
        Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_generated.brdfLutIndex];
        const float3 irradiance = SampleAmbientIrradiance(
            g_generated.irradianceIndex, g_generated.atmosphericMode != 0 ? g_generated.clearIrradianceIndex : kInvalidTextureIndex,
            normal, position.y, g_generated.ambientLow, g_generated.ambientHigh, g_generated.ambientOcclusion);
        const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, clampedRoughness);
        const float3 diffuseIbl = (1.0f - fresnel) * diffuseColor * irradiance;
        const float3 reflectionDirection = reflect(-viewDirection, normal);
        const float mipLevel = clampedRoughness * float(max(g_generated.prefilteredMipCount, 1u) - 1u);
        const float3 prefiltered = prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;
        const float2 environmentBrdf = brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, clampedRoughness), 0.0f);
        const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);
        radiance += (diffuseIbl + specularIbl) * g_generated.iblIntensity * ambientOcclusion;
    }
    return radiance;
}

float4 PsMain(PixelInput input) : SV_TARGET {
    const float3 viewDirection = normalize(g_generated.cameraPosition - input.position);
    // 裏から見たら法線を返す。巻き順（SV_IsFrontFace）には頼らない（メッシュを作る側の
    // 三角形の向きと、パイプラインの表の定義を揃える約束を増やさないため）。
    float3 geometric = normalize(input.normal);
    if (dot(geometric, viewDirection) < 0) geometric = -geometric;
    // 接線（+U）と従法線（+V）。法線マップと、ワールド座標で貼る層の向きに使う。
    const float3 tangent = normalize(input.tangent.xyz - geometric * dot(input.tangent.xyz, geometric));
    const float3 bitangent = cross(geometric, tangent) * input.tangent.w;

    float3 baseColor = g_generated.fallbackColor;
    float roughness = g_generated.fallbackRoughness, metallic = 0, ambientOcclusion = 1;
    float3 normal = geometric;
    if (g_generated.hasMaterial != 0) {
        const float2 meters = input.uv;
        const float2 worldMeters = input.position.xz;
        // 画素 1 つが覆う長さ（m）。ミップの選択に使う（x: 道路座標、y: ワールド座標）。
        const float2 footprint = float2(max(length(ddx(meters)), length(ddy(meters))),
                                        max(length(ddx(worldMeters)), length(ddy(worldMeters))));
        const float2 xAxis = normalize(tangent.xz + 1e-6f);
        const float2 yAxis = normalize(bitangent.xz + 1e-6f);
        const LayerMaterialSample surface = EvaluateLayerMaterial(g_generated.material, meters, worldMeters, footprint,
                                                                  float2(1, 1), xAxis, yAxis);
        baseColor = surface.color;
        roughness = surface.surface.x;
        metallic = surface.surface.y;
        ambientOcclusion = surface.surface.z;
        normal = normalize(tangent * surface.normal.x + bitangent * surface.normal.y + geometric * surface.normal.z);
    }
    return float4(ShadeGenerated(input.position, normal, viewDirection, baseColor, roughness, metallic, ambientOcclusion), 1);
}
