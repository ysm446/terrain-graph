// Snow Cover の Deep Snow / Dusting。高さ・雪深は内部では m。
// 並行書き込みは段ごとの読み取りと集計に分離する。
#include "CompositeCommon.hlsli"

struct SnowCoverConstants {
    uint4 textures; // 入力状態、出力状態、weather、最終出力（UAV）
    uint4 inputs;   // 入力 Height SRV、Height UAV、降雪 Mask SRV、Mask UAV
    uint4 grid;     // 現在解像度、前段解像度、最終解像度、出力成分
    uint4 work;     // 粒子 UAV、集計 UAV、粒子数、ランプ点数
    float4 scale;   // 現在セル幅、最終セル幅、Height の m、地形幅
    float4 snow;    // 降雪深、流量補正、最大傾斜 tan、融雪
    float4 snowLine;    // 雪線強度（負なら無効）、高さ、フェザー、風ぼかし半径（セル）
    float4 wind;    // xyz: 風向、w: 強度（負なら無効）
    float4 dust;    // 強度（負なら無効）、滑落角 tan、フェザー下端 tan、曲率影響
    float4 noise;   // 強さ（負なら無効）、スケール、roughness、octaves
    float4 advection; // 流動長、流量補正、強度、値の保持率
    float4 ramp[8]; // 位置、値、補間（0: 定数、1: 線形、2: smooth）
};
ConstantBuffer<SnowCoverConstants> g_cover : register(b1);

int2 CoverCell(int2 p, uint n) { return clamp(p, 0, int(n) - 1); }
float4 CoverRead(uint index, float2 p, uint n) {
    RWTexture2D<float4> field = ResourceDescriptorHeap[index];
    p = clamp(p, 0.0f, float(n - 1));
    int2 a = int2(p), b = min(a + 1, int(n - 1));
    return lerp(lerp(field[a], field[int2(b.x, a.y)], frac(p.x)),
                lerp(field[int2(a.x, b.y)], field[b], frac(p.x)), frac(p.y));
}
float CoverOriginal(int2 p) {
    Texture2D<float> source = ResourceDescriptorHeap[g_cover.inputs.x];
    return source.Load(int3(CoverCell(p, g_cover.grid.z), 0)) * g_cover.scale.z;
}
float CoverRamp(float x) {
    float value = g_cover.ramp[0].y;
    for (uint i = 1; i < g_cover.work.w; ++i) {
        float4 a = g_cover.ramp[i - 1], b = g_cover.ramp[i];
        if (x < b.x) {
            float t = saturate((x - a.x) / max(b.x - a.x, 1e-6f));
            if (a.z < 0.5f) t = 0;
            else if (a.z > 1.5f) t = t * t * (3 - 2 * t);
            return lerp(a.y, b.y, t);
        }
        value = b.y;
    }
    return value;
}

