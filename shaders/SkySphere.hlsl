// 天球プレビューの窓に出す、回せる球。
//
// **裏返した球**として描く（一覧のサムネイル SkyThumbnail.hlsl と同じ見え方）。
// 球の内側に環境が貼られていて、その中から見上げている絵になる。
// 円板の中心が正面、縁が真横で、地平線は円の中央を横切る。
//
// サムネイルと違うのは 3 つ。
//   - **適用中の環境キューブをそのまま引く**（HDRI を読み直さない）。毎フレーム描ける。
//   - 向き（ヨー / ピッチ）を回せる。
//   - 露出とトーンマップはビューポートに合わせる（サムネイルは固定値）。
//
// **円の外はアルファ 0 で抜く。** 置いた先の背景色に重ねてもらう（サムネイルと同じ）。

#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

struct SkySphereConstants
{
    uint outputIndex;
    uint size;
    uint environmentIndex;  // 適用中の環境キューブ。kInvalidTextureIndex なら何も描かない
    float intensity;        // 環境光の強さ（天球ごと）

    float exposure;
    uint tonemapMode;
    // 1 で球が区画いっぱい。大きくすると球がはみ出し、真ん中を大きく見られる。
    float zoom;
    float pad0;

    // 向き。xy が ヨーの sin / cos、zw が ピッチの sin / cos。
    // 三角関数は CPU 側で済ませる（画素ごとに計算する意味がない）。
    float4 rotation;
};

ConstantBuffer<SkySphereConstants> g_sphere : register(b0);

static const uint kInvalidTextureIndex = 0xFFFFFFFFu;

// 視線をピッチ → ヨーの順に回す。**ヨーは世界の Y 軸まわり。**
// 逆順にすると、傾けたあとの水平回転が斜めに効いて気持ち悪い動きになる。
float3 RotateDirection(float3 direction, float4 rotation)
{
    const float sinYaw = rotation.x;
    const float cosYaw = rotation.y;
    const float sinPitch = rotation.z;
    const float cosPitch = rotation.w;

    // ピッチ（X 軸まわり）
    float3 result = float3(direction.x, direction.y * cosPitch - direction.z * sinPitch,
                           direction.y * sinPitch + direction.z * cosPitch);
    // ヨー（Y 軸まわり）
    result = float3(result.x * cosYaw + result.z * sinYaw, result.y,
                    -result.x * sinYaw + result.z * cosYaw);
    return result;
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_sphere.size || dispatchThreadId.y >= g_sphere.size)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_sphere.outputIndex];

    // 出力の中心を原点、半径 1 の円に正規化する。y は上向きにする。
    const float2 uvCentered =
        ((float2(dispatchThreadId.xy) + 0.5f) / float(g_sphere.size)) * 2.0f - 1.0f;
    const float2 disc = float2(uvCentered.x, -uvCentered.y);
    const float radius = length(disc);

    // 余白はサムネイルと同じ。寄ると球が枠からはみ出す（円板の半径を広げる）。
    const float sphereRadius = 0.92f * g_sphere.zoom;
    const float aa = 1.5f / float(g_sphere.size);

    if (radius > sphereRadius + aa || g_sphere.environmentIndex == kInvalidTextureIndex)
    {
        output[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 球の**内側**を見る方向。中心は正面（-Z）の内壁、縁は真横を向く。
    const float2 spherePoint = disc / sphereRadius;
    const float depth = sqrt(saturate(1.0f - dot(spherePoint, spherePoint)));
    const float3 direction = RotateDirection(normalize(float3(spherePoint, -depth)),
                                             g_sphere.rotation);

    TextureCube<float4> environment = ResourceDescriptorHeap[g_sphere.environmentIndex];
    const float3 radiance =
        environment.SampleLevel(g_samplerLinearClamp, direction, 0.0f).rgb * g_sphere.intensity;

    const float coverage = 1.0f - smoothstep(sphereRadius - aa, sphereRadius + aa, radius);

    output[dispatchThreadId.xy] = float4(
        LinearToSrgb(ApplyTonemap(radiance * g_sphere.exposure, g_sphere.tonemapMode)), coverage);
}
