#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

// ModelPreview.cpp と同じ並び。単位はm、UVは読み込み時に画像座標へ変換済み。
struct ModelConstants
{
    uint baseColorIndex, normalIndex, roughnessIndex, metallicIndex;
    uint aoIndex, mapChannels, flipNormalGreen, irradianceIndex;
    uint prefilteredIndex, brdfLutIndex, prefilteredMipCount, tonemapMode;
    float3 baseColorTint; float roughnessValue;
    float metallicValue, aoValue; float2 colorAdjust;
    float3 cameraPosition; float exposure;
    float3 lightDirection; float lightIlluminance;
    float3 lightColor; float iblIntensity;
    float4x4 viewProjection;
};

ConstantBuffer<ModelConstants> g_model : register(b1);

float MapLod(uint index, float2 deltaX, float2 deltaY)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    float2 dimensions;
    float levels;
    map.GetDimensions(0, dimensions.x, dimensions.y, levels);

    const float2 dx = deltaX * dimensions;
    const float2 dy = deltaY * dimensions;
    return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8f));
}

float4 SampleMap(uint index, float2 uv, float lod)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    return map.SampleLevel(g_samplerLinearWrap, uv, lod);
}

float SampleScalarMap(uint index, uint channelSlot, float2 uv, float lod)
{
    return SelectChannel(SampleMap(index, uv, lod),
                         UnpackChannel(g_model.mapChannels, channelSlot));
}


struct VertexInput { float3 position:POSITION; float3 normal:NORMAL; float4 tangent:TANGENT; float2 uv:TEXCOORD0; };
struct PixelInput { float4 clip:SV_POSITION; float3 position:POSITION; float3 normal:NORMAL; float4 tangent:TANGENT; float2 uv:TEXCOORD0; };
PixelInput VsMain(VertexInput input) {
    PixelInput output;
    output.clip=mul(float4(input.position,1),g_model.viewProjection);
    output.position=input.position; output.normal=input.normal; output.tangent=input.tangent; output.uv=input.uv;
    return output;
}
float4 PsMain(PixelInput input):SV_TARGET {
    const float3 normalGeometric=normalize(input.normal);
    const float3 viewDirection=normalize(g_model.cameraPosition-input.position);
    const float2 uv=input.uv;
    const float2 deltaX=ddx(uv),deltaY=ddy(uv);
    float3 baseColor = g_model.baseColorTint;
    if (g_model.baseColorIndex != kInvalidTextureIndex)
    {
        baseColor *= SampleMap(g_model.baseColorIndex, uv,
                               MapLod(g_model.baseColorIndex, deltaX, deltaY))
                         .rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_model.colorAdjust.x, g_model.colorAdjust.y);

    float roughness = g_model.roughnessValue;
    if (g_model.roughnessIndex != kInvalidTextureIndex)
    {
        roughness = SampleScalarMap(g_model.roughnessIndex, TG_CHANNEL_SLOT_ROUGHNESS, uv,
                                    MapLod(g_model.roughnessIndex, deltaX, deltaY));
    }

    float metallic = g_model.metallicValue;
    if (g_model.metallicIndex != kInvalidTextureIndex)
    {
        metallic = SampleScalarMap(g_model.metallicIndex, TG_CHANNEL_SLOT_METALLIC, uv,
                                   MapLod(g_model.metallicIndex, deltaX, deltaY));
    }

    float ambientOcclusion = g_model.aoValue;
    if (g_model.aoIndex != kInvalidTextureIndex)
    {
        ambientOcclusion = SampleScalarMap(g_model.aoIndex, TG_CHANNEL_SLOT_AO, uv,
                                           MapLod(g_model.aoIndex, deltaX, deltaY));
    }

    // --- 法線 --------------------------------------------------------------
    // UVと幾何法線から作った接線を使用する。緑は画像の+V方向。
    float3 normal = normalGeometric;
    if (g_model.normalIndex != kInvalidTextureIndex)
    {
        float3 sampled = SampleMap(g_model.normalIndex, uv,
                                   MapLod(g_model.normalIndex, deltaX, deltaY))
                                 .rgb *
                             2.0f - 1.0f;
        if (g_model.flipNormalGreen != 0u)
        {
            sampled.y = -sampled.y;
        }

        const float3 tangent=normalize(input.tangent.xyz-normalGeometric*dot(input.tangent.xyz,normalGeometric));
        const float3 bitangent=cross(normalGeometric,tangent)*input.tangent.w;
        normal=normalize(tangent*sampled.x+bitangent*sampled.y+normalGeometric*sampled.z);
    }

    // --- 陰影（ビューポートと同じ式）---------------------------------------
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_model.lightDirection);
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_model.lightColor, g_model.lightIlluminance,
                                            diffuseColor, f0, clampedRoughness);

    if (g_model.irradianceIndex != kInvalidTextureIndex)
    {
        // MeshPbr と同じ分割和近似。nDotV は 1 を超えると NaN になるので clamp で守る。
        const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

        TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_model.irradianceIndex];
        TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_model.prefilteredIndex];
        Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_model.brdfLutIndex];

        const float3 irradiance =
            irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;
        const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, clampedRoughness);
        const float3 diffuseIbl = (1.0f - fresnel) * diffuseColor * irradiance;

        const float3 reflectionDirection = reflect(-viewDirection, normal);
        const float mipLevel =
            clampedRoughness * float(max(g_model.prefilteredMipCount, 1u) - 1u);
        const float3 prefiltered =
            prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;
        const float2 environmentBrdf =
            brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, clampedRoughness), 0.0f);
        const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

        radiance += (diffuseIbl + specularIbl) * g_model.iblIntensity * ambientOcclusion;
    }


    return float4(LinearToSrgb(ApplyTonemap(radiance*g_model.exposure,g_model.tonemapMode)),1);
}