// Wind 用の Height だけを分離 Gaussian でならす。積雪はならさない。
[numthreads(8,8,1)]
void CsWindHorizontal(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_cover.textures.y];
    int radius = int(ceil(g_cover.snowLine.w));
    float sigma = max(g_cover.snowLine.w * 0.5f, 0.5f), sum = 0, weights = 0;
    for (int i = -radius; i <= radius; ++i) {
        float w = exp(-0.5f * i * i / (sigma * sigma));
        sum += CoverOriginal(int2(id.xy) + int2(i,0)) * w;
        weights += w;
    }
    target[id.xy] = float4(sum / weights, 0, 0, 0);
}
[numthreads(8,8,1)]
void CsWindVertical(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_cover.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_cover.textures.y];
    int radius = int(ceil(g_cover.snowLine.w));
    float sigma = max(g_cover.snowLine.w * 0.5f, 0.5f), sum = 0, weights = 0;
    for (int i = -radius; i <= radius; ++i) {
        float w = exp(-0.5f * i * i / (sigma * sigma));
        sum += source[CoverCell(int2(id.xy) + int2(0,i), g_cover.grid.z)].x * w;
        weights += w;
    }
    target[id.xy] = float4(sum / weights, 0, 0, 0);
}
float CoverWindHeight(int2 p) {
    RWTexture2D<float4> field = ResourceDescriptorHeap[g_cover.textures.x];
    return g_cover.snowLine.w > 0 ? field[CoverCell(p,g_cover.grid.z)].x : CoverOriginal(p);
}
[numthreads(8,8,1)]
void CsWeather(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> weather = ResourceDescriptorHeap[g_cover.textures.z];
    float h = CoverOriginal(id.xy), weight = 1;
    if (g_cover.snowLine.x >= 0) {
        // このアプリの標高原点は Height 0.5（変位 0）。
        float elevation = h - 0.5f * g_cover.scale.z;
        float t = g_cover.snowLine.z > 0 ? saturate((elevation - g_cover.snowLine.y) / g_cover.snowLine.z)
                                                   : float(elevation >= g_cover.snowLine.y);
        weight *= lerp(1 - g_cover.snowLine.x, 1, CoverRamp(t));
    }
    if (g_cover.inputs.z != kInvalidTextureIndex) {
        Texture2D<float> mask = ResourceDescriptorHeap[g_cover.inputs.z];
        weight *= saturate(mask.SampleLevel(g_samplerLinearClamp, (id.xy + 0.5f) / g_cover.grid.z, 0));
    }
    if (g_cover.wind.w >= 0) {
        int2 p = id.xy;
        float2 grad = float2(CoverWindHeight(p+int2(1,0))-CoverWindHeight(p-int2(1,0)),
                            CoverWindHeight(p+int2(0,1))-CoverWindHeight(p-int2(0,1))) / (2*g_cover.scale.y);
        float3 normal = normalize(float3(-grad.x, 1, -grad.y));
        float facing = saturate(-dot(g_cover.wind.xyz, normal));
        weight = saturate(weight * lerp(1-g_cover.wind.w, 1+g_cover.wind.w, facing));
    }
    weather[id.xy] = float4(h, weight, weight, 0);
}

// 初期粗格子へ積雪を落とす。以降は厚みを拡大し、その段の元地形へ足す。
[numthreads(8,8,1)]
void CsInit(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.x)) return;
    RWTexture2D<float4> weather = ResourceDescriptorHeap[g_cover.textures.z];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_cover.textures.y];
    uint n = g_cover.grid.x, full = g_cover.grid.z;
    uint2 lo = id.xy * full / n, hi = max(lo + 1, (id.xy + 1) * full / n);
    float h = 0, depth = 0, count = 0;
    for (uint y = lo.y; y < hi.y; ++y) for (uint x = lo.x; x < hi.x; ++x) {
        float4 w = weather[uint2(x,y)];
        h += w.x; depth += w.y * 1.01f * g_cover.snow.x; ++count;
    }
    h /= count; depth /= count;
    float flow = 0;
    if (g_cover.grid.y > 0) {
        float4 prev = CoverRead(g_cover.textures.x, (id.xy + 0.5f) / n * g_cover.grid.y - 0.5f, g_cover.grid.y);
        depth = max(prev.x - prev.y, 0); flow = prev.z;
    }
    // x: 雪面、y: 下地、z: 累積流動量、w: この段の開始時の流動量
    target[id.xy] = float4(h + depth, h, flow, flow);
}
[numthreads(8,8,1)]
void CsSettle(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.x)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_cover.textures.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_cover.textures.y];
    int2 p = id.xy;
    float4 own = source[p];
    const int2 offsets[8] = {int2(1,0),int2(0,1),int2(-1,0),int2(0,-1),
                           int2(1,1),int2(-1,-1),int2(-1,1),int2(1,-1)};
    float delta = 0, flow = 0;
    for (int k = 0; k < 8; ++k) {
        float4 other = source[CoverCell(p + offsets[k],g_cover.grid.x)];
        float transfer = other.x-own.x + g_cover.snow.y*(other.z-own.z);
        transfer *= k >= 4 ? 1.0f : 0.7f; // 軸方向と斜め方向で移動係数を変える。
        transfer = clamp(transfer, own.y-own.x, other.x-other.y);
        delta += transfer; flow += abs(transfer)*0.17f;
    }
    float2 grad = float2(source[CoverCell(p+int2(1,0),g_cover.grid.x)].x-source[CoverCell(p-int2(1,0),g_cover.grid.x)].x,
                        source[CoverCell(p+int2(0,1),g_cover.grid.x)].x-source[CoverCell(p-int2(0,1),g_cover.grid.x)].x);
    float len = length(grad);
    // normalize(0) の未定義結果を避け、平面は自身をサンプルする。
    grad = len > 1e-8f ? grad/len : 0;
    float hsample = CoverRead(g_cover.textures.x, float2(p)-grad,g_cover.grid.x).x;
    own.x = max(min(own.x+delta*0.17f,hsample+g_cover.snow.z*g_cover.scale.x),own.y);
    own.z += flow;
    target[p] = own;
}

