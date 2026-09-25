#include "AtmosphereCommon.hlsli"
#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "Tonemap.hlsli"
#include "ImpostorCommon.hlsli"

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
    // lodView: LOD の色分け表示。ベースカラーのマップは色に使わず（アルファ抜きには使う）、ティントが段の色。
    uint visibleOffset, lodView;
    // インポスター（ImpostorCommon.hlsli）。色は sRGB で符号化した RGBA8、法線は 8 面体 + 深度 + ラフネス。
    uint impostorColor, impostorNormal;
    float3 impostorCenter; float impostorRadius;
    // impostorShadow: 影パス。板を光源へ向け、光の向きから見た画像で抜く（PsImpostorShadow）。
    // impostorVariation: 画素ごとの「色むらを受ける割合」（r）。無効なら全画素 1。
    uint impostorFrames, impostorFullSphere, impostorShadow, impostorVariation;
    // 色むら（MaterialLibrary.h の ColorVariation）。pointAttributes は点の属性（x = 色むら）で、
    // 無効なら中立の 0.5。variationLow / High は値 0 / 1 の端の調整（色相ラジアン, 彩度, 明度, 未使用）。
    uint pointAttributes; float variationJitter; uint2 variationPadding;
    float4 variationLow, variationHigh;
};

ConstantBuffer<ModelConstants> g_model : register(b1);

float ModelShadow(float3 position, float nDotL, uint cascade) {
    uint index = g_model.shadows.indices[cascade];
    if (index == kInvalidTextureIndex) return 1;
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
// variation: 株の色むらの値（0〜1、0.5 が中立）。
struct PixelInput { float4 clip:SV_POSITION; float3 position:POSITION; float3 normal:NORMAL; float4 tangent:TANGENT; float2 uv:TEXCOORD0; nointerpolation float fade:FADE; nointerpolation float variation:VARIATION; };
uint InstanceHash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }
float InstanceRandom(uint x) { return float(InstanceHash(x) >> 8) / 16777216.0; }
// 配置した 1 株の置き方。モデル空間の点 p は origin + axisX*l.x + up*(l.y+offset) + axisZ*l.z
// （l = (p - pivot) * scale）へ移る。軸は正規直交。
struct InstancePlacement { float3 origin, axisX, up, axisZ; float scale, fade, variation; };
InstancePlacement IdentityPlacement() {
    InstancePlacement result;
    result.origin = g_model.pivot; result.axisX = float3(1, 0, 0); result.up = float3(0, 1, 0);
    result.axisZ = float3(0, 0, 1); result.scale = 1; result.fade = 1; result.variation = 0.5;
    return result;
}
// 株の色むらの値。点の属性（散布の Variation）に、株ごとの乱数で個体差を足す。
float InstanceVariation(uint instance) {
    float value = 0.5;
    if (g_model.pointAttributes != kInvalidTextureIndex) {
        Texture2D<float4> attributes = ResourceDescriptorHeap[g_model.pointAttributes];
        value = attributes.Load(int3(instance % 1024, instance / 1024, 0)).x;
    }
    value += (InstanceRandom(instance ^ g_model.seed ^ 0x5bd1u) * 2 - 1) * g_model.variationJitter;
    return saturate(value);
}
// 色むらの値でベースカラーを寄せる。0.5 から離れるほど、その側の端の調整へ線形に近づく。
float3 ApplyColorVariation(float3 color, float value) {
    const float amount = saturate(abs(value - 0.5f) * 2);
    if (amount <= 0) return color;
    const float4 end = value < 0.5f ? g_model.variationLow : g_model.variationHigh;
    return AdjustBaseColor(color, end.x * amount, lerp(1, end.y, amount), lerp(1, end.z, amount));
}
// SV_InstanceID から、可視リストの区画を通して株を引く。
InstancePlacement LoadInstance(uint instance) {
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[g_model.visibleIndices];
    const uint2 entry = visible[g_model.visibleOffset + instance];
    instance = entry.x;
    Texture2D<float4> points = ResourceDescriptorHeap[g_model.points];
    const uint2 address = uint2(instance%1024,instance/1024);
    const float4 placement = points.Load(int3(address,0));
    const float4 orientation = points.Load(int3(address+uint2(0,g_model.rows),0));
    InstancePlacement result;
    result.up = normalize(lerp(float3(0,1,0),orientation.xyz,g_model.align));
    const float3 right = normalize(cross(abs(result.up.z)<0.99 ? float3(0,0,1) : float3(1,0,0),result.up));
    const float3 forward = cross(right,result.up);
    const float angle = orientation.w + InstanceRandom(instance ^ g_model.seed ^ 0xa6e1u)*6.2831853;
    const float c = cos(angle), s = sin(angle);
    result.axisX = right*c+forward*s; result.axisZ = forward*c-right*s;
    result.scale = lerp(g_model.scaleMin,g_model.scaleMax,InstanceRandom(instance ^ g_model.seed ^ 0x3187u)) *
                   (g_model.usePointSize != 0 ? placement.w/g_model.modelSize : 1);
    result.origin = placement.xyz;
    result.fade = asfloat(entry.y);
    result.variation = InstanceVariation(instance);
    return result;
}
float3 PlacePoint(InstancePlacement p, float3 position) {
    const float3 local = (position-g_model.pivot)*p.scale;
    return p.origin + p.axisX*local.x + p.up*(local.y+g_model.offset) + p.axisZ*local.z;
}
float3 PlaceDirection(InstancePlacement p, float3 direction) {
    return p.axisX*direction.x + p.up*direction.y + p.axisZ*direction.z;
}

