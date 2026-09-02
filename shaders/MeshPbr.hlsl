// マテリアルプレビューのメッシュ描画。
// 出力はトーンマップ前の線形放射輝度で、露出は後段の TonemapPass で掛ける。
//
// 2 枚目のレンダーターゲットへマテリアル UV を書き出す。ペイントのブラシパスが
// 「画面のこの画素はマテリアルのどこか」を引くために使う。CPU へ読み戻さずに
// 済ませるため、ID バッファではなく UV をそのまま持たせている。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "EnvCommon.hlsli"

struct MeshConstants
{
    float4x4 viewProjection;
    float4x4 model;
    float4x4 normalMatrix;

    float3 cameraPosition;
    float pad0;

    float3 lightDirection;   // サーフェスから光源へ向かう方向
    float lightIlluminance;  // lux 相当

    float3 lightColor;
    float pad1;

    float3 baseColor;
    float roughness;

    float metallic;
    float iblIntensity;
    uint prefilteredMipCount;
    float pad2;

    uint irradianceIndex;    // irradiance キューブの SRV
    uint prefilteredIndex;   // プリフィルタ済みキューブの SRV
    uint brdfLutIndex;       // 環境 BRDF の LUT
    uint useMaterialTextures;  // 0 なら UI の単色パラメータを使う

    // 合成結果のチャンネル（bindless）
    uint materialBaseColorIndex;
    uint materialNormalIndex;
    uint materialSurfaceIndex;
    uint materialHeightIndex;

    // ビューポートに何を出すか（0 = シェーディング結果）。TG_VIEW_* と一致させる。
    uint debugView;
    // ハイトを形状に反映する量。0 なら押し出さない。
    float displacementScale;
    float pad3;
    float pad4;

    float4x4 lightViewProjection;

    uint shadowIndex;  // 0xFFFFFFFF なら影を落とさない
    float shadowTexelSize;
    float shadowBias;
    float pad5;

    // テセレーションの分割量を画面上の辺の長さから決めるために使う。
    // **シャドウパスでも本描画と同じ値を渡す。** 分割が違うと形がずれ、
    // 自分の影が自分に落ちて縞（シャドウアクネ）になる。
    float4x4 tessellationViewProjection;
    float2 viewportSize;
    float tessellationMaxFactor;
    float pad6;

    // マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引く。
    // maskPreviewLow / High は、マスク 0 / 1 に対応するベースカラー。
    uint maskPreviewHatch;
    float maskPreviewLow;
    float maskPreviewHigh;
    float pad7;
};

// 「ハイト（ローカル）」で周りの平均を取る半径（合成テクセル）と、
// 引いた差を 0〜1 へ伸ばす倍率。素材の凹凸が見える強さとして選んである。
static const float kLocalHeightRadiusTexels = 6.0f;
static const float kLocalHeightGain = 16.0f;

// ビューポートの表示モード。C++ 側の renderer::DebugView と一致させること。
#define TG_VIEW_SHADED          0
#define TG_VIEW_BASECOLOR       1
#define TG_VIEW_NORMAL_TANGENT  2
#define TG_VIEW_NORMAL_WORLD    3
#define TG_VIEW_ROUGHNESS       4
#define TG_VIEW_METALLIC        5
#define TG_VIEW_AO              6
#define TG_VIEW_HEIGHT          7
#define TG_VIEW_HEIGHT_LOCAL    8
#define TG_VIEW_WIREFRAME       9

ConstantBuffer<MeshConstants> g_mesh : register(b1);

// --- 合成結果のサンプリング ------------------------------------------------
// 平面 + UV スケール 1（タイルしない 1 枚絵のプレビュー）ではクランプで読む。
// wrap だと UV 端のバイリニア補間が反対側の端と混ざり、地形の縁が
// 反対側の高さへ引っ張られる。球はシーム（経度の 0/1）の連続性に wrap が
// 必要で、UV スケール > 1 は明示的なタイリングなので wrap のまま。
// サンプラは三項演算子で選べない（unique global resource の制約）ので分岐で書く。

// 合成結果は**タイルしない 1 枚絵**（平面 1 枚に等倍で貼る）なので、常にクランプで読む。
// wrap だと UV 端のバイリニア補間が反対側の端と混ざり、地形の縁が
// 反対側の高さへ引っ張られる。
float4 SampleMaterialColor(Texture2D<float4> map, float2 uv)
{
    return map.Sample(g_samplerAnisoClamp, uv);
}