float2 CoverRandom(float2 p) {
    return frac(sin(float2(dot(p,float2(127.141f,311.742f)),dot(p,float2(269.513f,183.357f))))*621.5153f);
}
// Dusting の Fluvial Advection。Snow Base が使う Smooth / 摩擦 1 / 速度 1 の組み合わせ。
// field/flowmap は移動ステップごとに集計する。
[numthreads(8,8,1)]
void CsParticles(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> particles = ResourceDescriptorHeap[g_cover.work.x];
    RWTexture2D<float4> weather = ResourceDescriptorHeap[g_cover.textures.z];
    RWTexture2D<uint> sums = ResourceDescriptorHeap[g_cover.work.y];
    sums[id.xy] = 0; sums[id.xy+uint2(g_cover.grid.z,0)] = 0;
    float2 p = CoverRandom(id.xy)*g_cover.grid.z;
    float value = weather[CoverCell(int2(p),g_cover.grid.z)].z;
    particles[id.xy] = float4(p,value,float(id.y*g_cover.grid.z+id.x < g_cover.work.z));
}
[numthreads(8,8,1)]
void CsAdvect(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z) || id.y*g_cover.grid.z+id.x >= g_cover.work.z) return;
    RWTexture2D<float4> particles = ResourceDescriptorHeap[g_cover.work.x];
    RWTexture2D<float4> weather = ResourceDescriptorHeap[g_cover.textures.z];
    RWTexture2D<uint> sums = ResourceDescriptorHeap[g_cover.work.y];
    float4 particle = particles[id.xy];
    if (particle.w == 0) return;
    int2 p = CoverCell(int2(particle.xy),g_cover.grid.z);
    float4 right=weather[CoverCell(p+int2(1,0),g_cover.grid.z)], left=weather[CoverCell(p-int2(1,0),g_cover.grid.z)];
    float4 up=weather[CoverCell(p+int2(0,1),g_cover.grid.z)], down=weather[CoverCell(p-int2(0,1),g_cover.grid.z)];
    float2 grad = float2(right.x-left.x,up.x-down.x)/(2*g_cover.scale.y);
    float hmin = min(min(right.x,left.x),min(up.x,down.x));
    if (hmin > weather[p].x || length(grad) > 1.73205080757f) {
        particle.w=0; particles[id.xy]=particle; return;
    }
    grad += 0.1f*g_cover.advection.y*float2(right.w-left.w,up.w-down.w)/(2*g_cover.scale.y);
    if (length(grad) <= 1e-8f) { particle.w=0; particles[id.xy]=particle; return; }
    particle.xy -= normalize(grad);
    p = CoverCell(int2(particle.xy),g_cover.grid.z);
    particle.z = lerp(weather[p].z,particle.z,pow(g_cover.advection.w,g_cover.scale.y));
    InterlockedAdd(sums[p],uint(round(saturate(particle.z)*4096)));
    InterlockedAdd(sums[p+int2(g_cover.grid.z,0)],1);
    particles[id.xy]=particle;
}
[numthreads(8,8,1)]
void CsAdvectApply(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> weather = ResourceDescriptorHeap[g_cover.textures.z];
    RWTexture2D<uint> sums = ResourceDescriptorHeap[g_cover.work.y];
    uint2 total = uint2(sums[id.xy],sums[id.xy+uint2(g_cover.grid.z,0)]);
    float4 value = weather[id.xy];
    if (total.y > 0) {
        value.z = lerp(value.z, float(total.x)/(4096*float(total.y)), 1-pow(1-g_cover.advection.z,float(total.y)));
        value.w += total.y*g_cover.advection.z;
    }
    weather[id.xy]=value; sums[id.xy]=0; sums[id.xy+uint2(g_cover.grid.z,0)]=0;
}

