// マテリアルを回せる球・平面で見るプレビュー。ハイトで表面を変位する。
//
// メッシュは使わず、合成ハイトとのレイ交差を求める。**照らし方はビューポートと同じ**
// （適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。素材が本番の環境でどう見えるかを
// そのまま確かめるためで、一覧のサムネイル（MaterialThumbnail.hlsl）とは目的が違う
// （あちらは見比べるための固定 2 灯で、正面から見た円板）。
//
// 背景は環境キューブのぼかしたミップ。ビューポートの背景（Skybox）と同じ絵を、
// 同じ露出とトーンマップで出す。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "LayerMaterial.hlsli"
#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

struct SphereConstants
{
    uint outputIndex;
    uint size;               // 出力は正方形
    uint baseColorIndex;     // sRGB の SRV。kInvalidTextureIndex なら定数
    uint normalIndex;

    uint roughnessIndex;
    uint metallicIndex;
    uint aoIndex;
    uint mapChannels;        // 4bit ずつ TG_CHANNEL_SLOT_* の順

    float3 baseColorTint;
    float roughnessValue;

    float metallicValue;
    float aoValue;
    float lengthMeters;      // 映す長さ（m）。平面なら一辺、球なら直径
    uint flipNormalGreen;    // 0 以外なら法線マップの緑を反転して読む

    float3 cameraPosition;   // 球の中心は原点、半径 1
    float tanHalfFov;

    float3 lightDirection;   // サーフェスから光源へ向かう方向
    float lightIlluminance;

    float3 lightColor;
    float iblIntensity;

    uint irradianceIndex;    // kInvalidTextureIndex なら IBL を掛けない
    uint prefilteredIndex;
    uint brdfLutIndex;
    uint environmentIndex;   // 背景。kInvalidTextureIndex なら無地

    uint prefilteredMipCount;
    float backgroundMip;
    float exposure;
    uint tonemapMode;

    // ベースカラーの調整（ティントを掛けたあとに効く）。合成と同じ値を渡すこと。
    float2 colorAdjust;  // 色相（ラジアン）, 彩度
    float brightness;    // 明度（倍率）
    uint shape;
    float displacementMeters;
    uint heightIndex;
    uint heightFieldIndex;
    uint heightOutputIndex;
    LayerMaterialData layerMaterial;
};

ConstantBuffer<SphereConstants> g_sphere : register(b1);

// 背景が無いときの色（リニア）。露出を掛ける前の値なので、
// 明るさは天球を出しているときと同じくらいに見える程度で足りる。
static const float3 kFallbackBackground = float3(0.02f, 0.022f, 0.026f);

// UV の差分。u は 1 周で巻き戻るので、継ぎ目をまたぐ差は短いほうへ畳む。
// 畳まないと、継ぎ目の 1 列だけミップが最下段まで落ちて帯に見える。
float2 WrapDelta(float2 delta)
{
    return delta - round(float2(delta.x, 0.0f));
}

// 画素の大きさに見合ったミップを選ぶ。コンピュートには微分が無いので、
// 隣の画素との UV 差から自分で求める（ハードウェアの選び方と同じ式）。
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
                         UnpackChannel(g_sphere.mapChannels, channelSlot));
}

// 画素の中心から出るレイ。y は下向きの画素座標なので、上向きの基底に対して反転する。
float3 RayDirection(float2 pixel, float3 forward, float3 right, float3 up)
{
    const float2 ndc = ((pixel + 0.5f) / float(g_sphere.size)) * 2.0f - 1.0f;
    return normalize(forward + right * (ndc.x * g_sphere.tanHalfFov) -
                     up * (ndc.y * g_sphere.tanHalfFov));
}

// 単位球の法線。**外れたレイでも最も近い点の向きを返す。**
// 輪郭のぼかし（被覆）に使う画素は球を外れているので、そこで法線が無いと
// 縁の色が決まらない。外れていても連続した向きが取れるようにしておく。
float3 SphereNormal(float3 origin, float3 direction)
{
    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - 1.0f;
    const float discriminant = b * b - c;
    const float t = -b - sqrt(max(discriminant, 0.0f));
    return normalize(origin + direction * t);
}

