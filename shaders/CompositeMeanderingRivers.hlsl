// KTT の接線移流・地形勾配・再サンプリングを GPU のバッチ評価へ適応。
#include "CompositeCommon.hlsli"

cbuffer MeanderingConstants : register(b1) {
    uint4 g_points;  // 読み、書き、初期パス UAV、標本数
    uint4 g_images;  // Height SRV、Height UAV、流域 UAV、結果 UAV
    uint4 g_nearest; // 最近傍の読み / 書き UAV、JFA 間隔、Mask UAV
    uint4 g_grid;    // 解像度、上り勾配除去、seed、Mask 有効
    float4 g_scale;  // 地形幅 m、標高差 m、川幅 m、標本間隔 m
    float4 g_motion; // 移流の強さ、地形影響、平滑化、深さ比率
    float4 g_basin;  // 流域有効、半幅 m、深さ m、岸ノイズ
    float4 g_input[2048]; // UV、高さオフセット m、道のり 0〜1
};

uint2 PointCell(uint i) { return uint2(i & 63, i >> 6); }
float4 Point(uint i) {
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_points.x];
    return source[PointCell(i)];
}
float2 Unit(float2 v) { return v / max(length(v), 1e-8); }
float Random(uint i) {
    i ^= g_grid.z * 747796405u;
    i = (i ^ (i >> 16)) * 2246822519u;
    i = (i ^ (i >> 13)) * 3266489917u;
    return float(i ^ (i >> 16)) / 4294967295.0;
}
float InitialHeight(uint i) {
    Texture2D<float> height = ResourceDescriptorHeap[g_images.x];
    return height.SampleLevel(g_samplerLinearClamp, saturate(g_input[i].xy), 0) + g_input[i].z / g_scale.y;
}

[numthreads(64, 1, 1)]
void CsInit(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= g_points.w) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_points.y];
    RWTexture2D<float4> original = ResourceDescriptorHeap[g_points.z];
    float4 p = float4(saturate(g_input[i].xy), InitialHeight(i), g_input[i].w);
    if (g_grid.y != 0) {
        for (uint j = i; j > 0 && g_input[j].w > 0; ) {
            --j;
            p.z = min(p.z, InitialHeight(j));
        }
    }
    original[PointCell(i)] = p;
    if (p.w > 0 && p.w < 1 && g_motion.x > 0) {
        float2 tangent = Unit(g_input[i + 1].xy - g_input[i - 1].xy);
        p.xy = saturate(p.xy + float2(-tangent.y, tangent.x) *
            (Random(i) - 0.5) * 0.2 * g_scale.w / g_scale.x);
    }
    target[PointCell(i)] = p;
}

float BasinHeight(float2 uv) {
    RWTexture2D<float> source = ResourceDescriptorHeap[g_images.z];
    float2 p = clamp(uv * g_grid.x - 0.5, 0.0, float(g_grid.x - 1));
    int2 a = int2(p), b = min(a + 1, int(g_grid.x - 1));
    return lerp(lerp(source[a], source[int2(b.x, a.y)], frac(p.x)),
        lerp(source[int2(a.x, b.y)], source[b], frac(p.x)), frac(p.y));
}

[numthreads(64, 1, 1)]
void CsAdvect(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= g_points.w) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_points.y];
    float4 p = Point(i);
    if (p.w > 0 && p.w < 1) {
        float2 tangent = Unit(Point(i + 1).xy - Point(i - 1).xy);
        float cell = 1.0 / g_grid.x;
        float2 gradient = float2(BasinHeight(p.xy + float2(cell, 0)) - BasinHeight(p.xy - float2(cell, 0)),
            BasinHeight(p.xy + float2(0, cell)) - BasinHeight(p.xy - float2(0, cell))) *
            g_scale.y / (2 * cell * g_scale.x);
        float2 direction = Unit(tangent - gradient * g_motion.y * g_scale.w);
        float taper = smoothstep(0, 0.08, p.w) * smoothstep(0, 0.08, 1 - p.w);
        p.xy = saturate(p.xy + direction * g_motion.x * taper * g_scale.w * 0.75 / g_scale.x);
    }
    target[PointCell(i)] = p;
}

[numthreads(64, 1, 1)]
void CsSmooth(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= g_points.w) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_points.y];
    float4 p = Point(i);
    if (p.w > 0 && p.w < 1) p.xy = lerp(p.xy, (Point(i - 1).xy + Point(i + 1).xy) * 0.5, g_motion.z);
    target[PointCell(i)] = p;
}

[numthreads(64, 1, 1)]
void CsResample(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= g_points.w) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_points.y];
    float4 p = Point(i);
    if (p.w > 0 && p.w < 1) {
        uint first = i, last = i;
        while (first > 0 && Point(first).w > 0) --first;
        while (last + 1 < g_points.w && Point(last).w < 1) ++last;
        float total = 0;
        for (uint j = first; j < last; ++j) total += distance(Point(j).xy, Point(j + 1).xy);
        float wanted = total * p.w;
        for (uint j = first; j < last; ++j) {
            float4 a = Point(j), b = Point(j + 1);
            float d = distance(a.xy, b.xy);
            if (wanted <= d || j + 1 == last) {
                p.xyz = lerp(a.xyz, b.xyz, saturate(wanted / max(d, 1e-8)));
                break;
            }
            wanted -= d;
        }
    }
    target[PointCell(i)] = p;
}

