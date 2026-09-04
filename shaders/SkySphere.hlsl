// 天球プレビューの窓に出す、回せる球。
//
// **面を反転した球（天球メッシュ）を外から見た絵**を描く。手前の面は抜けていて、
// 向こう側の内壁に環境が貼られている――スカイドームを外から覗いた状態にあたる。
// メッシュは持たず、レイと単位球の交点を解析的に解く（無限に細かい球と同じ見え方）。
//
// **遠近を付ける。** 平行投影の円板だと、どの向きも同じ縮尺で並んで
// 平たい魚眼に見える。カメラを置くと中央が大きく、縁ほど詰まって球らしくなる。
//
// 一覧のサムネイル（SkyThumbnail.hlsl）と違うのは 4 つ。
//   - **適用中の環境キューブをそのまま引く**（HDRI を読み直さない）。毎フレーム描ける。
//   - 向き（ヨー / ピッチ）を回せる。
//   - 寄れる（カメラが球へ近づく）。
//   - 露出とトーンマップはビューポートに合わせる（サムネイルは固定値）。
//
// **球の外はアルファ 0 で抜く。** 置いた先の背景色に重ねてもらう（サムネイルと同じ）。

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
    // カメラから球の中心までの距離（球の半径は 1）。小さいほど寄る。
    float distance;
    float tanHalfFov;

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

    // 画素の中心からレイを飛ばす。カメラは +Z 側に置き、原点（球の中心）を見る。
    // y は画素座標が下向きなので反転する。
    const float2 ndc =
        ((float2(dispatchThreadId.xy) + 0.5f) / float(g_sphere.size)) * 2.0f - 1.0f;
    const float3 origin = float3(0.0f, 0.0f, g_sphere.distance);
    const float3 rayDirection =
        normalize(float3(ndc.x * g_sphere.tanHalfFov, -ndc.y * g_sphere.tanHalfFov, -1.0f));

    // 単位球との交差。**遠いほうの交点**が、向こう側の内壁にあたる
    // （面を反転した球なので、手前の面は抜けて見えない）。
    const float b = dot(origin, rayDirection);
    const float c = dot(origin, origin) - 1.0f;
    const float discriminant = b * b - c;

    // 輪郭は解析的にぼかす。球の中心からレイまでの最短距離が 1 で輪郭。
    // 画素の角幅 × 距離が、球の表面での画素の大きさ。
    const float distanceToAxis = sqrt(max(dot(origin, origin) - b * b, 0.0f));
    const float pixelWidth =
        (2.0f * g_sphere.tanHalfFov / float(g_sphere.size)) * g_sphere.distance;
    const float coverage = 1.0f - smoothstep(1.0f - pixelWidth, 1.0f + pixelWidth, distanceToAxis);

    if (coverage <= 0.0f || g_sphere.environmentIndex == kInvalidTextureIndex)
    {
        output[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 内壁の位置がそのまま、そこに貼られている環境の向き（球の半径は 1）。
    // 輪郭のぼかしに使う画素は球を外れているので、判別式は 0 で止めて縁の点を使う。
    const float far = -b + sqrt(max(discriminant, 0.0f));
    const float3 direction =
        RotateDirection(normalize(origin + rayDirection * far), g_sphere.rotation);

    TextureCube<float4> environment = ResourceDescriptorHeap[g_sphere.environmentIndex];
    const float3 radiance =
        environment.SampleLevel(g_samplerLinearClamp, direction, 0.0f).rgb * g_sphere.intensity;

    output[dispatchThreadId.xy] = float4(
        LinearToSrgb(ApplyTonemap(radiance * g_sphere.exposure, g_sphere.tonemapMode)), coverage);
}