// 球の中心から、レイまでの最短距離。1 が輪郭。
float SilhouetteDistance(float3 origin, float3 direction)
{
    const float b = dot(origin, direction);
    return sqrt(max(dot(origin, origin) - b * b, 0.0f));
}

// 合成済みハイトを一度だけ焼き、交差探索では軽いテクスチャ参照を使う。
float2 SurfaceUv(float3 p) {
    return g_sphere.shape == 1 ? p.xz * 0.5f + 0.5f : DirectionToEquirectUv(normalize(p));
}
float HeightAt(float3 p) {
    Texture2D<float> height = ResourceDescriptorHeap[g_sphere.heightFieldIndex];
    float2 uv = SurfaceUv(p);
    if (g_sphere.shape != 1) uv.y = clamp(uv.y, 0.5f / g_sphere.size, 1 - 0.5f / g_sphere.size);
    return height.SampleLevel(g_samplerLinearWrap, uv, 0);
}
float SurfaceDistance(float3 p) {
    const float displacement = (HeightAt(p) - 0.5f) * g_sphere.displacementMeters * 2 / g_sphere.lengthMeters;
    return g_sphere.shape == 1 ? p.y - displacement : length(p) - 1 - displacement;
}
// 有限の領域内で最初の交差を探す。球と平面の輪郭にもハイトを反映する。
bool TraceHeight(float3 origin, float3 direction, out float3 position) {
    const float amplitude = g_sphere.displacementMeters / g_sphere.lengthMeters;
    const float3 extent = g_sphere.shape == 1 ? float3(1, max(amplitude, 1e-5f), 1) : (1 + amplitude).xxx;
    const float3 safeDirection = float3(abs(direction.x) < 1e-6f ? 1e-6f : direction.x,
        abs(direction.y) < 1e-6f ? 1e-6f : direction.y, abs(direction.z) < 1e-6f ? 1e-6f : direction.z);
    const float3 a = (-extent - origin) / safeDirection, b = (extent - origin) / safeDirection;
    const float3 nearT = min(a,b), farT = max(a,b);
    float begin = max(max(nearT.x, nearT.y), max(nearT.z, 0));
    float end = min(farT.x, min(farT.y, farT.z));
    position = 0;
    if (end <= begin) return false;
    float previousT = begin;
    float previous = SurfaceDistance(origin + direction * begin);
    [loop] for (uint i = 1; i <= 192; ++i) {
        float t = lerp(begin, end, i / 192.0f);
        float value = SurfaceDistance(origin + direction * t);
        if (value * previous <= 0) {
            [unroll] for (uint j = 0; j < 7; ++j) {
                const float mid = (previousT + t) * 0.5f;
                const float v = SurfaceDistance(origin + direction * mid);
                if (v * previous > 0) { previousT = mid; previous = v; } else t = mid;
            }
            position = origin + direction * ((previousT + t) * 0.5f);
            return true;
        }
        previous = value; previousT = t;
    }
    return false;
}
[numthreads(8, 8, 1)]
void CsHeight(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_sphere.size)) return;
    const float2 scale = g_sphere.shape == 1 ? g_sphere.lengthMeters.xx :
        float2(kPi, kPi * 0.5f) * g_sphere.lengthMeters;
    const float2 uv = (float2(id.xy) + 0.5f) / g_sphere.size * scale;
    float value = 0.5f;
    if (g_sphere.layerMaterial.count > 0)
        value = EvaluateLayerMaterialBase(g_sphere.layerMaterial, uv, uv, scale / g_sphere.size, float2(1,0), float2(0,1)).height;
    else if (g_sphere.heightIndex != kInvalidTextureIndex)
        value = SampleScalarMap(g_sphere.heightIndex, TG_CHANNEL_SLOT_HEIGHT, uv,
            MapLod(g_sphere.heightIndex, float2(scale.x / g_sphere.size,0), float2(0,scale.y / g_sphere.size)));
    RWTexture2D<float> height = ResourceDescriptorHeap[g_sphere.heightOutputIndex];
    height[id.xy] = saturate(value);
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_sphere.size || dispatchThreadId.y >= g_sphere.size)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_sphere.outputIndex];

    // --- カメラ ------------------------------------------------------------
    // 原点を見る軌道カメラ。**仰角は C++ 側で ±85 度に制限してある**ので、
    // ここで上方向との縮退（外積が 0 になる）を気にしなくてよい。
    const float3 origin = g_sphere.cameraPosition;
    const float3 forward = normalize(-origin);
    const float3 right = normalize(cross(forward, float3(0.0f, 1.0f, 0.0f)));
    const float3 up = cross(right, forward);

    const float2 pixel = float2(dispatchThreadId.xy);
    const float3 direction = RayDirection(pixel, forward, right, up);

    // --- 背景 --------------------------------------------------------------
    float3 background = kFallbackBackground;
    if (g_sphere.environmentIndex != kInvalidTextureIndex)
    {
        TextureCube<float4> environment = ResourceDescriptorHeap[g_sphere.environmentIndex];
        background =
            environment.SampleLevel(g_samplerLinearClamp, direction, g_sphere.backgroundMip).rgb *
            g_sphere.iblIntensity;
    }

    // --- 輪郭の被覆 --------------------------------------------------------
    // 球を解析的に持っているので、判定を 0/1 にせず輪郭をまたぐ幅で滑らかにする。
    // 画素の角幅 × 距離が、球の表面での画素の大きさにあたる。
    const float pixelWidth = (2.0f * g_sphere.tanHalfFov / float(g_sphere.size)) * length(origin);
    float coverage =
        1.0f - smoothstep(1.0f - pixelWidth, 1.0f + pixelWidth,
                          SilhouetteDistance(origin, direction));
    const bool plane = g_sphere.shape == 1;
    const float planeT = abs(direction.y) > 1e-5f ? -origin.y / direction.y : -1;
    const float3 planePosition = origin + direction * planeT;
    if (plane) coverage = planeT > 0 ? 1 - smoothstep(1 - pixelWidth, 1 + pixelWidth, max(abs(planePosition.x), abs(planePosition.z))) : 0;
    float3 displacedPosition = 0;
    const bool displaced = g_sphere.displacementMeters > 0 &&
        (g_sphere.layerMaterial.count > 0 || g_sphere.heightIndex != kInvalidTextureIndex);
    if (displaced) coverage = TraceHeight(origin, direction, displacedPosition) ? 1 : 0;
    if (coverage <= 0.0f)
    {
        output[dispatchThreadId.xy] =
            float4(LinearToSrgb(ApplyTonemap(background * g_sphere.exposure,
                                             g_sphere.tonemapMode)),
                   1.0f);
        return;
    }

    // --- 球の上の点 --------------------------------------------------------
    const float3 normalGeometric = plane ? float3(0, origin.y >= 0 ? 1 : -1, 0) : (displaced ? normalize(displacedPosition) : SphereNormal(origin, direction));
    const float3 position = displaced ? displacedPosition : (plane ? planePosition : normalGeometric);  // 半径 1 なので法線と同じ
    const float3 viewDirection = normalize(origin - position);

    // **UV は素材の中の距離（m）で持つ**（レイヤーマテリアルの「1 UV = 1 m」と同じ規約）。
    // 平面は一辺 lengthMeters の板。球は直径 lengthMeters なので、緯度経度の u は赤道の
    // 周長 πL、v は極から極までの経線長 πL/2 にあたる。こうすると赤道付近の模様の大きさが
    // 同じ長さの平面と一致し、縦横比も崩れない。
    const float2 metersScale = plane ? float2(g_sphere.lengthMeters, g_sphere.lengthMeters)
                                     : float2(kPi * g_sphere.lengthMeters,
                                              kPi * g_sphere.lengthMeters * 0.5f);
    const float2 uv = (plane ? position.xz * 0.5f + 0.5f : DirectionToEquirectUv(normalGeometric)) * metersScale;
    // 隣の画素の UV。ミップを選ぶためだけに使う。
    const float2 uvX = DirectionToEquirectUv(SphereNormal(
                           origin, RayDirection(pixel + float2(1.0f, 0.0f), forward, right, up))) *
                       metersScale;
    const float2 uvY = DirectionToEquirectUv(SphereNormal(
                           origin, RayDirection(pixel + float2(0.0f, 1.0f), forward, right, up))) *
                       metersScale;
    float2 deltaX = WrapDelta(uvX - uv);
    float2 deltaY = WrapDelta(uvY - uv);
    if (plane) {
        const float3 rayX = RayDirection(pixel + float2(1,0), forward, right, up);
        const float3 rayY = RayDirection(pixel + float2(0,1), forward, right, up);
        deltaX = ((origin + rayX * (-origin.y / rayX.y)).xz - planePosition.xz) * 0.5f * metersScale;
        deltaY = ((origin + rayY * (-origin.y / rayY.y)).xz - planePosition.xz) * 0.5f * metersScale;
    }

    if (displaced) {
        // 元の球・平面との位置差を微分に混ぜない。変位後も画素幅に応じて読む。
        const float footprint = max(pixelWidth * 0.5f, 1.0f / g_sphere.size);
        deltaX = float2(footprint * metersScale.x, 0);
        deltaY = float2(0, footprint * metersScale.y);
    }
    float3 baseColor = g_sphere.baseColorTint;
    if (g_sphere.baseColorIndex != kInvalidTextureIndex)
    {
        baseColor *= SampleMap(g_sphere.baseColorIndex, uv,
                               MapLod(g_sphere.baseColorIndex, deltaX, deltaY))
                         .rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_sphere.colorAdjust.x, g_sphere.colorAdjust.y,
                                g_sphere.brightness);

    float roughness = g_sphere.roughnessValue;
    if (g_sphere.roughnessIndex != kInvalidTextureIndex)
    {
        roughness = SampleScalarMap(g_sphere.roughnessIndex, TG_CHANNEL_SLOT_ROUGHNESS, uv,
                                    MapLod(g_sphere.roughnessIndex, deltaX, deltaY));
    }

    float metallic = g_sphere.metallicValue;
    if (g_sphere.metallicIndex != kInvalidTextureIndex)
    {
        metallic = SampleScalarMap(g_sphere.metallicIndex, TG_CHANNEL_SLOT_METALLIC, uv,
                                   MapLod(g_sphere.metallicIndex, deltaX, deltaY));
    }

    float ambientOcclusion = g_sphere.aoValue;
    if (g_sphere.aoIndex != kInvalidTextureIndex)
    {
        ambientOcclusion = SampleScalarMap(g_sphere.aoIndex, TG_CHANNEL_SLOT_AO, uv,
                                           MapLod(g_sphere.aoIndex, deltaX, deltaY));
    }

    // --- 法線 --------------------------------------------------------------
    // 接空間は緯度経度の貼り方から作る。u が増える向きが接線、v が増える向き
    // （北極から南へ）が従法線。**このアプリの接空間は DirectX 規約**なので、
    // 緑 = +V（下向き）として読む。OpenGL 規約のマップは緑を反転する。
    float3 normal = normalGeometric;
    if (g_sphere.normalIndex != kInvalidTextureIndex)
    {
        float3 sampled = SampleMap(g_sphere.normalIndex, uv,
                                   MapLod(g_sphere.normalIndex, deltaX, deltaY))
                                 .rgb *
                             2.0f - 1.0f;
        if (g_sphere.flipNormalGreen != 0u)
        {
            sampled.y = -sampled.y;
        }

        // 極では接線が縮退するので、そのときは幾何法線のまま使う。
        const float horizontal = length(normalGeometric.xz);
        if (plane || horizontal > 1e-3f)
        {
            const float3 tangent = plane ? float3(1,0,0) : normalize(float3(-normalGeometric.z, 0.0f, normalGeometric.x));
            // v は北極（+Y）から南へ増えるので、従法線は下向き
            // （赤道・経度 0 では T=(0,0,1)、N=(1,0,0)、B=N×T=(0,-1,0)）。
            const float3 bitangent = plane ? float3(0,0,1) : cross(normalGeometric, tangent);
            normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                               normalGeometric * sampled.z);
        }
    }

    // --- 陰影（ビューポートと同じ式）---------------------------------------
    if (g_sphere.layerMaterial.count > 0) {
        const LayerMaterialSample material = EvaluateLayerMaterial(g_sphere.layerMaterial, uv, uv, metersScale / g_sphere.size, float2(1,1), float2(1,0), float2(0,1));
        baseColor = material.color; roughness = material.surface.x; metallic = material.surface.y; ambientOcclusion = material.surface.z;
        if (plane || length(normalGeometric.xz) > 1e-3f) {
            const float3 t = plane ? float3(1,0,0) : normalize(float3(-normalGeometric.z, 0, normalGeometric.x));
            const float3 b = plane ? float3(0,0,1) : cross(normalGeometric, t);
            normal = normalize(t * material.normal.x + b * material.normal.y + normalGeometric * material.normal.z);
        }
    }

    if (displaced && g_sphere.layerMaterial.count == 0) {
        const float stepSize = max(g_sphere.lengthMeters / g_sphere.size, 0.001f) * 2 / g_sphere.lengthMeters;
        const float3 t = plane ? float3(1,0,0) : normalize(float3(-normalGeometric.z, 0, normalGeometric.x) + float3(1e-7f,0,0));
        const float3 b = plane ? float3(0,0,1) : cross(normalGeometric, t);
        const float2 gradient = float2(HeightAt(position + t * stepSize) - HeightAt(position - t * stepSize),
            HeightAt(position + b * stepSize) - HeightAt(position - b * stepSize)) *
            g_sphere.displacementMeters / (stepSize * g_sphere.lengthMeters);
        const float3 detail = float3(dot(normal,t), dot(normal,b), dot(normal,normalGeometric));
        const float3 combined = ReorientNormal(normalize(float3(-gradient,1)), detail);
        normal = normalize(t * combined.x + b * combined.y + normalGeometric * combined.z);
    }
    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_sphere.lightDirection);
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_sphere.lightColor, g_sphere.lightIlluminance,
                                            diffuseColor, f0, clampedRoughness);

    if (g_sphere.irradianceIndex != kInvalidTextureIndex)
    {
        // MeshPbr と同じ分割和近似。nDotV は 1 を超えると NaN になるので clamp で守る。
        const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

        TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_sphere.irradianceIndex];
        TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_sphere.prefilteredIndex];
        Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_sphere.brdfLutIndex];

        const float3 irradiance =
            irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;
        const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, clampedRoughness);
        const float3 diffuseIbl = (1.0f - fresnel) * diffuseColor * irradiance;

        const float3 reflectionDirection = reflect(-viewDirection, normal);
        const float mipLevel =
            clampedRoughness * float(max(g_sphere.prefilteredMipCount, 1u) - 1u);
        const float3 prefiltered =
            prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;
        const float2 environmentBrdf =
            brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, clampedRoughness), 0.0f);
        const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

        radiance += (diffuseIbl + specularIbl) * g_sphere.iblIntensity * ambientOcclusion;
    }

    const float3 color = lerp(background, radiance, coverage) * g_sphere.exposure;
    output[dispatchThreadId.xy] =
        float4(LinearToSrgb(ApplyTonemap(color, g_sphere.tonemapMode)), 1.0f);
}

// 4 層分を横に並べたマスク画像。合成と同じ GPU 評価を使う。
[numthreads(8, 8, 1)]
void CsMasks(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_sphere.size * 4 || id.y >= g_sphere.size) return;
    const uint slot = id.x / g_sphere.size;
    // マスク画像は平面と同じ見え方にする（一辺 lengthMeters の範囲を 1 枚に収める）。
    const float2 uv = (float2(id.x % g_sphere.size, id.y) + 0.5f) / g_sphere.size * g_sphere.lengthMeters;
    const LayerMaterialSample sample = EvaluateLayerMaterialBase(g_sphere.layerMaterial, uv, uv,
        g_sphere.lengthMeters / g_sphere.size, float2(1,0), float2(0,1));
    const float value = slot < g_sphere.layerMaterial.count ? sample.coverage[slot] : 0;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_sphere.outputIndex];
    output[id.xy] = float4(value, value, value, 1);
}
