// ペイントマスクへブラシを積む。
//
// カーソル位置からマテリアルの UV を求める手段として、メッシュ描画が出力した
// UV バッファ（frac(uv * materialUvScale) と被覆フラグ）を使う。CPU へ読み戻さないので
// GPU 同期が要らず、遅延は「1 フレーム前の UV バッファを見る」ぶんだけで済む。
//
// ストロークは前フレームのカーソル位置から今フレームの位置までの線分として受け取る。
// マウスが速く動いても切れないよう、線分上を複数点で標本化して最大の重みを取る。
// 標本化はグループ内の 64 スレッドで行い、groupshared に置いて全スレッドで共有する。

#include "Common.hlsli"

struct BrushConstants
{
    uint uvBufferIndex;   // ビューポートの UV バッファ（RGBA16F, xy=UV, z=被覆）
    uint outputIndex;     // ペイントマスクの UAV（R8_UNORM）
    uint resolution;      // ペイントマスクの解像度（正方）
    uint sampleCount;     // 線分上の標本数（1..64）

    uint viewportWidth;
    uint viewportHeight;
    float radiusPixels;   // ブラシ半径。ビューポートの画面ピクセル単位
    float strength;       // 1 回の適用でマスクに足す量

    float fromX;          // 前フレームのカーソル位置（ビューポートのピクセル座標）
    float fromY;
    float toX;            // 今フレームのカーソル位置
    float toY;

    float falloff;        // 1 で線形、大きいほど中心に集中する
    uint erase;           // 0 で加算、1 で減算
};

ConstantBuffer<BrushConstants> g_brush : register(b0);

// 標本 1 点。xy: マテリアル UV、z: 画面 1 ピクセルあたりの UV 幅、w: 有効なら 1。
groupshared float4 gStrokeSamples[64];

// マスクはメッシュ上でタイリングするため、UV の距離は 0 / 1 の境界をまたぐ。
float WrappedUvDistance(float2 a, float2 b)
{
    float2 d = abs(a - b);
    d = min(d, 1.0f - d);
    return length(d);
}

// 画面 1 ピクセルあたりの UV 幅。ブラシ半径を画面ピクセルで指定するために要る。
// 隣接ピクセルとの差分を取るが、UV の継ぎ目やメッシュの輪郭をまたぐ差分は捨てる。
float EstimateUvPerPixel(Texture2D<float4> uvBuffer, int2 texel, float2 centerUv)
{
    const int2 offsets[4] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1)};

    float result = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        const int2 neighbor = clamp(texel + offsets[i], int2(0, 0),
                                    int2(g_brush.viewportWidth - 1, g_brush.viewportHeight - 1));
        const float4 sampled = uvBuffer[neighbor];
        if (sampled.z <= 0.0f)
        {
            continue;
        }
        const float distance = WrappedUvDistance(centerUv, sampled.xy);
        // タイルの継ぎ目や輪郭では差分が跳ねる。妥当な範囲のものだけ採用する。
        if (distance > 0.05f)
        {
            continue;
        }
        result = max(result, distance);
    }

    if (result <= 1e-7f)
    {
        // 近傍がすべて使えないときは「タイル 1 枚がビューポート幅に収まる」とみなす。
        return 1.0f / max(float(g_brush.viewportWidth), 1.0f);
    }
    return result;
}

float4 ComputeStrokeSample(uint index)
{
    const float t = (g_brush.sampleCount > 1u)
                        ? (float(index) / float(g_brush.sampleCount - 1u))
                        : 1.0f;
    const float2 position = lerp(float2(g_brush.fromX, g_brush.fromY),
                                 float2(g_brush.toX, g_brush.toY), t);

    // ビューポート外の標本は捨てる。clamp で端へ丸めると、ドラッグで
    // ウィンドウ外へ出たときに縁のテクセルへ貼り付いた線が引かれてしまう。
    if (position.x < 0.0f || position.y < 0.0f ||
        position.x >= float(g_brush.viewportWidth) ||
        position.y >= float(g_brush.viewportHeight))
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    const int2 texel = int2(position);

    Texture2D<float4> uvBuffer = ResourceDescriptorHeap[g_brush.uvBufferIndex];
    const float4 center = uvBuffer[texel];
    if (center.z <= 0.0f)
    {
        // メッシュに当たっていない。この標本は使わない。
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    return float4(center.xy, EstimateUvPerPixel(uvBuffer, texel, center.xy), 1.0f);
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex)
{
    // 標本化はグループごとに重複して行うが、参照する UV バッファのテクセル数は
    // 標本数 x 5 と小さく、別パスに分けてまで削るほどの負荷にはならない。
    gStrokeSamples[groupIndex] = (groupIndex < g_brush.sampleCount)
                                     ? ComputeStrokeSample(groupIndex)
                                     : float4(0.0f, 0.0f, 0.0f, 0.0f);
    GroupMemoryBarrierWithGroupSync();

    if (dispatchThreadId.x >= g_brush.resolution || dispatchThreadId.y >= g_brush.resolution)
    {
        return;
    }

    const uint2 texel = dispatchThreadId.xy;
    const float2 uv = (float2(texel) + 0.5f) / float(g_brush.resolution);

    float weight = 0.0f;
    for (uint i = 0; i < g_brush.sampleCount; ++i)
    {
        const float4 stroke = gStrokeSamples[i];
        if (stroke.w <= 0.0f)
        {
            continue;
        }

        const float radiusUv = max(g_brush.radiusPixels * stroke.z, 1e-5f);
        const float normalized = WrappedUvDistance(uv, stroke.xy) / radiusUv;
        if (normalized >= 1.0f)
        {
            continue;
        }

        weight = max(weight, pow(saturate(1.0f - normalized), max(g_brush.falloff, 0.01f)));
    }

    if (weight <= 0.0f)
    {
        return;
    }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_brush.outputIndex];
    const float amount = weight * g_brush.strength;
    const float current = output[texel];
    output[texel] = saturate((g_brush.erase != 0u) ? (current - amount) : (current + amount));
}