PixelInput VsMain(VertexInput input, uint instance : SV_InstanceID) {
    float fade = 1, variation = 0.5;
    if (g_model.sceneMode != 0) {
        const InstancePlacement placement = LoadInstance(instance);
        fade = placement.fade;
        variation = placement.variation;
        input.position = PlacePoint(placement, input.position);
        input.normal = PlaceDirection(placement, input.normal);
        input.tangent.xyz = PlaceDirection(placement, input.tangent.xyz);
    }
    PixelInput output;
    output.clip=mul(float4(input.position,1),g_model.viewProjection);
    output.position=input.position; output.normal=input.normal; output.tangent=input.tangent; output.uv=input.uv;
    output.fade=fade;
    output.variation=variation;
    return output;
}

// 陰影（ビューポートと同じ式）。メッシュとインポスターで共有する。position はワールド、normal は陰影に使う向き。
float4 ShadeModel(float3 position, float3 normal, float3 viewDirection, float3 baseColor,
                  float roughness, float metallic, float ambientOcclusion) {
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_model.lightDirection);
    // 直射光にはシャドウマップと雲影の両方を掛ける（地形の MeshPbr と同じ）。
    float visibility = ModelVisibility(position,dot(normal,lightDirection));
    if (g_model.sceneMode != 0 && g_model.atmosphericMode != 0)
        visibility *= CloudShadow(position, g_model.atmosphere, g_model.cloudNoiseIndex);
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
            g_model.irradianceIndex, blendAmbient ? g_model.clearIrradianceIndex : kInvalidTextureIndex, normal,
            position.y, g_model.ambientLow, g_model.ambientHigh, g_model.ambientOcclusion);
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
        if (g_model.lodView == 0) baseColor *= sampled.rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_model.colorAdjust.x, g_model.colorAdjust.y,
                                g_model.brightness);
    if (g_model.lodView == 0) baseColor = ApplyColorVariation(baseColor, input.variation);

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

    return ShadeModel(input.position, normal, viewDirection, baseColor, roughness, metallic, ambientOcclusion);
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

// --- インポスター -----------------------------------------------------------------
// カメラを向く四角形 1 枚。ピクセルごとに、視線に近い 3 方向の画像それぞれの平面（中心を通り、
// その方向に垂直）へ視線を当て、当たった位置の画素を重みで混ぜる。
// 株ごとの置き方は補間せずに渡し、ピクセルでは視線をモデル空間へ戻して画像を選ぶ。
// origin / scale は「モデル空間の点 → ワールド」の変換（InstancePlacement と同じ）。
struct ImpostorInput {
    float4 clip:SV_POSITION; float3 position:POSITION;
    nointerpolation float3 origin:ORIGIN; nointerpolation float3 axisX:AXISX; nointerpolation float3 up:AXISY;
    nointerpolation float scale:SCALE; nointerpolation float fade:FADE; nointerpolation float variation:VARIATION;
};

