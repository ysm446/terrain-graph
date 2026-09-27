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
    // 内側の境界（路肩の内側の端。境界マテリアル）。boundaryWidth が 0 なら無い。
    // boundaryFlags: bit0 = 模様を U（横切る向き）に繰り返す、bit1 = マスクを反転。
    // hasInner: 内側の帯（路面か内側の路肩）の材質があるか。innerOrigin / innerSign: この帯の
    // 横位置 x（内側の端からの距離）を、内側の帯の座標（innerOrigin + innerSign * x）へ写す。
    uint boundaryMask, boundaryHeight, boundaryFlags, hasInner;
    float boundaryWidth, boundaryRepeat, boundaryDepth, boundaryCenter;
    float innerOrigin, innerSign; float2 boundaryPadding;
    float3 innerFallbackColor; float innerFallbackRoughness;
    SceneShadowData shadows;
    AtmosphericParameters atmosphere;
    LayerMaterialData material;
    LayerMaterialData innerMaterial;
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

// 境界マテリアルの画像（R）。横切る向きはクランプ、道に沿う向きはラップで読む。
// alongU なら U が道に沿う向き（ラップ）、V が横切る向き（クランプ）。
float SampleBoundary(Texture2D<float4> image, float2 uv, bool alongU) {
    return alongU ? image.Sample(g_samplerAnisoWrapUClampV, uv).r : image.Sample(g_samplerAnisoClampUWrapV, uv).r;
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

    const float2 meters = input.uv;
    const float2 worldMeters = input.position.xz;
    // 画素 1 つが覆う長さ（m）。ミップの選択に使う（x: 道路座標、y: ワールド座標）。
    const float2 footprint = float2(max(length(ddx(meters)), length(ddy(meters))),
                                    max(length(ddx(worldMeters)), length(ddy(worldMeters))));
    const float2 xAxis = normalize(tangent.xz + 1e-6f);
    const float2 yAxis = normalize(bitangent.xz + 1e-6f);

    // この帯の材質（接線空間の法線のまま持つ）。
    float3 baseColor = g_generated.fallbackColor;
    float3 surfaceValues = float3(g_generated.fallbackRoughness, 0, 1);
    float3 tangentNormal = float3(0, 0, 1);
    if (g_generated.hasMaterial != 0) {
        const LayerMaterialSample surface = EvaluateLayerMaterial(g_generated.material, meters, worldMeters, footprint,
                                                                  float2(1, 1), xAxis, yAxis);
        baseColor = surface.color;
        surfaceValues = surface.surface;
        tangentNormal = surface.normal;
    }

    // --- 内側の境界（境界マテリアル） ---------------------------------------------
    // 内側の端から幅 boundaryWidth の中で、マスク（白 = 内側の帯）で内側の材質へ切り替え、
    // ハイトの凹凸を陰影に足す。road-material-editor の境界マテリアルと同じ読み方
    // （U = 内側から外へ横切る向き、V = 道に沿う向き。マスクとハイトは R）。
    if (g_generated.boundaryWidth > 0 && meters.x < g_generated.boundaryWidth) {
        const float across = saturate(meters.x / g_generated.boundaryWidth);
        const float along = meters.y / max(g_generated.boundaryRepeat, 0.01f);
        const bool alongU = (g_generated.boundaryFlags & 1u) != 0;
        // 横切る向きは端の画素を使い続ける（繰り返さない）。道に沿う向きだけ繰り返す。
        // サンプラーも横切る向きはクランプにする（ラップだと粗いミップで反対側の端が混ざる）。
        const float2 boundaryUv = alongU ? float2(along, across) : float2(across, along);
        float innerWeight = 0;
        if (g_generated.boundaryMask != kInvalidTextureIndex) {
            Texture2D<float4> mask = ResourceDescriptorHeap[g_generated.boundaryMask];
            innerWeight = SampleBoundary(mask, boundaryUv, alongU);
            if ((g_generated.boundaryFlags & 2u) != 0) innerWeight = 1 - innerWeight;
        }
        if (innerWeight > 1e-3f) {
            // 内側の帯の材質を、内側の帯の座標で評価する（端の外へ延ばした位置）。
            float3 innerColor = g_generated.innerFallbackColor;
            float3 innerSurface = float3(g_generated.innerFallbackRoughness, 0, 1);
            float3 innerNormal = float3(0, 0, 1);
            if (g_generated.hasInner != 0) {
                const float2 innerMeters = float2(g_generated.innerOrigin + g_generated.innerSign * meters.x, meters.y);
                const LayerMaterialSample inner = EvaluateLayerMaterial(
                    g_generated.innerMaterial, innerMeters, worldMeters, footprint, float2(1, 1),
                    xAxis * g_generated.innerSign, yAxis);
                innerColor = inner.color;
                innerSurface = inner.surface;
                // 内側の帯の x の向きがこの帯と逆なら、法線の x も返す。
                innerNormal = float3(inner.normal.x * g_generated.innerSign, inner.normal.y, inner.normal.z);
            }
            baseColor = lerp(baseColor, innerColor, innerWeight);
            surfaceValues = lerp(surfaceValues, innerSurface, innerWeight);
            tangentNormal = ReorientNormal(FlattenNormal(tangentNormal, 1 - innerWeight),
                                           FlattenNormal(innerNormal, innerWeight));
        }
        if (g_generated.boundaryHeight != kInvalidTextureIndex && g_generated.boundaryDepth > 0) {
            // ハイトの勾配（m / m）を差分で求め、法線に足す。外側の端へ向かって弱める。
            Texture2D<float4> height = ResourceDescriptorHeap[g_generated.boundaryHeight];
            const float stepMeters = max(footprint.x, 0.005f);
            const float2 perMeter = float2(1.0f / g_generated.boundaryWidth, 1.0f / max(g_generated.boundaryRepeat, 0.01f));
            const float2 du = alongU ? float2(0, perMeter.x * stepMeters) : float2(perMeter.x * stepMeters, 0);
            const float2 dv = alongU ? float2(perMeter.y * stepMeters, 0) : float2(0, perMeter.y * stepMeters);
            const float scale = 2 * g_generated.boundaryDepth * (1 - smoothstep(0.7f, 1.0f, across));
            const float hx = SampleBoundary(height, boundaryUv + du, alongU) - SampleBoundary(height, boundaryUv - du, alongU);
            const float hy = SampleBoundary(height, boundaryUv + dv, alongU) - SampleBoundary(height, boundaryUv - dv, alongU);
            const float2 gradient = float2(hx, hy) * scale / (2 * stepMeters);
            tangentNormal = ReorientNormal(normalize(float3(-gradient, 1)), tangentNormal);
        }
    }

    const float3 normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y + geometric * tangentNormal.z);
    return float4(ShadeGenerated(input.position, normal, viewDirection, baseColor, surfaceValues.x, surfaceValues.y,
                                 surfaceValues.z), 1);
}