// 移動後の川筋を流域へ投影してから河床を掘る。
[numthreads(64, 1, 1)]
void CsProject(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i >= g_points.w) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_points.y];
    float4 p = Point(i);
    p.z = BasinHeight(p.xy);
    if (g_grid.y != 0) {
        for (uint j = i; j > 0 && Point(j).w > 0; ) {
            --j;
            p.z = min(p.z, BasinHeight(Point(j).xy));
        }
    }
    target[PointCell(i)] = p;
}

// 線分上の標本を種にする Jump Flood。距離評価は点ではなく線分に対して行う。
[numthreads(8, 8, 1)]
void CsClear(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_grid.x)) return;
    RWTexture2D<uint> target = ResourceDescriptorHeap[g_nearest.y];
    target[id.xy] = 0xffffffffu;
}

[numthreads(64, 1, 1)]
void CsSeed(uint3 id : SV_DispatchThreadID) {
    uint i = id.x;
    if (i + 1 >= g_points.w || Point(i).w >= 1) return;
    RWTexture2D<uint> target = ResourceDescriptorHeap[g_nearest.y];
    float2 a = Point(i).xy, b = Point(i + 1).xy;
    uint steps = max(1u, uint(ceil(length((b - a) * g_grid.x) * 2)));
    for (uint j = 0; j <= steps; ++j) {
        uint2 p = min(uint2(saturate(lerp(a, b, float(j) / steps)) * g_grid.x), g_grid.x - 1);
        InterlockedMin(target[p], i);
    }
}

float SegmentDistance(float2 uv, uint i, out float4 projected) {
    projected = 0;
    if (i >= g_points.w || i + 1 >= g_points.w) return 1e20;
    float4 a = Point(i), b = Point(i + 1);
    float2 ab = b.xy - a.xy;
    float t = saturate(dot(uv - a.xy, ab) / max(dot(ab, ab), 1e-16));
    projected = lerp(a, b, t);
    return distance(uv, projected.xy) * g_scale.x;
}

[numthreads(8, 8, 1)]
void CsJump(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_grid.x)) return;
    RWTexture2D<uint> source = ResourceDescriptorHeap[g_nearest.x];
    RWTexture2D<uint> target = ResourceDescriptorHeap[g_nearest.y];
    float2 uv = (float2(id.xy) + 0.5) / g_grid.x;
    uint best = 0xffffffffu;
    float bestDistance = 1e20;
    for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
        int2 p = int2(id.xy) + int2(x, y) * int(g_nearest.z);
        if (any(p < 0) || any(p >= int(g_grid.x))) continue;
        uint candidate = source[p];
        float4 projected;
        float d = SegmentDistance(uv, candidate, projected);
        if (d < bestDistance || (d == bestDistance && candidate < best)) { best = candidate; bestDistance = d; }
    }
    target[id.xy] = best;
}

[numthreads(8, 8, 1)]
void CsBasin(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_grid.x)) return;
    Texture2D<float> height = ResourceDescriptorHeap[g_images.x];
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[g_nearest.x];
    RWTexture2D<float> target = ResourceDescriptorHeap[g_images.z];
    float h = height.Load(int3(id.xy, 0));
    float4 p;
    float d = SegmentDistance((float2(id.xy) + 0.5) / g_grid.x, nearest[id.xy], p);
    if (g_basin.x > 0 && g_basin.y > 0 && d < g_basin.y) {
        float weight = 1 - smoothstep(0, g_basin.y, d);
        h = lerp(h, p.z - g_basin.z / g_scale.y, weight);
    }
    target[id.xy] = h;
}

[numthreads(8, 8, 1)]
void CsFinish(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_grid.x)) return;
    RWTexture2D<float> basin = ResourceDescriptorHeap[g_images.z];
    RWTexture2D<uint> nearest = ResourceDescriptorHeap[g_nearest.x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_images.w];
    float4 p;
    float d = SegmentDistance((float2(id.xy) + 0.5) / g_grid.x, nearest[id.xy], p);
    float variation = sin(p.w * 79 + Random(0) * 6.283) * 0.65 + sin(p.w * 193 + Random(1) * 6.283) * 0.35;
    float width = g_scale.z * (1 + variation * g_basin.w);
    float radius = max(width * 0.5, g_scale.x / g_grid.x * 0.7071);
    float coverage = 1 - smoothstep(radius * 0.7, radius, d);
    float h = basin[id.xy];
    if (g_motion.w > 0 && coverage > 0) {
        float depth = width * g_motion.w * saturate(1 - d * d / (radius * radius));
        h = lerp(h, min(h, p.z - depth / g_scale.y), coverage);
    }
    output[id.xy] = float4(h, coverage, 0, 0);
}

[numthreads(8, 8, 1)]
void CsResolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_grid.x)) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_images.w];
    RWTexture2D<float> height = ResourceDescriptorHeap[g_images.y];
    height[id.xy] = output[id.xy].x;
}

[numthreads(8, 8, 1)]
void CsMask(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_grid.x)) return;
    RWTexture2D<float> target = ResourceDescriptorHeap[g_nearest.w];
    float mask = 0;
    if (g_grid.w != 0) {
        RWTexture2D<float4> output = ResourceDescriptorHeap[g_images.w];
        mask = output[id.xy].y;
    }
    target[id.xy] = mask;
}
