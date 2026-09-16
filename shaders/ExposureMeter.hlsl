// 自動露出の測光。シーンカラー（線形 HDR、cd/m^2 相当）の輝度ヒストグラムを作り、
// 暗い側と明るい側の一部を捨てた範囲の平均対数輝度から EV100 を求める。
// 1. CsClear     ヒストグラムを 0 にする。
// 2. CsHistogram 16x16 のタイルごとに groupshared へ集計し、バッファへ加算する。
// 3. CsResolve   1 グループで百分位を切り出し、結果バッファへ EV100 と平均対数輝度を書く。

#define TG_EXPOSURE_BINS 256

struct MeterConstants
{
    uint sourceIndex;
    uint histogramIndex;
    uint resultIndex;
    uint width;
    uint height;
    float minLog2;        // ビン 0 の対数輝度（log2 cd/m^2）。
    float rangeLog2;      // 全ビンが覆う対数幅。
    float lowFraction;    // 捨てる暗い側の割合（0〜1）。
    float highFraction;   // 捨てる明るい側の割合（0〜1）。
    float calibration;    // EV100 = log2(L * 100 / calibration)。標準は 12.5。
};

ConstantBuffer<MeterConstants> g_constants : register(b0);

groupshared uint g_tile[TG_EXPOSURE_BINS];

[numthreads(TG_EXPOSURE_BINS, 1, 1)]
void CsClear(uint3 id : SV_DispatchThreadID)
{
    RWStructuredBuffer<uint> histogram = ResourceDescriptorHeap[g_constants.histogramIndex];
    histogram[id.x] = 0u;
}

[numthreads(16, 16, 1)]
void CsHistogram(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    g_tile[index] = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (id.x < g_constants.width && id.y < g_constants.height)
    {
        Texture2D<float4> source = ResourceDescriptorHeap[g_constants.sourceIndex];
        const float3 color = source[id.xy].rgb;
        const float luminance = max(dot(color, float3(0.2126f, 0.7152f, 0.0722f)), 1e-8f);
        const float t = saturate((log2(luminance) - g_constants.minLog2) / g_constants.rangeLog2);
        const uint bin = min((uint)(t * (TG_EXPOSURE_BINS - 1) + 0.5f), TG_EXPOSURE_BINS - 1);
        InterlockedAdd(g_tile[bin], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    RWStructuredBuffer<uint> histogram = ResourceDescriptorHeap[g_constants.histogramIndex];
    if (g_tile[index] != 0u) InterlockedAdd(histogram[index], g_tile[index]);
}

groupshared uint g_counts[TG_EXPOSURE_BINS];
groupshared uint g_prefix[TG_EXPOSURE_BINS];

[numthreads(TG_EXPOSURE_BINS, 1, 1)]
void CsResolve(uint index : SV_GroupIndex)
{
    RWStructuredBuffer<uint> histogram = ResourceDescriptorHeap[g_constants.histogramIndex];
    g_counts[index] = histogram[index];
    GroupMemoryBarrierWithGroupSync();
    // 逐次の累積和。256 要素なので 1 スレッドで十分速い。
    if (index == 0u)
    {
        uint running = 0u;
        [loop] for (uint i = 0u; i < TG_EXPOSURE_BINS; ++i)
        {
            running += g_counts[i];
            g_prefix[i] = running;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index != 0u) return;

    const uint total = g_prefix[TG_EXPOSURE_BINS - 1];
    RWStructuredBuffer<float> result = ResourceDescriptorHeap[g_constants.resultIndex];
    if (total == 0u)
    {
        result[0] = 0.0f; result[1] = 0.0f; result[2] = 0.0f; result[3] = 0.0f;
        return;
    }
    // 累積の下位 lowFraction と上位 highFraction を捨て、残りの画素で対数輝度の平均を取る。
    const float lowCount = g_constants.lowFraction * total;
    const float highCount = (1.0f - g_constants.highFraction) * total;
    float weightSum = 0.0f, logSum = 0.0f;
    [loop] for (uint i = 0u; i < TG_EXPOSURE_BINS; ++i)
    {
        const float binStart = i == 0u ? 0.0f : (float)g_prefix[i - 1];
        const float binEnd = (float)g_prefix[i];
        const float weight = max(min(binEnd, highCount) - max(binStart, lowCount), 0.0f);
        const float logLuminance = g_constants.minLog2 + (i / (float)(TG_EXPOSURE_BINS - 1)) * g_constants.rangeLog2;
        weightSum += weight;
        logSum += weight * logLuminance;
    }
    const float meanLog2 = weightSum > 0.0f ? logSum / weightSum : g_constants.minLog2;
    const float luminance = exp2(meanLog2);
    result[0] = log2(luminance * 100.0f / g_constants.calibration);
    result[1] = meanLog2;
    result[2] = 1.0f;
    result[3] = 0.0f;
}