ImpostorInput VsImpostor(uint vertex : SV_VertexID, uint instance : SV_InstanceID) {
    static const float2 kCorners[6] = {float2(-1, -1), float2(1, -1), float2(1, 1),
                                       float2(-1, -1), float2(1, 1), float2(-1, 1)};
    InstancePlacement placement = IdentityPlacement();
    if (g_model.sceneMode != 0) placement = LoadInstance(instance);
    const float3 center = PlacePoint(placement, g_model.impostorCenter);
    const float radius = g_model.impostorRadius * placement.scale;
    const float3 toCamera = g_model.cameraPosition - center;
    const float cameraDistance = length(toCamera);
    float3 right, up;
    // 影パスは平行光の正射影。板を光源へ向け、半径ぶんだけ覆う。
    const bool shadow = g_model.impostorShadow != 0;
    ImpostorFrameBasis(shadow ? normalize(g_model.lightDirection) : toCamera / max(cameraDistance, 1e-4f), right, up);
    // 透視では球の輪郭が中心の平面上で半径より少し大きく見えるので、その分だけ広げる。
    const float extent = shadow ? radius
        : cameraDistance > radius * 1.01f
        ? radius * cameraDistance / sqrt(cameraDistance * cameraDistance - radius * radius)
        : radius * 8;
    ImpostorInput output;
    output.position = center + (right * kCorners[vertex % 6].x + up * kCorners[vertex % 6].y) * extent;
    output.clip = mul(float4(output.position, 1), g_model.viewProjection);
    output.origin = placement.origin; output.axisX = placement.axisX; output.up = placement.up;
    output.scale = placement.scale; output.fade = placement.fade; output.variation = placement.variation;
    return output;
}

// 焼いた画像を 1 本の視線で引いた結果（モデル空間）。
struct ImpostorHit {
    float coverage, roughness;
    float variationWeight;  // 色むらを受ける割合（葉 1、幹 0）
    float3 color;    // リニア
    float3 normal;   // モデル空間
    float3 surface;  // 焼いた深度から戻した表面の位置（モデル空間）
};
// eye を通り ray へ進む視線で引く。toViewer はマスを選ぶ向き（透視なら株の中心からカメラ、
// 平行光なら光源の向き）。3 マスそれぞれの平面へ視線を当て、覆いで重みを付けて混ぜる。
ImpostorHit SampleImpostor(float3 eye, float3 ray, float3 toViewer) {
    const float3 center = g_model.impostorCenter;
    const float radius = g_model.impostorRadius;
    const uint frames = g_model.impostorFrames;
    const bool fullSphere = g_model.impostorFullSphere != 0;
    const ImpostorFrames selected = SelectImpostorFrames(toViewer, frames, fullSphere);
    Texture2D<float4> colorMap = ResourceDescriptorHeap[g_model.impostorColor];
    Texture2D<float4> normalMap = ResourceDescriptorHeap[g_model.impostorNormal];
    Texture2D<float4> variationMap = ResourceDescriptorHeap[g_model.impostorVariation != kInvalidTextureIndex
                                                                ? g_model.impostorVariation : g_model.impostorColor];

    float2 atlasUv[3];
    float3 hit[3], direction[3];
    bool inside[3];
    [unroll] for (uint k = 0; k < 3; ++k) {
        direction[k] = ImpostorFrameDirection(selected.frame[k], frames, fullSphere);
        float3 right, up;
        ImpostorFrameBasis(direction[k], right, up);
        const float denominator = dot(ray, direction[k]);
        const float t = dot(center - eye, direction[k]) / (abs(denominator) > 1e-4f ? denominator : 1e-4f);
        hit[k] = eye + ray * t;
        const float3 local = hit[k] - center;
        const float2 tile = float2(0.5f + dot(local, right) / (2 * radius), 0.5f - dot(local, up) / (2 * radius));
        inside[k] = all(tile >= 0) && all(tile <= 1);
        atlasUv[k] = (float2(selected.frame[k]) + saturate(tile)) / float(frames);
    }
    // ミップは最も重いマスの座標の変化で決める（分岐の前に微分を取る）。
    const float lod = MapLod(g_model.impostorColor, ddx(atlasUv[0]), ddy(atlasUv[0]));

    ImpostorHit result;
    result.coverage = 0; result.roughness = 0; result.variationWeight = 0;
    result.color = 0; result.normal = 0; result.surface = 0;
    float2 normalSum = 0;
    [unroll] for (uint k = 0; k < 3; ++k) {
        if (!inside[k]) continue;
        const float4 c = colorMap.SampleLevel(g_samplerLinearClamp, atlasUv[k], lod);
        const float4 n = normalMap.SampleLevel(g_samplerLinearClamp, atlasUv[k], lod);
        const float weight = selected.weight[k] * c.a;
        result.color += SrgbToLinear(c.rgb) * weight;
        normalSum += n.xy * weight;
        result.roughness += n.w * weight;
        // 重みのアトラスが無い（作り直す前の）インポスターは、全画素が受ける。
        result.variationWeight += (g_model.impostorVariation != kInvalidTextureIndex
            ? variationMap.SampleLevel(g_samplerLinearClamp, atlasUv[k], lod).r : 1) * weight;
        // 深度は「中心を通る平面から、撮った向きへどれだけ手前か」（ImpostorBake.hlsl）。
        result.surface += (hit[k] + direction[k] * (n.z - 0.5f) * 2 * radius) * weight;
        result.coverage += weight;
    }
    const float inverse = 1 / max(result.coverage, 1e-4f);
    result.color *= inverse;
    result.roughness *= inverse;
    result.variationWeight *= inverse;
    result.surface *= inverse;
    result.normal = DecodeImpostorNormal(normalSum * inverse * 2 - 1);
    // メッシュのアルファ抜きと同じく、遠くで痩せないようミップ段に応じて持ち上げる。
    result.coverage *= 1 + max(lod, 0) * kAlphaMipScale;
    return result;
}
// 株の置き方（ImpostorInput）でワールドとモデル空間を行き来する。
float3 ImpostorToModel(ImpostorInput input, float3 world) {
    const float3 axisZ = cross(input.axisX, input.up);
    const float3 relative = world - (input.origin + input.up * g_model.offset);
    return float3(dot(relative, input.axisX), dot(relative, input.up), dot(relative, axisZ)) / input.scale + g_model.pivot;
}
float3 ImpostorDirectionToModel(ImpostorInput input, float3 direction) {
    return float3(dot(direction, input.axisX), dot(direction, input.up), dot(direction, cross(input.axisX, input.up)));
}
float3 ImpostorToWorld(ImpostorInput input, float3 model) {
    const float3 local = (model - g_model.pivot) * input.scale;
    return input.origin + input.axisX * local.x + input.up * (local.y + g_model.offset) + cross(input.axisX, input.up) * local.z;
}

