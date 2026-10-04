// 湖の水移動、粗密の水面平坦化、水位の延長。内部の高さと水深は m。
#include "CompositeCommon.hlsli"

struct LakeConstants {
    uint4 textures; // 状態入力 UAV、状態出力 UAV、最終出力 UAV、未使用
    uint4 inputs;   // Height SRV、Height UAV、供給 Mask SRV、出力 Mask UAV
    uint4 grid;     // 作業解像度、元解像度、低解像度、近傍の間隔
    uint4 mode;     // 境界流出、出力成分、水位延長の軸、範囲を湖面にする
    float4 scale;   // Height の m、供給する水深 m、セル幅 m、範囲を湖面にするときの最大水深 m
};
ConstantBuffer<LakeConstants> g_lake : register(b1);

int2 LakeCell(int2 p, uint n) { return clamp(p, 0, int(n) - 1); }

float4 LakeSample(float2 p, uint n) {
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    p = clamp(p, 0.0f, float(n - 1));
    int2 a = int2(p), b = min(a + 1, int(n - 1));
    return lerp(lerp(source[a], source[int2(b.x, a.y)], frac(p.x)),
                lerp(source[int2(a.x, b.y)], source[b], frac(p.x)), frac(p.y));
}

[numthreads(8, 8, 1)]
void CsInit(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.x)) return;
    Texture2D<float> height = ResourceDescriptorHeap[g_lake.inputs.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    float2 uv = (float2(id.xy) + 0.5f) / g_lake.grid.x;
    int2 p = LakeCell(int2(uv * g_lake.grid.y), g_lake.grid.y);
    float water = g_lake.scale.y;
    if (g_lake.inputs.z != kInvalidTextureIndex) {
        Texture2D<float> mask = ResourceDescriptorHeap[g_lake.inputs.z];
        water *= saturate(mask.SampleLevel(g_samplerLinearClamp, uv, 0));
    }
    // 元の height と供給水深を別フィールドに置いて輸送を開始する。
    target[id.xy] = float4(height.Load(int3(p, 0)) * g_lake.scale.x, water, 0, 0);
}

[numthreads(8, 8, 1)]
void CsTransport(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.x)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    int2 p = int2(id.xy);
    float4 state = source[p];
    float diff = 0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            int2 q = LakeCell(p + int2(x, y), g_lake.grid.x);
            float4 neighbor = source[q];
            float dh = neighbor.x - state.x;
            if (g_lake.mode.x != 0 && any(q != p + int2(x, y))) dh -= neighbor.y;
            if (abs(x) == abs(y) && x != 0) dh /= 1.4142f;
            diff += min(dh, neighbor.y) * 0.17f;
        }
    }
    diff = max(diff, -state.y);
    target[p] = float4(state.x + diff, max(0.0f, state.y + diff), 0, 0);
}

[numthreads(8, 8, 1)]
void CsUpsample(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    Texture2D<float> height = ResourceDescriptorHeap[g_lake.inputs.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    float2 uv = (float2(id.xy) + 0.5f) / g_lake.grid.y;
    float water = max(0.0f, LakeSample(uv * g_lake.grid.z - 0.5f, g_lake.grid.z).y);
    float ground = height.Load(int3(id.xy, 0)) * g_lake.scale.x;
    target[id.xy] = float4(ground + water, water, 0, 0);
}

[numthreads(8, 8, 1)]
void CsFlatten(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.x)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    int2 p = int2(id.xy);
    float4 state = source[p];
    float hmin = state.x;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            hmin = min(hmin, source[LakeCell(p + int2(x, y) * int(g_lake.grid.w), g_lake.grid.x)].x);
    float surface = max(hmin, state.x - state.y);
    target[p] = float4(surface, max(0.0f, state.y - (state.x - surface)), 0, 0);
}

[numthreads(8, 8, 1)]
void CsLevelInit(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    float4 state = source[id.xy];
    // 乾いた場所へ水位を延長する。未到達の距離は十分大きな値にする。
    state.z = state.y > 0.001f ? 0.0f : 1e20f;
    state.w = state.x;
    target[id.xy] = state;
}

