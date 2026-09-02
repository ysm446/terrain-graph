// 合成の中間結果（下地の Height）からマスクを作る。
//
// レイヤーの合成パスは Height を UAV として書き換えるため、近傍を参照する
// この計算を同じディスパッチで行うと競合する。読み取り専用の別パスに分ける。
// タイル分割時も、レイヤー単位で全タイルのこのパスを終えてから
// 合成パスへ進むこと（そうしないとタイル境界に継ぎ目が出る）。

#include "CompositeCommon.hlsli"

#define TG_MASK_HEIGHT    3
#define TG_MASK_SLOPE     4
#define TG_MASK_CURVATURE 5
#define TG_MASK_CAVITY    6

// 勾配とラプラシアンを見た目の妥当な範囲へ丸める係数。
// 生の値はノイズ周波数がそのまま出て極端に大きくなる。
static const float kDerivedGradientScale = 0.02f;
static const float kDerivedCurvatureScale = 0.006f;
static const float kCavityScale = 0.05f;
// 窪みリングの半径と正規化の基準解像度。リングを UV 単位で固定することで、
// 合成解像度を変えてもマスクの効きが変わらないようにする（1024 で従来と一致）。
static const float kCavityReferenceResolution = 1024.0f;

struct MaskConstants
{
    uint heightIndex;  // 下地 Height の SRV
    uint outputIndex;  // マスクの UAV
    uint source;
    float derivedScale;

    uint4 tile;        // x, y, width, height
    uint2 resolution;
    float2 pad0;
};

ConstantBuffer<MaskConstants> g_mask : register(b1);

float SampleHeight(Texture2D<float> height, float2 uv)
{
    return height.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_mask.tile.z || dispatchThreadId.y >= g_mask.tile.w)
    {
        return;
    }

    const uint2 texel = g_mask.tile.xy + dispatchThreadId.xy;

    Texture2D<float> height = ResourceDescriptorHeap[g_mask.heightIndex];
    RWTexture2D<float> output = ResourceDescriptorHeap[g_mask.outputIndex];

    const float2 texelSize = 1.0f / float2(g_mask.resolution);
    const float2 uv = (float2(texel) + 0.5f) * texelSize;

    const float center = SampleHeight(height, uv);
    const float scale = max(g_mask.derivedScale, 0.0f);

    float result = 0.0f;

    if (g_mask.source == TG_MASK_HEIGHT)
    {
        result = saturate(center * scale);
    }
    else if (g_mask.source == TG_MASK_SLOPE)
    {
        const float hx0 = SampleHeight(height, uv - float2(texelSize.x, 0.0f));
        const float hx1 = SampleHeight(height, uv + float2(texelSize.x, 0.0f));
        const float hy0 = SampleHeight(height, uv - float2(0.0f, texelSize.y));
        const float hy1 = SampleHeight(height, uv + float2(0.0f, texelSize.y));

        // UV 単位の微分にしておくと、合成解像度を変えても見た目が変わらない。
        const float dx = (hx1 - hx0) * 0.5f / texelSize.x;
        const float dy = (hy1 - hy0) * 0.5f / texelSize.y;

        result = saturate(length(float2(dx, dy)) * kDerivedGradientScale * scale);
    }
    else if (g_mask.source == TG_MASK_CURVATURE)
    {
        const float hx0 = SampleHeight(height, uv - float2(texelSize.x, 0.0f));
        const float hx1 = SampleHeight(height, uv + float2(texelSize.x, 0.0f));
        const float hy0 = SampleHeight(height, uv - float2(0.0f, texelSize.y));
        const float hy1 = SampleHeight(height, uv + float2(0.0f, texelSize.y));

        const float laplacian = (hx0 + hx1 + hy0 + hy1 - 4.0f * center) /
                                (texelSize.x * texelSize.y);

        // 0.5 を平坦とし、凸で大きく、凹で小さくなるようにする。
        result = saturate(0.5f + laplacian * kDerivedCurvatureScale * scale * 0.5f);
    }
    else if (g_mask.source == TG_MASK_CAVITY)
    {
        // 周囲の平均より低いほど窪んでいるとみなす簡易 AO。
        // 半径を変えた 2 つのリングを見て、中くらいと大きめの窪みを拾う。
        // 半径が小さすぎるとノイズの最高周波数の穴ばかり拾ってしまう。
        float sum = 0.0f;
        int count = 0;
        for (int ring = 1; ring <= 2; ++ring)
        {
            // 半径は UV 単位で固定する。テクセル単位にすると解像度で効きが変わる。
            const float radiusUv = float(ring) * 8.0f / kCavityReferenceResolution;
            for (int i = 0; i < 8; ++i)
            {
                const float angle = (float(i) / 8.0f) * 2.0f * kPi;
                const float2 offset = float2(cos(angle), sin(angle)) * radiusUv;
                sum += SampleHeight(height, uv + offset);
                ++count;
            }
        }

        const float average = sum / float(count);
        result = saturate(0.5f + (average - center) * kCavityReferenceResolution *
                                     kCavityScale * scale);
    }

    output[texel] = result;
}
