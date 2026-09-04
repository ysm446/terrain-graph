// マテリアル一覧に出すサムネイルを描く。
//
// メッシュは使わず、正面を向いた球を解析的に解く。マップは円板の UV でそのまま引く。
// 見た目を比べるためのものなので、プレビュー本体と厳密に一致させる必要はない。
//
// **円の外はアルファ 0 で抜く。背景色を焼き込まない。**
// サムネイルはレイヤーパネル（#1E1E1E）とマテリアル一覧（#232323）の両方に出るので、
// 背景を塗ると必ずどちらかで四角い色違いの板に見える。
// 抜いておけば ImGui が置いた先の色に重ねてくれる。

#include "Brdf.hlsli"
#include "CompositeCommon.hlsli"
#include "Tonemap.hlsli"

struct ThumbnailConstants
{
    uint outputIndex;
    uint size;
    uint baseColorIndex;   // sRGB の SRV。kInvalidTextureIndex なら定数
    uint normalIndex;

    uint roughnessIndex;
    uint metallicIndex;
    uint aoIndex;
    uint heightIndex;      // 今は使わない。将来の視差用に枠だけ確保する

    float3 baseColorTint;
    float roughnessValue;

    float metallicValue;
    float aoValue;
    float uvScale;
    // スカラーのマップのチャンネル指定。4bit ずつ TG_CHANNEL_SLOT_* の順。
    uint mapChannels;

    // ベースカラーの調整（ティントを掛けたあとに効く）。合成と同じ値を渡すこと。
    float2 colorAdjust;  // 色相（ラジアン）, 彩度
    float2 pad0;
};

ConstantBuffer<ThumbnailConstants> g_thumbnail : register(b0);


float4 SampleMap(uint index, float2 uv)
{
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    return map.SampleLevel(g_samplerLinearWrap, uv, 0.0f);
}

