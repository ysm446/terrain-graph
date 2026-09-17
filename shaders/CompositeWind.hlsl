// 地形全体の風の場（Wind Field ノード）。
//
// 一様な風を地形にぶつけ、発散のない流れに直す（ポテンシャル流の近似）。
// 地形の高さで固体セルを決め、圧力のヤコビ反復で投影する。稜線での吹き上げ、
// 風下の剥離、谷筋への収束という大まかな流れが出ればよい（乱流は局所側が担う）。
//
// 格子は水平 res × res、鉛直 layers 層の絶対高さ（0 = ハイト 0、上端 = 標高差 + 余白）。
// 速度・圧力・発散は構造化バッファに置く（添字 = (k * res + j) * res + i、
// i は u（+X）、j は v（+Z）、k は上向き）。
//
//   CsInit       : 流体セルに一様な風、固体セルに 0。圧力を 0 に
//   CsDivergence : 速度の発散
//   CsJacobi     : 圧力のヤコビ反復（ping-pong。固体と外側は勾配 0 の境界）
//   CsProject    : 速度から圧力勾配を引き、固体セルを 0 に
//   CsToMask     : 地表直上の風速からマスクを作る（Speed / Spindrift）
//
// 速度の格子は同位置（MAC ではない）。粗い格子の大まかな流れが目的なので、
// 市松模様の誤差は許容する。

#include "CompositeCommon.hlsli"

struct WindConstants
{
    // x: Height SRV、y: 速度 UAV、z: 圧力 A UAV、w: 圧力 B UAV
    uint4 indices0;
    // x: 発散 UAV、y: マスク UAV、z: マスクの一辺、w: 出力（0: Speed、1: Spindrift）
    uint4 indices1;
    // x: 水平の一辺（セル数）、y: 層数、z: ヤコビの向き（0: A→B、1: B→A）、w: 未使用
    uint4 grid;
    // x: 水平セル幅（m）、y: 鉛直セル幅（m）、z: 標高差（m。ハイト 1 の高さ）、w: 未使用
    float4 cell;
    // x: 風向 u 成分、y: 風向 v 成分、z: 風速（m/s）、w: 粉雪のしきい値（m/s）
    float4 wind;
    // x: 粉雪の幅（m/s。しきい値からこれだけ超えたら 1）、y: 風下判定の傾き、zw: 未使用
    float4 spindrift;
};

ConstantBuffer<WindConstants> g_wind : register(b1);

uint CellIndex(uint i, uint j, uint k)
{
    return (k * g_wind.grid.x + j) * g_wind.grid.x + i;
}

// セル (i, j) 直下の地形の高さ（m）。
float TerrainHeight(uint i, uint j)
{
    Texture2D<float> height = ResourceDescriptorHeap[g_wind.indices0.x];
    const float2 uv = (float2(i, j) + 0.5f) / float(g_wind.grid.x);
    return height.SampleLevel(g_samplerLinearClamp, uv, 0.0f) * g_wind.cell.z;
}

float CellCenterY(uint k)
{
    return (float(k) + 0.5f) * g_wind.cell.y;
}

bool IsSolid(uint i, uint j, uint k)
{
    return CellCenterY(k) < TerrainHeight(i, j);
}

bool InGrid(int i, int j, int k)
{
    return i >= 0 && j >= 0 && k >= 0 && i < int(g_wind.grid.x) && j < int(g_wind.grid.x) &&
           k < int(g_wind.grid.y);
}

[numthreads(8, 8, 1)]
void CsInit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_wind.grid.x || id.y >= g_wind.grid.x || id.z >= g_wind.grid.y) return;
    RWStructuredBuffer<float4> velocity = ResourceDescriptorHeap[g_wind.indices0.y];
    RWStructuredBuffer<float> pressureA = ResourceDescriptorHeap[g_wind.indices0.z];
    RWStructuredBuffer<float> pressureB = ResourceDescriptorHeap[g_wind.indices0.w];
    const uint index = CellIndex(id.x, id.y, id.z);
    const bool solid = IsSolid(id.x, id.y, id.z);
    // w に固体フラグを持つ（以降のパスで地形を引き直さない）。
    velocity[index] = solid ? float4(0.0f, 0.0f, 0.0f, 1.0f)
                            : float4(g_wind.wind.x * g_wind.wind.z, 0.0f, g_wind.wind.y * g_wind.wind.z, 0.0f);
    pressureA[index] = 0.0f;
    pressureB[index] = 0.0f;
}

// 近傍の速度。外側は自分の速度（勾配 0）、固体は 0。
float4 NeighborVelocity(RWStructuredBuffer<float4> velocity, int i, int j, int k, float4 self)
{
    if (!InGrid(i, j, k)) return self;
    return velocity[CellIndex(uint(i), uint(j), uint(k))];
}

