// ハイトの Levels。入力の範囲（自動なら今の地形の最低〜最高）を、出力の範囲へ写し直す。
// 侵食で削られて縮んだ高さの範囲を、元の全幅へ戻すのに使う。
// 値はどれもハイト 0〜1 の比（m は C++ 側で標高差で割ってある）。
#include "CompositeCommon.hlsli"

struct LevelsConstants {
    uint4 indices; // Height UAV、Mask SRV、範囲 UAV（R32_UINT 2×1）、解像度
    float4 range;  // 入力の最低・最高、出力の最低・最高
    float4 shape;  // ガンマ、自動（1）か手動（0）か
};
ConstantBuffer<LevelsConstants> g_levels : register(b1);

// 自動の範囲は 2 パス（クリア → 集計）で 1 枚へためる。非負の float はビット列の大小が
// uint と同じ順序なので、そのまま InterlockedMin / Max に掛けられる（Mask Height の全範囲と同じ手）。
[numthreads(1, 1, 1)]
void CsRangeClear(uint3 id : SV_DispatchThreadID) {
    RWTexture2D<uint> range = ResourceDescriptorHeap[g_levels.indices.z];
    range[uint2(0, 0)] = 0xFFFFFFFFu;
    range[uint2(1, 0)] = 0u;
}

[numthreads(8, 8, 1)]
void CsRangeReduce(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_levels.indices.w)) return;
    RWTexture2D<float> height = ResourceDescriptorHeap[g_levels.indices.x];
    RWTexture2D<uint> range = ResourceDescriptorHeap[g_levels.indices.z];
    const uint bits = asuint(max(height[id.xy], 0.0f));
    uint previous;
    InterlockedMin(range[uint2(0, 0)], bits, previous);
    InterlockedMax(range[uint2(1, 0)], bits, previous);
}

[numthreads(8, 8, 1)]
void CsLevels(uint3 id : SV_DispatchThreadID) {
    const uint n = g_levels.indices.w;
    if (any(id.xy >= n)) return;
    RWTexture2D<float> height = ResourceDescriptorHeap[g_levels.indices.x];
    float low = g_levels.range.x;
    float high = g_levels.range.y;
    if (g_levels.shape.y > 0.5f) {
        RWTexture2D<uint> range = ResourceDescriptorHeap[g_levels.indices.z];
        low = asfloat(range[uint2(0, 0)]);
        high = asfloat(range[uint2(1, 0)]);
    }
    const float original = height[id.xy];
    // 入力の範囲の外は端へ寄せる（最低より低い所は出力の最低になる）。幅は下限で止める。
    float t = saturate((original - low) / max(high - low, 1e-6f));
    t = pow(t, max(g_levels.shape.x, 1e-3f));
    const float result = lerp(g_levels.range.z, g_levels.range.w, t);
    float mask = 1.0f;
    if (g_levels.indices.y != kInvalidTextureIndex) {
        Texture2D<float> m = ResourceDescriptorHeap[g_levels.indices.y];
        mask = saturate(m.SampleLevel(g_samplerLinearClamp, (id.xy + 0.5f) / n, 0));
    }
    height[id.xy] = saturate(lerp(original, result, mask));
}