float2 SampleMaterialNormal(Texture2D<float2> map, float2 uv)
{
    return map.Sample(g_samplerAnisoClamp, uv);
}

float SampleMaterialScalar(Texture2D<float> map, float2 uv)
{
    return map.Sample(g_samplerAnisoClamp, uv);
}

// 頂点 / ドメインシェーダ用（微分が無いので SampleLevel）。
float SampleMaterialScalarLevel(Texture2D<float> map, float2 uv)
{
    return map.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

struct VsInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 tangent  : TANGENT;
    float2 uv       : TEXCOORD0;
};

struct VsOutput
{
    float4 clipPosition  : SV_Position;
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
};

// ライトから見た深度と比べて、この画素が影の中かを返す（1 = 当たっている）。
//
// 深度は普通の Texture2D として読む（比較サンプラは使わない）。
// 3x3 のポイントサンプルで平均を取り、境界のジャギーを和らげる。
float SampleShadow(float3 worldPosition, float nDotL, uint shadowIndex, float texelSize,
                   float bias, float4x4 lightViewProjection)
{
    if (shadowIndex == 0xFFFFFFFFu)
    {
        return 1.0f;
    }

    const float4 lightClip = mul(lightViewProjection, float4(worldPosition, 1.0f));
    if (lightClip.w <= 0.0f)
    {
        return 1.0f;
    }
    const float3 ndc = lightClip.xyz / lightClip.w;
    const float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    // 範囲の外は影を落とさない（シャドウマップが覆っていない）。
    if (any(uv < 0.0f) || any(uv > 1.0f) || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    // 斜めに当たっているほど自己遮蔽しやすいので、下駄を増やす。
    const float slopeBias = bias * (1.0f + 3.0f * (1.0f - saturate(nDotL)));

    Texture2D<float> shadowMap = ResourceDescriptorHeap[shadowIndex];
    float visibility = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y) * texelSize;
            const float depth = shadowMap.SampleLevel(g_samplerPointClamp, uv + offset, 0.0f);
            visibility += (ndc.z - slopeBias <= depth) ? 1.0f : 0.0f;
        }
    }
    return visibility / 9.0f;
}

// --- ディスプレイスメント -------------------------------------------------
// 合成した Height を読み、ワールド空間の法線方向へ押し引きする。
// **VsMain と DsMain の両方がこの関数を通る。** 別々の式を書くと、
// モデル行列を入れたときにテセレーションの ON / OFF で形が変わってしまう。
// 頂点 / ドメインシェーダには微分が無いので SampleLevel を使う。
float3 ApplyDisplacement(float3 worldPosition, float3 worldNormal, float2 uv)
{
    if (g_mesh.useMaterialTextures == 0u || g_mesh.displacementScale == 0.0f)
    {
        return worldPosition;
    }
    Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
    const float height = SampleMaterialScalarLevel(heightMap, uv);
    // 高さの中央（0.5）を基準にする。全体が膨らまないようにするため。
    return worldPosition + worldNormal * ((height - 0.5f) * g_mesh.displacementScale);
}

VsOutput VsMain(VsInput input)
{
    VsOutput output;

    const float3 worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    float3 worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    worldPosition = ApplyDisplacement(worldPosition, normalize(worldNormal), input.uv);

    output.worldPosition = worldPosition;
    output.clipPosition = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    output.worldNormal = worldNormal;
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;

    return output;
}

// --- テセレーション -------------------------------------------------------
//
// 分割量は**画面上の辺の長さ**から決める。細かいメッシュではそのまま 1 になり、
// 近づいて 1 辺が伸びたときだけ細かく割る。ディスプレイスメントは
// ドメインシェーダで掛ける（分割後の点で高さを引くため）。

// 1 辺をおよそ何ピクセルに保つか。小さいほど細かく割る。
static const float kTessellationTargetPixels = 10.0f;

struct HsControlPoint
{
    float3 worldPosition : WORLDPOSITION;
    float3 worldNormal   : NORMAL;
    float3 worldTangent  : TANGENT;
    float tangentSign    : TANGENTSIGN;
    float2 uv            : TEXCOORD0;
};

struct HsPatchConstants
{
    float edges[3]  : SV_TessFactor;
    float inside    : SV_InsideTessFactor;
};