[numthreads(8, 8, 1)]
void CsDivergence(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_wind.grid.x || id.y >= g_wind.grid.x || id.z >= g_wind.grid.y) return;
    RWStructuredBuffer<float4> velocity = ResourceDescriptorHeap[g_wind.indices0.y];
    RWStructuredBuffer<float> divergence = ResourceDescriptorHeap[g_wind.indices1.x];
    const uint index = CellIndex(id.x, id.y, id.z);
    const float4 self = velocity[index];
    if (self.w > 0.5f)
    {
        divergence[index] = 0.0f;
        return;
    }
    const int i = int(id.x), j = int(id.y), k = int(id.z);
    const float dx = g_wind.cell.x, dy = g_wind.cell.y;
    const float du = (NeighborVelocity(velocity, i + 1, j, k, self).x - NeighborVelocity(velocity, i - 1, j, k, self).x) / (2.0f * dx);
    const float dv = (NeighborVelocity(velocity, i, j + 1, k, self).z - NeighborVelocity(velocity, i, j - 1, k, self).z) / (2.0f * dx);
    const float dw = (NeighborVelocity(velocity, i, j, k + 1, self).y - NeighborVelocity(velocity, i, j, k - 1, self).y) / (2.0f * dy);
    divergence[index] = du + dv + dw;
}

// 近傍の圧力。外側と固体は自分の圧力（ノイマン境界）。
float NeighborPressure(RWStructuredBuffer<float> pressure, RWStructuredBuffer<float4> velocity,
                       int i, int j, int k, float self)
{
    if (!InGrid(i, j, k)) return self;
    const uint index = CellIndex(uint(i), uint(j), uint(k));
    if (velocity[index].w > 0.5f) return self;
    return pressure[index];
}

[numthreads(8, 8, 1)]
void CsJacobi(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_wind.grid.x || id.y >= g_wind.grid.x || id.z >= g_wind.grid.y) return;
    RWStructuredBuffer<float4> velocity = ResourceDescriptorHeap[g_wind.indices0.y];
    RWStructuredBuffer<float> divergence = ResourceDescriptorHeap[g_wind.indices1.x];
    RWStructuredBuffer<float> source = ResourceDescriptorHeap[g_wind.grid.z == 0u ? g_wind.indices0.z : g_wind.indices0.w];
    RWStructuredBuffer<float> target = ResourceDescriptorHeap[g_wind.grid.z == 0u ? g_wind.indices0.w : g_wind.indices0.z];
    const uint index = CellIndex(id.x, id.y, id.z);
    if (velocity[index].w > 0.5f)
    {
        target[index] = 0.0f;
        return;
    }
    const int i = int(id.x), j = int(id.y), k = int(id.z);
    const float self = source[index];
    const float wx = 1.0f / (g_wind.cell.x * g_wind.cell.x);
    const float wy = 1.0f / (g_wind.cell.y * g_wind.cell.y);
    const float sum =
        wx * (NeighborPressure(source, velocity, i + 1, j, k, self) + NeighborPressure(source, velocity, i - 1, j, k, self)) +
        wx * (NeighborPressure(source, velocity, i, j + 1, k, self) + NeighborPressure(source, velocity, i, j - 1, k, self)) +
        wy * (NeighborPressure(source, velocity, i, j, k + 1, self) + NeighborPressure(source, velocity, i, j, k - 1, self));
    target[index] = (sum - divergence[index]) / (4.0f * wx + 2.0f * wy);
}

[numthreads(8, 8, 1)]
void CsProject(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_wind.grid.x || id.y >= g_wind.grid.x || id.z >= g_wind.grid.y) return;
    RWStructuredBuffer<float4> velocity = ResourceDescriptorHeap[g_wind.indices0.y];
    // 反復回数が偶数なら結果は A に戻っている。C++ 側が偶数回に揃える。
    RWStructuredBuffer<float> pressure = ResourceDescriptorHeap[g_wind.indices0.z];
    const uint index = CellIndex(id.x, id.y, id.z);
    float4 v = velocity[index];
    if (v.w > 0.5f)
    {
        return;
    }
    const int i = int(id.x), j = int(id.y), k = int(id.z);
    const float self = pressure[index];
    const float dx = g_wind.cell.x, dy = g_wind.cell.y;
    v.x -= (NeighborPressure(pressure, velocity, i + 1, j, k, self) - NeighborPressure(pressure, velocity, i - 1, j, k, self)) / (2.0f * dx);
    v.z -= (NeighborPressure(pressure, velocity, i, j + 1, k, self) - NeighborPressure(pressure, velocity, i, j - 1, k, self)) / (2.0f * dx);
    v.y -= (NeighborPressure(pressure, velocity, i, j, k + 1, self) - NeighborPressure(pressure, velocity, i, j, k - 1, self)) / (2.0f * dy);
    velocity[index] = v;
}