// スカラーのマップを 1 つ読む。指定されたチャンネルだけを取り出す。
float SampleScalarMap(uint index, uint channelSlot, float2 uv)
{
    return SelectChannel(SampleMap(index, uv),
                         UnpackChannel(g_thumbnail.mapChannels, channelSlot));
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_thumbnail.size || dispatchThreadId.y >= g_thumbnail.size)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_thumbnail.outputIndex];

    // 出力の中心を原点、半径 1 の円に正規化する。y は上向きにする。
    const float2 uvCentered =
        ((float2(dispatchThreadId.xy) + 0.5f) / float(g_thumbnail.size)) * 2.0f - 1.0f;
    const float2 disc = float2(uvCentered.x, -uvCentered.y);
    const float radiusSq = dot(disc, disc);

    // 少し余白を取って球を収める。
    const float sphereRadius = 0.92f;
    const float radius = sqrt(radiusSq);

    // 輪郭のジャギーを消すための幅。disc は size テクセルで [-1, 1] を張るので、
    // 1 テクセル = 2 / size。その 1.5 倍を半値幅にする（合わせて 3 テクセル）。
    const float aa = 1.5f / float(g_thumbnail.size);

    if (radius > sphereRadius + aa)
    {
        output[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float2 spherePoint = disc / sphereRadius;
    const float3 geometricNormal =
        float3(spherePoint, sqrt(saturate(1.0f - dot(spherePoint, spherePoint))));

    // マップは円板の座標でそのまま引く。球へ厳密に貼るのではなく、
    // 「その素材がどう見えるか」を確かめられれば足りる。
    const float2 uv = (spherePoint * 0.5f + 0.5f) * g_thumbnail.uvScale;

    float3 baseColor = g_thumbnail.baseColorTint;
    if (g_thumbnail.baseColorIndex != kInvalidTextureIndex)
    {
        baseColor *= SampleMap(g_thumbnail.baseColorIndex, uv).rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_thumbnail.colorAdjust.x, g_thumbnail.colorAdjust.y);

    float roughness = g_thumbnail.roughnessValue;
    if (g_thumbnail.roughnessIndex != kInvalidTextureIndex)
    {
        roughness = SampleScalarMap(g_thumbnail.roughnessIndex,
                                    TG_CHANNEL_SLOT_ROUGHNESS, uv);
    }

    float metallic = g_thumbnail.metallicValue;
    if (g_thumbnail.metallicIndex != kInvalidTextureIndex)
    {
        metallic = SampleScalarMap(g_thumbnail.metallicIndex, TG_CHANNEL_SLOT_METALLIC, uv);
    }

    float ambientOcclusion = g_thumbnail.aoValue;
    if (g_thumbnail.aoIndex != kInvalidTextureIndex)
    {
        ambientOcclusion = SampleScalarMap(g_thumbnail.aoIndex, TG_CHANNEL_SLOT_AO, uv);
    }

    // 球の接空間は、正面を向いているので x が接線、y が従法線でよい。
    float3 normal = geometricNormal;
    if (g_thumbnail.normalIndex != kInvalidTextureIndex)
    {
        const float3 sampled = SampleMap(g_thumbnail.normalIndex, uv).rgb * 2.0f - 1.0f;
        // 円板の左右端では法線がほぼ ±X になり、X 軸からの直交化が縮退して
        // NaN（輝点・黒点）が出る。BuildOrthonormalBasis と同じく軸を切り替える。
        const float3 axis = (abs(geometricNormal.x) < 0.999f) ? float3(1.0f, 0.0f, 0.0f)
                                                              : float3(0.0f, 1.0f, 0.0f);
        const float3 tangent =
            normalize(axis - geometricNormal * dot(geometricNormal, axis));
        const float3 bitangent = cross(geometricNormal, tangent);
        normal = normalize(tangent * sampled.x + bitangent * sampled.y +
                           geometricNormal * sampled.z);
    }

    float3 diffuseColor;
    float3 f0;
    SplitBaseColor(baseColor, metallic, diffuseColor, f0);

    // 下限はビューポート（MeshPbr）と揃える。ずれていると、同じマテリアルが
    // サムネイルと本描画で違う粗さに見える。
    const float clampedRoughness = clamp(roughness, kMinPerceptualRoughness, 1.0f);
    const float3 viewDirection = float3(0.0f, 0.0f, 1.0f);

    // --- 2 灯で照らす ------------------------------------------------------
    //
    // 1 灯だけだと陰側がのっぺりして、素材の凹凸が半分しか読めない。
    // 素材ライブラリのサムネイルは**見比べるためのもの**なので、
    // 形と粗さが一目で分かるように 2 方向から当てる。
    //
    //   キー  : 左上手前。主役。暖色寄りの白
    //   フィル: 右奥。**カメラより奥に置く**ことで、右の輪郭が光って
    //           背景からシルエットが分離する。寒色寄りにしてキーと差をつける
    //
    // **強さは素材の暗さを見込んで決めてある。** 地面素材はアルベドが 0.1〜0.3 と
    // 暗く、控えめに当てると 2 灯にしても差が模様のノイズに埋もれてしまう。
    // 白い素材でも飽和しないことは確認済み（最も明るい素材で最大 168 / 255）。
    const float3 keyDirection = normalize(float3(-0.45f, 0.55f, 0.70f));
    const float3 fillDirection = normalize(float3(0.70f, 0.35f, -0.62f));

    // 一覧の中で明るさが揃うよう、露出は掛けずに正規化した強さで直接シェーディングする。
    float3 radiance = ShadeDirectionalLight(normal, viewDirection, keyDirection,
                                            float3(1.0f, 0.98f, 0.95f), 4.5f, diffuseColor, f0,
                                            clampedRoughness);
    radiance += ShadeDirectionalLight(normal, viewDirection, fillDirection,
                                      float3(0.62f, 0.74f, 1.0f), 3.0f, diffuseColor, f0,
                                      clampedRoughness);

    // 環境光の代わり。上からの弱い半球光で、影側が真っ黒にならないようにする。
    const float hemisphere = saturate(normal.y * 0.5f + 0.5f);
    radiance += diffuseColor * lerp(0.05f, 0.20f, hemisphere) * ambientOcclusion;

    // 輪郭は「縁を暗く落とす」のではなくアルファで抜く。
    // 落とすと、素材の色によっては濃いグレーの輪郭として見えてしまう。
    // 球らしさは斜めを向いた法線のシェーディングだけで足りる。
    const float coverage = 1.0f - smoothstep(sphereRadius - aa, sphereRadius + aa, radius);

    output[dispatchThreadId.xy] =
        float4(LinearToSrgb(ApplyTonemap(radiance, kTonemapAces)), coverage);
}