// 連続 3D gradient noise。乱数列はエンジン側で定義。
float CoverHash(float3 p) { return frac(sin(dot(p,float3(127.1,311.7,74.7)))*43758.5453); }
float CoverNoise(float3 p) {
    float3 cell=floor(p), f=frac(p), w=f*f*f*(f*(f*6-15)+10);
    float sum=0;
    for (int z=0;z<2;++z) for (int y=0;y<2;++y) for (int x=0;x<2;++x) {
        float3 corner=float3(x,y,z), q=cell+corner;
        float3 gradient=float3(CoverHash(q),CoverHash(q+19.17),CoverHash(q+47.31))*2-1;
        gradient /= max(length(gradient),1e-6f);
        float3 weight=lerp(1-w,w,corner);
        sum+=dot(gradient,f-corner)*weight.x*weight.y*weight.z;
    }
    return sum;
}
[numthreads(8,8,1)]
void CsFinish(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_cover.textures.x];
    RWTexture2D<float4> weather = ResourceDescriptorHeap[g_cover.textures.z];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_cover.textures.w];
    float4 state=source[id.xy];
    float depth=max(0,state.x-state.y-(0.01f*g_cover.snow.x+0.01f)-2*g_cover.snow.w*g_cover.snow.x);
    float cover=depth > 0 ? 1 : 0;
    if (g_cover.dust.x >= 0) {
        int2 p=id.xy;
        float h=weather[p].x;
        float hr=weather[CoverCell(p+int2(1,0),g_cover.grid.z)].x, hl=weather[CoverCell(p-int2(1,0),g_cover.grid.z)].x;
        float hu=weather[CoverCell(p+int2(0,1),g_cover.grid.z)].x, hd=weather[CoverCell(p-int2(0,1),g_cover.grid.z)].x;
        float slope=length(float2(hr-hl,hu-hd)/(2*g_cover.scale.y));
        if (g_cover.noise.x >= 0) {
            float3 pos=float3((id.x+0.5f)*g_cover.scale.y-0.5f*g_cover.scale.w,0,
                             (id.y+0.5f)*g_cover.scale.y-0.5f*g_cover.scale.w)/g_cover.noise.y;
            float n=CoverNoise(pos);
            for (int i=0;i<int(g_cover.noise.w);++i) n+=pow(g_cover.noise.z,i+1)*CoverNoise(pos*exp2(float(i+1)));
            slope *= 1+n*g_cover.noise.x;
        }
        float mask=weather[p].z;
        float lo=g_cover.dust.z*mask, hi=g_cover.dust.y*mask;
        float adherence=hi-lo > 1e-8f ? 1-saturate((slope-lo)/(hi-lo)) : float(slope <= hi);
        adherence *= 1+(hr+hl+hu+hd-4*h)/g_cover.scale.y*g_cover.dust.w;
        float dust=mask > 0.01f ? adherence*g_cover.dust.x : 0;
        cover=max(cover,saturate(dust));
    }
    target[id.xy]=float4(cover,depth,max(0,state.z-state.w)/g_cover.scale.y,0);
}
[numthreads(8,8,1)]
void CsResolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float4> source=ResourceDescriptorHeap[g_cover.textures.w];
    RWTexture2D<float> target=ResourceDescriptorHeap[g_cover.inputs.y];
    target[id.xy]=saturate(target[id.xy]+source[id.xy].y/g_cover.scale.z);
}
[numthreads(8,8,1)]
void CsMask(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_cover.grid.z)) return;
    RWTexture2D<float> target=ResourceDescriptorHeap[g_cover.inputs.w];
    if (g_cover.grid.w >= 3) { target[id.xy]=0; return; }
    RWTexture2D<float4> source=ResourceDescriptorHeap[g_cover.textures.w];
    target[id.xy]=source[id.xy][g_cover.grid.w];
}