// 地表直上の風速。地表から半セル上の位置を 8 近傍で補間する（固体セルは重みから外す）。
// 最初の流体層をそのまま読むと層の境目が等高線状の縞になるので、鉛直にも補間する。
float3 SurfaceWind(float2 uv)
{
    RWStructuredBuffer<float4> velocity = ResourceDescriptorHeap[g_wind.indices0.y];
    Texture2D<float> height = ResourceDescriptorHeap[g_wind.indices0.x];
    const float terrain = height.SampleLevel(g_samplerLinearClamp, uv, 0.0f) * g_wind.cell.z;
    const int res = int(g_wind.grid.x);
    const int layers = int(g_wind.grid.y);
    const float3 cellPos = float3(uv.x * float(res) - 0.5f, (terrain + 0.5f * g_wind.cell.y) / g_wind.cell.y - 0.5f,
                                  uv.y * float(res) - 0.5f);
    const int3 base = int3(floor(cellPos));
    const float3 t = cellPos - float3(base);
    float3 sum = 0.0f;
    float weight = 0.0f;
    [unroll]
    for (int corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        const int3 cell = clamp(base + offset, int3(0, 0, 0), int3(res - 1, layers - 1, res - 1));
        const float4 v = velocity[CellIndex(uint(cell.x), uint(cell.z), uint(cell.y))];
        if (v.w > 0.5f) continue;
        const float w = (offset.x != 0 ? t.x : 1.0f - t.x) * (offset.y != 0 ? t.y : 1.0f - t.y) *
                        (offset.z != 0 ? t.z : 1.0f - t.z);
        sum += v.xyz * w;
        weight += w;
    }
    if (weight > 1e-4f)
    {
        return sum / weight;
    }
    // 8 近傍が全部固体（急な崖の中など）なら、その真上の最初の流体層を読む。
    const uint i = uint(clamp(int(uv.x * float(res)), 0, res - 1));
    const uint j = uint(clamp(int(uv.y * float(res)), 0, res - 1));
    const int first = int(floor(terrain / g_wind.cell.y - 0.5f)) + 1;
    const uint k = uint(clamp(first, 0, layers - 1));
    return velocity[CellIndex(i, j, k)].xyz;
}

[numthreads(8, 8, 1)]
void CsToMask(uint3 id : SV_DispatchThreadID)
{
    const uint resolution = g_wind.indices1.z;
    if (id.x >= resolution || id.y >= resolution) return;
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_wind.indices1.y];
    Texture2D<float> height = ResourceDescriptorHeap[g_wind.indices0.x];
    const float2 texel = 1.0f / float(resolution);
    const float2 uv = (float2(id.xy) + 0.5f) * texel;
    const float3 v = SurfaceWind(uv);
    const float speed = length(v);
    const float reference = max(g_wind.wind.z, 1e-3f);

    if (g_wind.indices1.w == 0u)
    {
        // 風速。一様風の 2 倍で 1（稜線での吹き上げが最大 2 倍程度）。
        mask[id.xy] = saturate(speed / (2.0f * reference));
        return;
    }

    // 粉雪。風速がしきい値を超え、かつ風下側（風の向きに地形が下る所）。
    // 勾配は水平セル 1 つぶんの差分から取る（m / m）。
    const float dx = g_wind.cell.x;
    const float2 step = float2(1.0f / float(g_wind.grid.x), 1.0f / float(g_wind.grid.x));
    const float hE = height.SampleLevel(g_samplerLinearClamp, uv + float2(step.x, 0.0f), 0.0f) * g_wind.cell.z;
    const float hW = height.SampleLevel(g_samplerLinearClamp, uv - float2(step.x, 0.0f), 0.0f) * g_wind.cell.z;
    const float hN = height.SampleLevel(g_samplerLinearClamp, uv + float2(0.0f, step.y), 0.0f) * g_wind.cell.z;
    const float hS = height.SampleLevel(g_samplerLinearClamp, uv - float2(0.0f, step.y), 0.0f) * g_wind.cell.z;
    const float2 gradient = float2(hE - hW, hN - hS) / (2.0f * dx);
    const float slopeAlongWind = dot(gradient, g_wind.wind.xy);
    // 風下側は風の向きに高さが下がる（負の勾配）。
    const float lee = saturate(-slopeAlongWind / max(g_wind.spindrift.y, 1e-3f));
    const float strength = saturate((speed - g_wind.wind.w) / max(g_wind.spindrift.x, 1e-3f));
    mask[id.xy] = strength * lee;
}