// 投影も変位もせず、ワールド空間の制御点を出すだけ。
HsControlPoint VsControl(VsInput input)
{
    HsControlPoint output;
    output.worldPosition = mul(g_mesh.model, float4(input.position, 1.0f)).xyz;
    output.worldNormal = mul((float3x3)g_mesh.normalMatrix, input.normal);
    output.worldTangent = mul((float3x3)g_mesh.model, input.tangent.xyz);
    output.tangentSign = input.tangent.w;
    output.uv = input.uv;
    return output;
}

// ワールド空間の 2 点が画面上で何ピクセル離れるか。
float ScreenEdgeFactor(float3 a, float3 b)
{
    const float4 clipA = mul(g_mesh.tessellationViewProjection, float4(a, 1.0f));
    const float4 clipB = mul(g_mesh.tessellationViewProjection, float4(b, 1.0f));
    // カメラの後ろに回った辺は判断できないので、最大まで割る。
    if (clipA.w <= 0.0f || clipB.w <= 0.0f)
    {
        return g_mesh.tessellationMaxFactor;
    }

    const float2 screenA = (clipA.xy / clipA.w) * 0.5f * g_mesh.viewportSize;
    const float2 screenB = (clipB.xy / clipB.w) * 0.5f * g_mesh.viewportSize;
    const float pixels = length(screenA - screenB);
    return clamp(pixels / kTessellationTargetPixels, 1.0f, g_mesh.tessellationMaxFactor);
}

