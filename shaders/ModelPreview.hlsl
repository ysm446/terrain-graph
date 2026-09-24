#include "AtmosphereCommon.hlsli"
#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

// ModelPreview.cpp と同じ並び。単位はm、UVは読み込み時に画像座標へ変換済み。
struct SceneShadowData {
    float4x4 view;
    float4x4 matrices[4];
    uint4 indices;
    float4 splits, biases;
    float nearDistance, texel, blend; uint count;
};
struct ModelConstants
{
    uint baseColorIndex, normalIndex, roughnessIndex, metallicIndex;
    uint aoIndex, mapChannels, flipNormalGreen, irradianceIndex;
    uint prefilteredIndex, brdfLutIndex, prefilteredMipCount, tonemapMode;
    float3 baseColorTint; float roughnessValue;
    float metallicValue, aoValue; float2 colorAdjust;
    // ambientLow / High: 環境光を雲あり / 雲なしで混ぜる高さの範囲（m）。SampleAmbientIrradiance を参照。
    // alphaCutoff: ベースカラーのアルファがこれ未満の画素を捨てる。0 で不透明。
    float brightness, ambientLow, ambientHigh, alphaCutoff;
    float3 cameraPosition; float exposure;
    float3 lightDirection; float lightIlluminance;
    float3 lightColor; float iblIntensity;
    float4x4 viewProjection;
    uint points, rows, visibleIndices, seed;
    float weightStart, weightEnd, scaleMin, scaleMax;
    float3 pivot; float modelSize;
    float align, offset; uint usePointSize, sceneMode;
    SceneShadowData shadows;
    // 雲影（MeshPbr と同じ CloudShadow）。atmosphericMode が 0 なら使わない。
    AtmosphericParameters atmosphere;
    // clearIrradianceIndex: 雲なしの環境の irradiance（0xFFFFFFFF なら混ぜない）。ambientOcclusion: 遮蔽の強さ。
    uint cloudNoiseIndex, atmosphericMode, clearIrradianceIndex; float ambientOcclusion;
    // visibleOffset: 可視リストの区画の先頭（SV_InstanceID は StartInstance を含まない）。
    uint visibleOffset; uint3 padding;
};

ConstantBuffer<ModelConstants> g_model : register(b1);

