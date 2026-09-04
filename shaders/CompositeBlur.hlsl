// ハイトをぼかす加工パス（Heightmap Blur ノード）。
//
// 近傍を読むので、Height を UAV で書き換えるレイヤーの合成パスとは同じ
// ディスパッチにできない。**分離型ガウス**の水平パスと垂直パスに分ける。
// タイル分割時は、1 パスを全タイル終えてから次のパスへ進むこと
// （そうしないとまだ書かれていない隣のタイルを読んで継ぎ目が出る）。
//
// 2 次元ガウスを 1 次元 × 2 回に分解できるので、計算量は O(n^2 * R) で済む。
// 参考: terrain-editor の Heightmap Blur（CPU 実装。同じ分離型ガウス）。

#include "CompositeCommon.hlsli"

struct BlurConstants
{
    uint sourceIndex;  // 入力（Texture2D<float>）
    uint outputIndex;  // 出力（ぼかし: RWTexture2D<float> / 法線: RWTexture2D<float2>）
    uint axis;         // 0 = 水平 / 1 = 垂直
    float radiusTexels;

    uint4 tile;        // x, y, width, height（出力全体の中での矩形）
    uint2 resolution;  // 出力全体の解像度
    float strength;    // 垂直パスで元の高さと混ぜる量
    float heightPerSize;  // 法線パスの実寸比（標高差 / 一辺）
};

ConstantBuffer<BlurConstants> g_blur : register(b1);

// 分離型ガウスの 1 パス。重みはシェーダ内で作り、合計で正規化する
// （CPU 側でカーネル配列を組んで渡す必要がない）。
//   w(x) = exp(-0.5 * (x / sigma)^2)、sigma = 半径 * 0.5
[numthreads(8, 8, 1)]
void CsBlur(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_blur.tile.z || dispatchThreadId.y >= g_blur.tile.w)
    {
        return;
    }

    const int2 texel = int2(g_blur.tile.xy + dispatchThreadId.xy);

    Texture2D<float> source = ResourceDescriptorHeap[g_blur.sourceIndex];
    RWTexture2D<float> output = ResourceDescriptorHeap[g_blur.outputIndex];

    const int2 limit = int2(g_blur.resolution) - 1;
    const int2 step = (g_blur.axis == 0u) ? int2(1, 0) : int2(0, 1);
    const int kernelRadius = clamp(int(ceil(g_blur.radiusTexels)), 1, 128);
    const float sigma = max(g_blur.radiusTexels * 0.5f, 0.5f);

    float sum = source.Load(int3(texel, 0));
    float weightSum = 1.0f;
    for (int offset = 1; offset <= kernelRadius; ++offset)
    {
        const float x = float(offset) / sigma;
        const float weight = exp(-0.5f * x * x);
        // 端は clamp-to-edge。境界のアーティファクトを最小にする。
        const int2 low = clamp(texel - step * offset, int2(0, 0), limit);
        const int2 high = clamp(texel + step * offset, int2(0, 0), limit);
        sum += (source.Load(int3(low, 0)) + source.Load(int3(high, 0))) * weight;
        weightSum += weight * 2.0f;
    }

    const float blurred = sum / weightSum;

    // 元の高さと混ぜるのは垂直パスだけ。水平パスは中間結果なので混ぜない
    // （両方で混ぜると強さが 2 回掛かって効きが変わる）。
    if (g_blur.axis == 0u)
    {
        output[uint2(texel)] = blurred;
    }
    else
    {
        output[uint2(texel)] = lerp(output[uint2(texel)], blurred, g_blur.strength);
    }
}

// ぼかした後の Height から法線を作り直す。
//
// 合成の法線はレイヤーが自分のハイトソースから作るので、Height だけを
// ぼかすと**形と陰影が食い違う**。ここで実寸の勾配から作り直して合わせる。
// 勾配のスケールの考え方は CompositeLayer.hlsl の ComputeLayerNormal と同じ
// （こちらは出力そのものを読むので UV スケールは掛からない）。
[numthreads(8, 8, 1)]
void CsNormalFromHeight(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_blur.tile.z || dispatchThreadId.y >= g_blur.tile.w)
    {
        return;
    }

    const int2 texel = int2(g_blur.tile.xy + dispatchThreadId.xy);

    Texture2D<float> height = ResourceDescriptorHeap[g_blur.sourceIndex];
    RWTexture2D<float2> normalTarget = ResourceDescriptorHeap[g_blur.outputIndex];

    const int2 limit = int2(g_blur.resolution) - 1;
    const float hx0 = height.Load(int3(clamp(texel - int2(1, 0), int2(0, 0), limit), 0));
    const float hx1 = height.Load(int3(clamp(texel + int2(1, 0), int2(0, 0), limit), 0));
    const float hy0 = height.Load(int3(clamp(texel - int2(0, 1), int2(0, 0), limit), 0));
    const float hy1 = height.Load(int3(clamp(texel + int2(0, 1), int2(0, 0), limit), 0));

    // UV 単位の勾配（テクセル差 × 解像度）。合成解像度に依らない値になる。
    const float2 resolution = float2(g_blur.resolution);
    const float dx = (hx1 - hx0) * 0.5f * resolution.x;
    const float dy = (hy1 - hy0) * 0.5f * resolution.y;

    // 実寸の勾配へ。tan(傾き) がそのまま法線の xy になる。
    const float scale = g_blur.heightPerSize;
    const float3 normal = normalize(float3(-dx * scale, -dy * scale, 1.0f));
    normalTarget[uint2(texel)] = EncodeTangentNormal(normal);
}

// 合成の Height を小さなグリッドへ落とす（CPU へ読み戻すため）。
//
// ビューポートでパスを地形に沿って編集するのに、CPU 側でも地形の高さが要る
// （クリック位置の地形への投影、点の表示位置）。合成解像度をそのまま読み戻すと
// 重いので、セルの平均（DownsampleHeight）で 512² 程度へ落としてから写す。
// 定数は BlurConstants を流用する（sourceIndex: Height の SRV、outputIndex: 出力 UAV、
// resolution: 出力の一辺）。
[numthreads(8, 8, 1)]
void CsDownsample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    const uint resolution = g_blur.resolution.x;
    if (cell.x >= resolution || cell.y >= resolution)
    {
        return;
    }
    Texture2D<float> source = ResourceDescriptorHeap[g_blur.sourceIndex];
    RWTexture2D<float> output = ResourceDescriptorHeap[g_blur.outputIndex];
    output[cell] = DownsampleHeight(source, cell, resolution);
}