float4 PsImpostor(ImpostorInput input):SV_TARGET {
    const float3 camera = ImpostorToModel(input, g_model.cameraPosition);
    const float3 target = ImpostorToModel(input, input.position);
    const ImpostorHit hit = SampleImpostor(camera, normalize(target - camera), normalize(camera - g_model.impostorCenter));
    clip(hit.coverage - 0.5f);
    const float3 axisZ = cross(input.axisX, input.up);
    const float3 normal = normalize(input.axisX * hit.normal.x + input.up * hit.normal.y + axisZ * hit.normal.z);
    // 影と環境光の高さは、板の上の点ではなく焼いた深度から戻した表面で引く（影を落とす側と揃える）。
    const float3 surface = ImpostorToWorld(input, hit.surface);
    // 色むらはメッシュと同じ式で、葉の画素（重み）にだけ掛ける。
    const float3 baseColor = g_model.lodView != 0 ? g_model.baseColorTint
        : lerp(hit.color, ApplyColorVariation(hit.color, input.variation), hit.variationWeight);
    return ShadeModel(surface, normal, normalize(g_model.cameraPosition - input.position), baseColor,
                      hit.roughness, 0, 1);
}

// 影パス。光の向きの平行な視線で引き、焼いた深度から戻した表面の深度を書く
// （板の平面の深度だと、影が中心の平面へつぶれ、受ける側とも食い違う）。
float PsImpostorShadow(ImpostorInput input):SV_Depth {
    const float3 toLight = ImpostorDirectionToModel(input, normalize(g_model.lightDirection));
    const float3 target = ImpostorToModel(input, input.position);
    const ImpostorHit hit = SampleImpostor(target + toLight * (4 * g_model.impostorRadius), -toLight, toLight);
    clip(hit.coverage - 0.5f);
    const float4 projected = mul(float4(ImpostorToWorld(input, hit.surface), 1), g_model.viewProjection);
    return saturate(projected.z / projected.w);
}

// インポスター段の切り替え中の区画（PsDither と同じ相補的なディザ）。
float4 PsImpostorDither(ImpostorInput input):SV_TARGET {
    const float threshold = LodDitherThreshold(uint2(input.clip.xy));
    if (input.fade > 1.5f) clip(threshold - (input.fade - 2));
    else clip(input.fade - threshold);
    return PsImpostor(input);
}
