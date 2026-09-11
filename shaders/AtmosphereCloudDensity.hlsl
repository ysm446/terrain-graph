// 周期ノイズ 64³ を 2D 配列へ保存。R: 従来の Perlin、G: Perlin-Worley、B: 細部 Worley。

cbuffer CloudVolumeConstants : register(b0)
{
    uint  resolution;   // = 64
    int   seed;
    uint outputIndex;
    float pad1;
};



uint Hash4(int x, int y, int z, int s)
{
    uint h = (uint)x * 0x27d4eb2du;
    h ^= (uint)y * 0x165667b1u;
    h ^= (uint)z * 0x9e3779b9u;
    h ^= (uint)s * 0xc2b2ae35u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

// Random unit-sphere gradient. Two-angle parameterization keeps it cheap and
// avoids the rejection loop / atan trig stacking up across millions of voxels.
float3 Gradient3(int x, int y, int z, int s)
{
    uint h = Hash4(x, y, z, s);
    const float twoPi = 6.28318530717958647692f;
    float phi = ((float)(h & 0xFFFFu) / 65535.0f) * twoPi;
    float cosTheta = ((float)((h >> 16) & 0xFFFFu) / 65535.0f) * 2.0f - 1.0f;
    float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

float Fade(float t)
{
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Periodic Perlin: integer coords are wrapped mod `period` before being
// hashed for gradients. The result is that Perlin3DPeriodic(x, ..., period)
// has period exactly `period` along each axis, which lets us write the 3D
// noise into a tileable texture (the values at uvw = 0 and uvw = 1 in
// normalized texture space match seamlessly).
int WrapInt(int v, int period)
{
    int m = v % period;
    return m < 0 ? m + period : m;
}

float Perlin3DPeriodic(float x, float y, float z, int s, int period)
{
    float fx = floor(x);
    float fy = floor(y);
    float fz = floor(z);
    int xi0 = (int)fx, yi0 = (int)fy, zi0 = (int)fz;
    int x0 = WrapInt(xi0,     period), y0 = WrapInt(yi0,     period), z0 = WrapInt(zi0,     period);
    int x1 = WrapInt(xi0 + 1, period), y1 = WrapInt(yi0 + 1, period), z1 = WrapInt(zi0 + 1, period);
    float dx = x - fx, dy = y - fy, dz = z - fz;

    float3 g000 = Gradient3(x0, y0, z0, s);
    float3 g100 = Gradient3(x1, y0, z0, s);
    float3 g010 = Gradient3(x0, y1, z0, s);
    float3 g110 = Gradient3(x1, y1, z0, s);
    float3 g001 = Gradient3(x0, y0, z1, s);
    float3 g101 = Gradient3(x1, y0, z1, s);
    float3 g011 = Gradient3(x0, y1, z1, s);
    float3 g111 = Gradient3(x1, y1, z1, s);

    float v000 = dot(g000, float3(dx,         dy,         dz));
    float v100 = dot(g100, float3(dx - 1.0f,  dy,         dz));
    float v010 = dot(g010, float3(dx,         dy - 1.0f,  dz));
    float v110 = dot(g110, float3(dx - 1.0f,  dy - 1.0f,  dz));
    float v001 = dot(g001, float3(dx,         dy,         dz - 1.0f));
    float v101 = dot(g101, float3(dx - 1.0f,  dy,         dz - 1.0f));
    float v011 = dot(g011, float3(dx,         dy - 1.0f,  dz - 1.0f));
    float v111 = dot(g111, float3(dx - 1.0f,  dy - 1.0f,  dz - 1.0f));

    float u = Fade(dx), v = Fade(dy), w = Fade(dz);
    float lx0 = lerp(lerp(v000, v100, u), lerp(v010, v110, u), v);
    float lx1 = lerp(lerp(v001, v101, u), lerp(v011, v111, u), v);
    return lerp(lx0, lx1, w);
}

// fBM with periodic Perlin. `basePeriod` is the period at the lowest
// frequency; each octave doubles both frequency and period so the period
// stays consistent through the octave summation.
float Fbm3DPeriodic(float3 p, int s, int basePeriod)
{
    float total = 0.0f;
    float amplitude = 1.0f;
    float maxAmp = 0.0f;
    float freq = 1.0f;
    int period = basePeriod;
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        total += Perlin3DPeriodic(p.x * freq, p.y * freq, p.z * freq, s + i * 1013, period) * amplitude;
        maxAmp += amplitude;
        amplitude *= 0.5f;
        freq *= 2.0f;
        period *= 2;
    }
    return maxAmp > 0.0f ? total / maxAmp : 0.0f;
}

// 特徴点の乱数だけを周期で折り返す。距離は折り返す前の隣接セルから測る。
// レイマーチ中には評価せず、ノイズ生成時にだけ使う。
float InvertedWorley(float3 uvw, int period, int s) {
    float3 p = uvw * period;
    int3 cell = int3(floor(p));
    float3 local = frac(p);
    float nearest = 1.0;
    [loop] for (int z = -1; z <= 1; ++z)
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int x = -1; x <= 1; ++x) {
        int3 offset = int3(x, y, z);
        int3 wrapped = int3(WrapInt(cell.x+x, period), WrapInt(cell.y+y, period), WrapInt(cell.z+z, period));
        uint a = Hash4(wrapped.x, wrapped.y, wrapped.z, s);
        uint b = Hash4(wrapped.x, wrapped.y, wrapped.z, s + 1013);
        float3 feature = float3(a & 65535u, a >> 16, b & 65535u) / 65535.0;
        float3 delta = float3(offset) + feature - local;
        nearest = min(nearest, dot(delta, delta));
    }
    return 1 - sqrt(nearest);
}

[numthreads(8, 8, 1)]
void CSGenerate(uint3 dtid : SV_DispatchThreadID)
{
    if (any(dtid >= resolution))
    {
        return;
    }
    float3 uvw = ((float3)dtid + 0.5) / (float)resolution;

    // Two scales of fBM combined: a wide base shape + finer detail.
    // basePeriod must match the input scale so uvw=1 wraps to the same
    // gradient layout as uvw=0 (otherwise the texture has a visible seam
    // at world-space multiples of horizontalScale).
    float baseShape = Fbm3DPeriodic(uvw * 4.0,  seed,     4);
    float detail    = Fbm3DPeriodic(uvw * 12.0, seed + 1, 12);
    float n = baseShape * 0.7 + detail * 0.3;
    n = saturate(n * 0.5 + 0.5);

    float w4 = InvertedWorley(uvw, 4, seed + 47);
    float w8 = InvertedWorley(uvw, 8, seed + 59);
    float w16 = InvertedWorley(uvw, 16, seed + 71);
    float w32 = InvertedWorley(uvw, 32, seed + 83);
    float lowWorley = w4 * 0.625 + w8 * 0.25 + w16 * 0.125;
    float highWorley = w8 * 0.625 + w16 * 0.25 + w32 * 0.125;
    float perlin = saturate(baseShape * 1.5 + 0.5);
    // Perlin の低い部分を Worley の丸い特徴点で膨らませる。
    // 値域を調整して、全域が高密度にならないようにする。
    float perlinWorley = saturate((lerp(perlin, 1.0, lowWorley) - 0.2) / 0.8);
    float erosion = saturate((highWorley - 0.15) / 0.7);
    RWTexture2DArray<float4> Output = ResourceDescriptorHeap[outputIndex];
    // 実験用の輪郭変位は細部を混ぜない低周波Perlinを使う。既存RGBは維持する。
    float displacement = saturate(Perlin3DPeriodic(uvw.x*4,uvw.y*4,uvw.z*4,seed+97,4)*1.5+0.5);
    Output[dtid] = float4(n, perlinWorley, erosion, displacement);
}
