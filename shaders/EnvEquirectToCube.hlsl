// equirectangular マップをキューブマップへ変換する。

#include "EnvCommon.hlsli"

struct EquirectToCubeConstants
{
    uint sourceIndex;  // equirect の SRV
    uint outputIndex;  // キューブの UAV（Texture2DArray として書く）
    uint faceSize;
    // ファイルの値を cd/m^2 へ直す較正倍率。
    //
    // **HDRI ファイルは絶対輝度で較正されていない。** 画像内の比は本物だが、
    // 基準は入っていないので、ここで人が与える。手続き的な空は
    // すでに cd/m^2 で書き込んでいるため 1.0。
    float luminanceScale;
};

ConstantBuffer<EquirectToCubeConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.faceSize || dispatchThreadId.y >= g_constants.faceSize)
    {
        return;
    }

    Texture2D<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2DArray<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const uint face = dispatchThreadId.z;
    const float3 direction = CubeFaceDirection(face, CubeFaceUv(dispatchThreadId.xy,
                                                               g_constants.faceSize));

    const float2 uv = DirectionToEquirectUv(direction);
    const float3 radiance = source.SampleLevel(g_samplerEquirect, uv, 0.0f).rgb;

    // 出力は R16G16B16A16_FLOAT。較正後の太陽ピークが half の上限（65504）を
    // 超えると Inf になり、irradiance の畳み込み全体へ NaN が伝播して拡散 IBL が
    // 黒く落ちる。太陽光はディレクショナルライトで別に扱う設計なので削って構わない。
    const float3 calibrated = min(radiance * g_constants.luminanceScale, 60000.0f);
    output[uint3(dispatchThreadId.xy, face)] = float4(calibrated, 1.0f);
}