float ModelShadow(float3 position, float nDotL, uint cascade) {
    uint index = g_model.shadows.indices[cascade];
    if (index == 0xffffffffu) return 1;
    float4 clip = mul(g_model.shadows.matrices[cascade],float4(position,1));
    float3 ndc = clip.xyz/clip.w;
    float2 uv = ndc.xy*float2(0.5,-0.5)+0.5;
    if (clip.w <= 0 || any(uv<0) || any(uv>1) || ndc.z>1) return 1;
    Texture2D<float> shadowMap=ResourceDescriptorHeap[NonUniformResourceIndex(index)];
    float visibility=0;
    float bias=g_model.shadows.biases[cascade]*(1+3*(1-saturate(nDotL)));
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
        visibility += ndc.z-bias <= shadowMap.SampleLevel(g_samplerPointClamp,uv+float2(x,y)*g_model.shadows.texel,0) ? 1 : 0;
    return visibility/9;
}
float ModelVisibility(float3 position, float nDotL) {
    if (g_model.shadows.count==0) return 1;
    if (g_model.shadows.count==1) return ModelShadow(position,nDotL,0);
    float distance=-mul(g_model.shadows.view,float4(position,1)).z;
    const uint lastCascade=g_model.shadows.count-1;
    if(distance>g_model.shadows.splits[lastCascade]) return 1;
    uint cascade=0; while(cascade<lastCascade && distance>g_model.shadows.splits[cascade]) ++cascade;
    float visibility=ModelShadow(position,nDotL,cascade);
    float start=cascade==0?g_model.shadows.nearDistance:g_model.shadows.splits[cascade-1];
    float end=g_model.shadows.splits[cascade];
    float blendStart=end-(end-start)*g_model.shadows.blend;
    if(distance<=blendStart) return visibility;
    float next=cascade<lastCascade?ModelShadow(position,nDotL,min(cascade+1,lastCascade)):1;
    return lerp(visibility,next,smoothstep(blendStart,end,distance));
}

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
// fade: LOD の切り替えの進み具合（InstanceCulling.hlsl）。1 なら抜かない。
struct PixelInput { float4 clip:SV_POSITION; float3 position:POSITION; float3 normal:NORMAL; float4 tangent:TANGENT; float2 uv:TEXCOORD0; nointerpolation float fade:FADE; };
uint InstanceHash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }
float InstanceRandom(uint x) { return float(InstanceHash(x) >> 8) / 16777216.0; }
PixelInput VsMain(VertexInput input, uint instance : SV_InstanceID) {
    float fade = 1;
    if (g_model.sceneMode != 0) {
        StructuredBuffer<uint2> visible = ResourceDescriptorHeap[g_model.visibleIndices];
        const uint2 entry = visible[g_model.visibleOffset + instance];
        instance = entry.x;
        fade = asfloat(entry.y);
        Texture2D<float4> points = ResourceDescriptorHeap[g_model.points];
        const uint2 address = uint2(instance%1024,instance/1024);
        const float4 placement = points.Load(int3(address,0));
        const float4 orientation = points.Load(int3(address+uint2(0,g_model.rows),0));
        float3 up = normalize(lerp(float3(0,1,0),orientation.xyz,g_model.align));
        float3 right = normalize(cross(abs(up.z)<0.99 ? float3(0,0,1) : float3(1,0,0),up));
        float3 forward = cross(right,up);
        const float angle = orientation.w + InstanceRandom(instance ^ g_model.seed ^ 0xa6e1u)*6.2831853;
        const float c = cos(angle), s = sin(angle);
        const float3 axisX = right*c+forward*s, axisZ = forward*c-right*s;
        const float scale = lerp(g_model.scaleMin,g_model.scaleMax,InstanceRandom(instance ^ g_model.seed ^ 0x3187u)) *
                            (g_model.usePointSize != 0 ? placement.w/g_model.modelSize : 1);
        const float3 local = (input.position-g_model.pivot)*scale;
        input.position = placement.xyz + axisX*local.x + up*(local.y+g_model.offset) + axisZ*local.z;
        input.normal = axisX*input.normal.x + up*input.normal.y + axisZ*input.normal.z;
        input.tangent.xyz = axisX*input.tangent.x + up*input.tangent.y + axisZ*input.tangent.z;
    }
    PixelInput output;
    output.clip=mul(float4(input.position,1),g_model.viewProjection);
    output.position=input.position; output.normal=input.normal; output.tangent=input.tangent; output.uv=input.uv;
    output.fade=fade;
    return output;
}
// アルファ抜きの判定。ミップはアルファも平均するので、遠くほど閾値を超える画素が減って
// 葉が痩せる。ミップが1段進むごとにアルファを持ち上げ、見かけの被覆を保つ。
static const float kAlphaMipScale = 0.25f;
void ClipAlpha(float alpha, float lod) {
    if (g_model.alphaCutoff <= 0) return;
    clip(alpha * (1 + max(lod, 0) * kAlphaMipScale) - g_model.alphaCutoff);
}
// 影パス（アルファ抜きのパーツだけ）。深度だけを書くので色は返さない。
void PsShadow(PixelInput input) {
    const float lod = MapLod(g_model.baseColorIndex, ddx(input.uv), ddy(input.uv));
    ClipAlpha(SampleMap(g_model.baseColorIndex, input.uv, lod).a, lod);
}
float4 PsMain(PixelInput input, bool frontFace:SV_IsFrontFace):SV_TARGET {
    const float3 normalGeometric=normalize(input.normal);
    const float3 viewDirection=normalize(g_model.cameraPosition-input.position);
    const float2 uv=input.uv;
    const float2 deltaX=ddx(uv),deltaY=ddy(uv);
    // 面の法線（向きは問わない）。clip より前に微分を取る。
    const float3 faceNormal=normalize(cross(ddx(input.position),ddy(input.position)));
    float3 baseColor = g_model.baseColorTint;
    if (g_model.baseColorIndex != kInvalidTextureIndex)
    {
        const float lod = MapLod(g_model.baseColorIndex, deltaX, deltaY);
        const float4 sampled = SampleMap(g_model.baseColorIndex, uv, lod);
        ClipAlpha(sampled.a, lod);
        baseColor *= sampled.rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_model.colorAdjust.x, g_model.colorAdjust.y,
                                g_model.brightness);

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
    // 両面のマテリアルを裏から見たとき。面に対して鏡映し、面に沿った成分（葉のカードで
    // 上や外へ曲げた法線）は残す。平らな法線なら単純な反転と同じ。
    // 閉じたメッシュの裏面は奥で隠れるので影響しない。
    if (!frontFace) normal = reflect(normal, faceNormal);

    // --- 陰影（ビューポートと同じ式）---------------------------------------
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_model.lightDirection);
    // 直射光にはシャドウマップと雲影の両方を掛ける（地形の MeshPbr と同じ）。
    float visibility = ModelVisibility(input.position,dot(normal,lightDirection));
    if (g_model.sceneMode != 0 && g_model.atmosphericMode != 0)
        visibility *= CloudShadow(input.position, g_model.atmosphere, g_model.cloudNoiseIndex);
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_model.lightColor, g_model.lightIlluminance,
                                            diffuseColor, f0, clampedRoughness) * visibility;

    if (g_model.irradianceIndex != kInvalidTextureIndex)
    {
        // MeshPbr と同じ分割和近似。nDotV は 1 を超えると NaN になるので clamp で守る。
        const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

        TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_model.prefilteredIndex];
        Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_model.brdfLutIndex];

        // 雲の上に置かれたモデルは、雲なしの環境で照らす（地形の MeshPbr と同じ）。
        const bool blendAmbient = g_model.sceneMode != 0 && g_model.atmosphericMode != 0;
        const float3 irradiance = SampleAmbientIrradiance(
            g_model.irradianceIndex, blendAmbient ? g_model.clearIrradianceIndex : 0xffffffffu, normal,
            input.position.y, g_model.ambientLow, g_model.ambientHigh, g_model.ambientOcclusion);
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


    if (g_model.sceneMode != 0) return float4(radiance,1);
    return float4(LinearToSrgb(ApplyTonemap(radiance*g_model.exposure,g_model.tonemapMode)),1);
}

// LOD の切り替え中の区画。4x4 の Bayer 配列で、来る段は閾値が t 未満の画素、
// 去る段は t 以上の画素だけを残す。両段で画素を分け合うので、隙間も二重描きも出ない。
float LodDitherThreshold(uint2 pixel) {
    static const uint kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (kBayer[(pixel.y & 3) * 4 + (pixel.x & 3)] + 0.5f) / 16.0f;
}
float4 PsDither(PixelInput input, bool frontFace:SV_IsFrontFace):SV_TARGET {
    const float threshold = LodDitherThreshold(uint2(input.clip.xy));
    if (input.fade > 1.5f) clip(threshold - (input.fade - 2));
    else clip(input.fade - threshold);
    return PsMain(input, frontFace);
}