HsPatchConstants HsConstant(InputPatch<HsControlPoint, 3> patch)
{
    HsPatchConstants output;
    // SV_TessFactor[i] は「制御点 i の向かい側の辺」に対応する。
    output.edges[0] = ScreenEdgeFactor(patch[1].worldPosition, patch[2].worldPosition);
    output.edges[1] = ScreenEdgeFactor(patch[2].worldPosition, patch[0].worldPosition);
    output.edges[2] = ScreenEdgeFactor(patch[0].worldPosition, patch[1].worldPosition);
    output.inside = (output.edges[0] + output.edges[1] + output.edges[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("HsConstant")]
HsControlPoint HsMain(InputPatch<HsControlPoint, 3> patch, uint id : SV_OutputControlPointID)
{
    return patch[id];
}

[domain("tri")]
VsOutput DsMain(HsPatchConstants patchConstants, float3 barycentric : SV_DomainLocation,
                const OutputPatch<HsControlPoint, 3> patch)
{
    VsOutput output;

    float3 worldPosition = patch[0].worldPosition * barycentric.x +
                           patch[1].worldPosition * barycentric.y +
                           patch[2].worldPosition * barycentric.z;
    const float3 worldNormal = normalize(patch[0].worldNormal * barycentric.x +
                                         patch[1].worldNormal * barycentric.y +
                                         patch[2].worldNormal * barycentric.z);
    const float3 worldTangent = patch[0].worldTangent * barycentric.x +
                                patch[1].worldTangent * barycentric.y +
                                patch[2].worldTangent * barycentric.z;
    const float2 uv = patch[0].uv * barycentric.x + patch[1].uv * barycentric.y +
                      patch[2].uv * barycentric.z;

    // 分割後の点で高さを引いて押し出す。式は VsMain と共通の ApplyDisplacement。
    worldPosition = ApplyDisplacement(worldPosition, worldNormal, uv);

    output.worldPosition = worldPosition;
    output.clipPosition = mul(g_mesh.viewProjection, float4(worldPosition, 1.0f));
    output.worldNormal = worldNormal;
    output.worldTangent = worldTangent;
    output.tangentSign = patch[0].tangentSign;
    output.uv = uv;
    return output;
}

struct PsOutput
{
    float4 color : SV_Target0;
    // xy: マテリアル UV（タイル 1 枚ぶんに畳んだもの）、z: メッシュに当たったか
    float4 materialUv : SV_Target1;
};

PsOutput PsMain(VsOutput input)
{
    const float3 geometricNormal = normalize(input.worldNormal);
    const float3 viewDirection = normalize(g_mesh.cameraPosition - input.worldPosition);

    float3 baseColor = g_mesh.baseColor;
    float roughnessValue = g_mesh.roughness;
    float metallicValue = g_mesh.metallic;
    float ambientOcclusion = 1.0f;
    float3 normal = geometricNormal;

    if (g_mesh.useMaterialTextures != 0u)
    {
        Texture2D<float4> baseColorMap = ResourceDescriptorHeap[g_mesh.materialBaseColorIndex];
        Texture2D<float2> normalMap    = ResourceDescriptorHeap[g_mesh.materialNormalIndex];
        Texture2D<float4> surfaceMap   = ResourceDescriptorHeap[g_mesh.materialSurfaceIndex];

        const float2 uv = input.uv;

        baseColor = SampleMaterialColor(baseColorMap, uv).rgb;

        const float3 surface = SampleMaterialColor(surfaceMap, uv).rgb;
        roughnessValue = surface.r;
        metallicValue = surface.g;
        ambientOcclusion = surface.b;

        // タンジェント空間法線をワールド空間へ移す。
        const float3 tangentNormal = DecodeTangentNormal(SampleMaterialNormal(normalMap, uv));
        const float3 tangent =
            normalize(input.worldTangent - geometricNormal * dot(geometricNormal, input.worldTangent));
        const float3 bitangent = cross(geometricNormal, tangent) * input.tangentSign;
        normal = normalize(tangent * tangentNormal.x + bitangent * tangentNormal.y +
                           geometricNormal * tangentNormal.z);
    }

    // --- マスクのプレビューで、飽和した所へ斜線を引く ----------------------
    //
    // マスクのプレビューは「0 の色」と「1 の色」の間を塗るだけなので、
    // ベースカラーから元のマスクへ戻せる。**0 か 1 に張り付いている所**へ
    // 1px 幅・4px 周期の斜線を中間の灰色で重ね、飽和していることを見せる。
    // 濃淡が付いている所と、上限に当たって潰れた所は、絵では見分けが付かない。
    //
    // 画素の座標は **x と y を別々に切り捨ててから足す**。float のまま足して
    // 丸めると桁落ちで縞の位相が揺れ、太いバンドに見える（terrain-editor で踏んだ）。
    if (g_mesh.maskPreviewHatch != 0u)
    {
        const float low = g_mesh.maskPreviewLow;
        const float high = g_mesh.maskPreviewHigh;
        const float mask = saturate((baseColor.r - low) / max(high - low, 1e-4f));
        if (mask >= 0.99f || mask <= 0.01f)
        {
            const int2 pixel = int2(int(input.clipPosition.x), int(input.clipPosition.y));
            if (((pixel.x + pixel.y) & 3) == 3)
            {
                const float stripe = lerp(low, high, 0.5f);
                baseColor = float3(stripe, stripe, stripe);
            }
        }
    }

    // --- デバッグ表示 ------------------------------------------------------
    // チャンネルの中身をそのまま出す。露出もトーンマップも掛けない
    // （後段の TonemapPass が debugView を見て素通しする）。
    if (g_mesh.debugView != TG_VIEW_SHADED)
    {
        float3 debugColor = float3(0.0f, 0.0f, 0.0f);
        if (g_mesh.debugView == TG_VIEW_BASECOLOR)
        {
            // ベースカラーはリニアで持っているので、見た目を合わせて sRGB で出す。
            debugColor = LinearToSrgb(saturate(baseColor));
        }
        else if (g_mesh.debugView == TG_VIEW_NORMAL_TANGENT)
        {
            // 法線マップそのものの見え方（接空間）。
            float3 tangentNormal = float3(0.0f, 0.0f, 1.0f);
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float2> normalMap = ResourceDescriptorHeap[g_mesh.materialNormalIndex];
                tangentNormal = DecodeTangentNormal(
                    SampleMaterialNormal(normalMap, input.uv));
            }
            debugColor = tangentNormal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == TG_VIEW_NORMAL_WORLD)
        {
            // 陰影に実際に使う向き。法線マップを当てたあとのワールド空間法線。
            debugColor = normal * 0.5f + 0.5f;
        }
        else if (g_mesh.debugView == TG_VIEW_ROUGHNESS)
        {
            debugColor = roughnessValue.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_METALLIC)
        {
            debugColor = metallicValue.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_AO)
        {
            debugColor = ambientOcclusion.xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_WIREFRAME)
        {
            // 線だけを見る表示。塗りではないので単色で描く。
            debugColor = float3(0.66f, 0.72f, 0.78f);
        }
        else if (g_mesh.debugView == TG_VIEW_HEIGHT)
        {
            float height = 0.0f;
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                height = SampleMaterialScalar(heightMap, input.uv);
            }
            debugColor = saturate(height).xxx;
        }
        else if (g_mesh.debugView == TG_VIEW_HEIGHT_LOCAL)
        {
            // **その場の起伏だけ**を見る。地形の大きな高さ（標高差 600m の傾き）を
            // 周りの平均として引き、残りを 0.5 中心へ伸ばす。
            // 素材のハイトマップをそのまま貼ったような見た目になる。
            float local = 0.5f;
            if (g_mesh.useMaterialTextures != 0u)
            {
                Texture2D<float> heightMap = ResourceDescriptorHeap[g_mesh.materialHeightIndex];
                const float center = SampleMaterialScalar(heightMap, input.uv);

                // 周りの平均。半径は合成テクセル基準で固定する
                // （解像度を変えても「どのくらい大きな形を引くか」が変わらない）。
                float2 size = float2(1.0f, 1.0f);
                heightMap.GetDimensions(size.x, size.y);
                const float2 texel = 1.0f / max(size, float2(1.0f, 1.0f));
                const float radius = kLocalHeightRadiusTexels;
                float sum = 0.0f;
                [unroll]
                for (int i = 0; i < 8; ++i)
                {
                    const float angle = (float(i) / 8.0f) * 6.28318530718f;
                    const float2 offset = float2(cos(angle), sin(angle)) * radius * texel;
                    sum += SampleMaterialScalar(heightMap, input.uv + offset);
                }
                local = 0.5f + (center - sum / 8.0f) * kLocalHeightGain;
            }
            debugColor = saturate(local).xxx;
        }

        PsOutput debugOutput;
        debugOutput.color = float4(debugColor, 1.0f);
        debugOutput.materialUv = float4(frac(input.uv), 1.0f, 0.0f);
        return debugOutput;
    }

    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallicValue, diffuseColor, f0);

    const float roughness = clamp(roughnessValue, kMinPerceptualRoughness, 1.0f);

    const float3 lightDirection = normalize(g_mesh.lightDirection);
    // 影は直接光にだけ掛ける。環境光（IBL）は別に扱う。
    const float shadow = SampleShadow(input.worldPosition, dot(normal, lightDirection),
                                      g_mesh.shadowIndex, g_mesh.shadowTexelSize,
                                      g_mesh.shadowBias, g_mesh.lightViewProjection);

    float3 radiance = ShadeDirectionalLight(normal, viewDirection, lightDirection,
                                            g_mesh.lightColor, g_mesh.lightIlluminance,
                                            diffuseColor, f0, roughness) *
                      shadow;

    // --- IBL（分割和近似） -------------------------------------------------
    // saturate + 加算だと最大 1.00001 になり、FresnelSchlickRoughness の
    // pow(1 - nDotV, 5) が負の底で NaN になる。clamp で上限も守る。
    const float nDotV = clamp(dot(normal, viewDirection), 1e-4f, 1.0f);

    TextureCube<float4> irradianceMap = ResourceDescriptorHeap[g_mesh.irradianceIndex];
    TextureCube<float4> prefilteredMap = ResourceDescriptorHeap[g_mesh.prefilteredIndex];
    Texture2D<float2> brdfLut = ResourceDescriptorHeap[g_mesh.brdfLutIndex];

    // irradiance マップには E / pi（平均放射輝度）が入っているので、
    // diffuseColor を掛けるだけでよい。
    const float3 irradiance = irradianceMap.SampleLevel(g_samplerLinearClamp, normal, 0.0f).rgb;

    const float3 fresnel = FresnelSchlickRoughness(f0, nDotV, roughness);
    const float3 kD = 1.0f - fresnel;
    const float3 diffuseIbl = kD * diffuseColor * irradiance;

    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float mipLevel = roughness * float(max(g_mesh.prefilteredMipCount, 1u) - 1u);
    const float3 prefiltered =
        prefilteredMap.SampleLevel(g_samplerLinearClamp, reflectionDirection, mipLevel).rgb;

    const float2 environmentBrdf =
        brdfLut.SampleLevel(g_samplerLinearClamp, float2(nDotV, roughness), 0.0f);
    const float3 specularIbl = prefiltered * (f0 * environmentBrdf.x + environmentBrdf.y);

    radiance += (diffuseIbl + specularIbl) * g_mesh.iblIntensity * ambientOcclusion;

    PsOutput output;
    // シーンカラーは R16G16B16A16_FLOAT。half の上限（65504）を超えると Inf になり、
    // トーンマップを経て NaN → ハイライト中心の黒点になる。上限手前でクランプする。
    output.color = float4(min(radiance, 60000.0f), 1.0f);
    // ペイントマスクはタイル 1 枚ぶんのテクスチャなので、UV も畳んで書き出す。
    output.materialUv = float4(frac(input.uv), 1.0f, 0.0f);
    return output;
}