[numthreads(8, 8, 1)]
void CsExtendLevel(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    int2 p = int2(id.xy);
    float4 state = source[p];
    if (state.y <= 0.001f) {
        [unroll] for (int sign = -1; sign <= 1; sign += 2) {
            int step = sign * int(g_lake.grid.w);
            int2 offset = g_lake.mode.z == 0 ? int2(step, 0) : int2(0, step);
            float4 neighbor = source[LakeCell(p + offset, g_lake.grid.y)];
            float distance = neighbor.z + g_lake.scale.z * g_lake.grid.w;
            if (distance < state.z) { state.z = distance; state.w = neighbor.w; }
        }
    }
    target[p] = state;
}

[numthreads(8, 8, 1)]
void CsFinish(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.z];
    float4 state = source[id.xy];
    float depth = max(0.0f, state.y - 0.001f);
    target[id.xy] = float4(depth > 0 ? 1.0f : 0.0f, depth, state.w / g_lake.scale.x, 0);
}

[numthreads(8, 8, 1)]
void CsResolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.z];
    RWTexture2D<float> target = ResourceDescriptorHeap[g_lake.inputs.y];
    // 範囲を湖面にするときは、水のある所を水位そのものへ置き換える（湖底を掘っているので、
    // 地面 + 水深 ではなく水面の高さを書く）。
    if (g_lake.mode.w != 0) {
        if (source[id.xy].y > 0) target[id.xy] = saturate(source[id.xy].z);
        return;
    }
    target[id.xy] = saturate(target[id.xy] + source[id.xy].y / g_lake.scale.x);
}

// --- 範囲を湖面にする（湖面が平らな DEM 用） -----------------------------------------
// Water Mask の範囲をそのまま湖にする。水位は範囲の中の地面の平均の高さ。

float LakeAreaMask(float2 uv) {
    if (g_lake.inputs.z == kInvalidTextureIndex) return 0.0f;
    Texture2D<float> mask = ResourceDescriptorHeap[g_lake.inputs.z];
    return saturate(mask.SampleLevel(g_samplerLinearClamp, uv, 0));
}

// 範囲の中の（地面の高さ m, 1）を置く。CsAreaReduce が 1 画素まで畳む。
[numthreads(8, 8, 1)]
void CsAreaSum(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    Texture2D<float> height = ResourceDescriptorHeap[g_lake.inputs.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    float inside = LakeAreaMask((float2(id.xy) + 0.5f) / g_lake.grid.y) > 0.001f ? 1.0f : 0.0f;
    target[id.xy] = float4(height.Load(int3(id.xy, 0)) * g_lake.scale.x * inside, inside, 0, 0);
}

// 2×2 を足して半分の大きさへ（grid.w が書く側の一辺）。
[numthreads(8, 8, 1)]
void CsAreaReduce(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.w)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    int2 p = int2(id.xy) * 2;
    target[id.xy] = source[p] + source[p + int2(1, 0)] + source[p + int2(0, 1)] + source[p + int2(1, 1)];
}

// 水位（合計 / 画素数）から「マスク × 最大水深」だけ下を湖底にし、水面と水深を置く。
// 範囲の中でも水位より高い地面は陸のまま残る。範囲の外には水を張らない。
[numthreads(8, 8, 1)]
void CsAreaApply(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    Texture2D<float> height = ResourceDescriptorHeap[g_lake.inputs.x];
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_lake.textures.y];
    float4 sum = source[int2(0, 0)];
    float ground = height.Load(int3(id.xy, 0)) * g_lake.scale.x;
    float inside = LakeAreaMask((float2(id.xy) + 0.5f) / g_lake.grid.y);
    float water = 0.0f;
    if (sum.y > 0.0f && inside > 0.001f) {
        float level = sum.x / sum.y;
        float bed = min(ground, level - inside * g_lake.scale.w);
        water = max(0.0f, level - bed);
        ground = bed;
    }
    target[id.xy] = float4(ground + water, water, 0, 0);
}

[numthreads(8, 8, 1)]
void CsMask(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_lake.grid.y)) return;
    RWTexture2D<float> target = ResourceDescriptorHeap[g_lake.inputs.w];
    if (g_lake.mode.y >= 3) { target[id.xy] = 0; return; }
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_lake.textures.z];
    target[id.xy] = source[id.xy][g_lake.mode.y];
}
