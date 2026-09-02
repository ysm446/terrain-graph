// リニアなテクスチャ（EXR）を一覧に出すための表示用テクスチャを作る。
//
// ImGui はテクスチャの値をそのままバックバッファへ書くので、リニアのまま渡すと
// 極端に暗く見える（0.2 のアルベドが 0.2 のまま出る）。ここで sRGB へ直して
// RGBA8 に焼き、一覧はそれを描く。
//
// 8bit の画像（PNG など）は中身がすでに sRGB なので、この経路は通さない。

#include "Common.hlsli"

struct PreviewConstants
{
    uint sourceIndex;
    uint outputIndex;
    uint size;      // 出力の一辺
    float sourceMip;  // 縮小率に見合ったミップ。エイリアスを避けるため明示する
};

ConstantBuffer<PreviewConstants> g_constants : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_constants.size || dispatchThreadId.y >= g_constants.size)
    {
        return;
    }

    Texture2D<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_constants.outputIndex];

    const float2 uv = (float2(dispatchThreadId.xy) + 0.5f) / float(g_constants.size);
    const float3 linearColor = source.SampleLevel(g_samplerLinearClamp, uv,
                                                  g_constants.sourceMip).rgb;

    // 1 を超える値は白へ潰す。サムネイルなので露出は合わせない。
    output[dispatchThreadId.xy] = float4(LinearToSrgb(saturate(linearColor)), 1.0f);
}
