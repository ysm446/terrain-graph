// 天球一覧に出すサムネイルを描く。
//
// **裏返した球**として描く。マテリアルのサムネイルが球の外側を見ているのに対し、
// こちらは球の内側に環境が貼られていて、その中から見上げている絵になる。
// 円板の中心が正面（奥）、縁が真横で、地平線は円の中央を横切る。
//
// **円の外はアルファ 0 で抜く。背景色を焼き込まない。**
// 理由は MaterialThumbnail.hlsl と同じ（置いた先の背景色に重ねてもらう）。

#include "EnvCommon.hlsli"
#include "Tonemap.hlsli"

struct SkyThumbnailConstants
{
    uint outputIndex;
    uint size;
    uint equirectIndex;    // kInvalidTextureIndex なら手続き的な空を評価する
    float luminanceScale;  // HDRI の較正倍率（生の値 → cd/m^2）

    float3 zenithColor;
    float intensity;

    float3 horizonColor;
    float pad0;

    float3 groundColor;
    float pad1;
};

ConstantBuffer<SkyThumbnailConstants> g_thumbnail : register(b0);

static const uint kInvalidTextureIndex = 0xFFFFFFFFu;

// サムネイルの露出。**天球ごとに変えない固定値**にしてある。
// 自動で合わせると、輝度を下げた天球も明るく写り、設定の効きが見えなくなる。
// 基準（12000 cd/m^2 = 晴天の空）が中間より少し明るく出るように置いた。
static const float kThumbnailExposure = 0.8f / 12000.0f;

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
    const float radius = length(disc);

    // 余白と輪郭のぼかし幅は MaterialThumbnail と揃える。一覧で球の大きさが
    // 揃っていないと、マテリアルと天球が別の枠のものに見える。
    const float sphereRadius = 0.92f;
    const float aa = 1.5f / float(g_thumbnail.size);

    if (radius > sphereRadius + aa)
    {
        output[dispatchThreadId.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // 球の**内側**を見る方向。中心は正面（-Z）の内壁、縁は真横を向く。
    const float2 spherePoint = disc / sphereRadius;
    const float depth = sqrt(saturate(1.0f - dot(spherePoint, spherePoint)));
    const float3 direction = normalize(float3(spherePoint, -depth));

    float3 radiance;
    if (g_thumbnail.equirectIndex == kInvalidTextureIndex)
    {
        radiance = EvaluateProceduralSky(direction, g_thumbnail.zenithColor,
                                         g_thumbnail.horizonColor, g_thumbnail.groundColor,
                                         g_thumbnail.intensity);
    }
    else
    {
        Texture2D<float4> equirect = ResourceDescriptorHeap[g_thumbnail.equirectIndex];
        // 縁では隣り合う画素の方向が大きく開くが、equirect 側を小さく（256x128）
        // 落としてから渡しているので、ここでミップを引く必要はない。
        radiance = equirect.SampleLevel(g_samplerLinearWrap, DirectionToEquirectUv(direction),
                                        0.0f).rgb *
                   g_thumbnail.luminanceScale;
    }

    const float coverage = 1.0f - smoothstep(sphereRadius - aa, sphereRadius + aa, radius);

    output[dispatchThreadId.xy] = float4(
        LinearToSrgb(ApplyTonemap(radiance * kThumbnailExposure, kTonemapAces)), coverage);
}
