// マテリアル 1 つを、回せる球で見るためのプレビュー。
//
// メッシュは使わず、単位球とレイを解析的に交差させる。**照らし方はビューポートと同じ**
// （適用中の天球の IBL + 太陽 + 露出 + トーンマップ）。素材が本番の環境でどう見えるかを
// そのまま確かめるためで、一覧のサムネイル（MaterialThumbnail.hlsl）とは目的が違う
// （あちらは見比べるための固定 2 灯で、正面から見た円板）。
//
// 背景は環境キューブのぼかしたミップ。ビューポートの背景（Skybox）と同じ絵を、
// 同じ露出とトーンマップで出す。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
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
    float uvScale;           // 球 1 周に並べるタイル数
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
    float2 pad0;
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
    const float coverage =
        1.0f - smoothstep(1.0f - pixelWidth, 1.0f + pixelWidth,
                          SilhouetteDistance(origin, direction));
    if (coverage <= 0.0f)
    {
        output[dispatchThreadId.xy] =
            float4(LinearToSrgb(ApplyTonemap(background * g_sphere.exposure,
                                             g_sphere.tonemapMode)),
                   1.0f);
        return;
    }

    // --- 球の上の点 --------------------------------------------------------
    const float3 normalGeometric = SphereNormal(origin, direction);
    const float3 position = normalGeometric;  // 半径 1 なので法線と同じ
    const float3 viewDirection = normalize(origin - position);

    // マップは緯度経度で貼る（環境マップと同じ並び）。uvScale で並べる数を決める。
    const float2 uv = DirectionToEquirectUv(normalGeometric) * g_sphere.uvScale;
    // 隣の画素の UV。ミップを選ぶためだけに使う。
    const float2 uvX = DirectionToEquirectUv(SphereNormal(
                           origin, RayDirection(pixel + float2(1.0f, 0.0f), forward, right, up))) *
                       g_sphere.uvScale;
    const float2 uvY = DirectionToEquirectUv(SphereNormal(
                           origin, RayDirection(pixel + float2(0.0f, 1.0f), forward, right, up))) *
                       g_sphere.uvScale;
    const float2 deltaX = WrapDelta(uvX - uv);
    const float2 deltaY = WrapDelta(uvY - uv);

    float3 baseColor = g_sphere.baseColorTint;
    if (g_sphere.baseColorIndex != kInvalidTextureIndex)
    {
        baseColor *= SampleMap(g_sphere.baseColorIndex, uv,
                               MapLod(g_sphere.baseColorIndex, deltaX, deltaY))
                         .rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_sphere.colorAdjust.x, g_sphere.colorAdjust.y);

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
        if (horizontal > 1e-3f)
        {
            const float3 tangent = normalize(float3(-normalGeometric.z, 0.0f, normalGeometric.x));
            // v は北極（+Y）から南へ増えるので、従法線は下向き
            // （赤道・経度 0 では T=(0,0,1)、N=(1,0,0)、B=N×T=(0,-1,0)）。
            const float3 bitangent = cross(normalGeometric, tangent);
            normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                               normalGeometric * sampled.z);
        }
    }

    // --- 陰影（ビューポートと同じ式）---------------------------------------
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
