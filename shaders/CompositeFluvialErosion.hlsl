// 多段解像度の粒子侵食。
// 粒子は反復開始時の地形を読み、64 bit の固定小数和を 32 bit atomic で集計する。
// 同じセルの提案は平均して適用する。
#include "CompositeCommon.hlsli"

struct FluvialErosionConstants {
    uint4 state;   // 入力状態、出力状態、力、差分和（すべて UAV）
    uint4 work;    // 開始点の所有者、提案数、入力 Height SRV、Height UAV
    uint4 grid;    // 現在 n、前段 n、合成 n、反復番号
    uint4 masks;   // 侵食 SRV、硬度 SRV、出力 UAV、出力成分
    float4 scale;  // セル幅、Height の m、基準幅、細部平滑化
    float4 erosion; // 強さ、Channeling、摩擦、粒度 + 1
    float4 motion; // Wear tan、Deposit tan、Max tan、速度
    float4 detail; // Flow Volume、Small Channels、歩数、一定硬度
    float4 force;  // 外力 xy、Shear xy
};
ConstantBuffer<FluvialErosionConstants> g_f : register(b1);
uint N() { return g_f.grid.x; }
int2 Cell(float2 p) { return clamp(int2(p), 0, int(N()) - 1); }
float Random2(float2 p) { return frac(abs(sin(dot(p, float2(1.29898, 7.8233)))) * 1228.543); }
float4 ReadState(float2 uv, uint index, uint n) {
    RWTexture2D<float4> t = ResourceDescriptorHeap[index];
    float2 p = clamp(uv * n - 0.5, 0.0, float(n - 1));
    int2 a = int2(p), b = min(a + 1, int(n) - 1);
    return lerp(lerp(t[a], t[int2(b.x,a.y)], frac(p.x)),
                lerp(t[int2(a.x,b.y)], t[b], frac(p.x)), frac(p.y));
}
float Original(uint2 p, uint n) {
    Texture2D<float> h = ResourceDescriptorHeap[g_f.work.z];
    return DownsampleHeight(h, p, n) * g_f.scale.y;
}
float OriginalLinear(float2 uv, uint n) {
    float2 p = clamp(uv * n - 0.5, 0.0, float(n - 1));
    uint2 a = uint2(p), b = min(a + 1, n - 1);
    return lerp(lerp(Original(a,n), Original(uint2(b.x,a.y),n), frac(p.x)),
                lerp(Original(uint2(a.x,b.y),n), Original(b,n), frac(p.x)), frac(p.y));
}
float MaskAtUv(uint index, float2 uv, float fallback) {
    if (index == kInvalidTextureIndex) return fallback;
    Texture2D<float> t = ResourceDescriptorHeap[index];
    return saturate(t.SampleLevel(g_samplerLinearClamp, uv, 0));
}

float MaskAt(uint index, int2 p, float fallback) {
    return MaskAtUv(index,(float2(p)+0.5)/N(),fallback);
}

[numthreads(8,8,1)]
void CsInit(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= N())) return;
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_f.state.y];
    float h = Original(id.xy, N());
    float4 v = float4(h,0,0,0);
    if (g_f.grid.y > 0) {
        float2 uv = (id.xy + 0.5) / N();
        float4 prev = ReadState(uv, g_f.state.x, g_f.grid.y);
        float coarse = OriginalLinear(uv, g_f.grid.y);
        // 元地形の細部を戻し、侵食の強かった所では復元を弱める。
        float keep = pow(0.1, prev.y * g_f.scale.w / max(g_f.scale.x, 0.0001));
        v = float4(prev.x + (h - coarse) * keep, prev.yzw);
    }
    target[id.xy] = v;
}

// 境界コピーは別バッファに行う。力の計算と同時に高さを書き換えない。
[numthreads(8,8,1)]
void CsBoundary(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= N())) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[g_f.state.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_f.state.y];
    int2 p = id.xy, q = p;
    if (p.x == int(N())-1) q.x--;
    else if (p.x == 0) q.x++;
    else if (p.y == int(N())-1) q.y--;
    else if (p.y == 0) q.y++;
    float4 v = source[p];
    float protection = MaskAt(g_f.masks.x,p,1) * (1-max(g_f.detail.w,MaskAt(g_f.masks.y,p,0)));
    v.x = lerp(v.x, source[q].x, protection);
    v.w += 0.1 * g_f.scale.x;
    target[p] = v;
    RWTexture2D<uint> owner = ResourceDescriptorHeap[g_f.work.x];
    RWTexture2D<uint> count = ResourceDescriptorHeap[g_f.work.y];
    RWTexture2D<uint> sums = ResourceDescriptorHeap[g_f.state.w];
    owner[p] = 0xffffffff; count[p] = 0;
    sums[p]=0; sums[p+int2(N(),0)]=0; sums[p+int2(0,N())]=0; sums[p+int2(N(),N())]=0;
}

[numthreads(8,8,1)]
void CsForces(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= N())) return;
    RWTexture2D<float4> h = ResourceDescriptorHeap[g_f.state.x];
    RWTexture2D<float2> forces = ResourceDescriptorHeap[g_f.state.z];
    float flow = g_f.scale.x <= g_f.scale.z ? g_f.detail.x : 0;
    float base = h[id.xy].x + h[id.xy].y * flow;
    float2 grad = 0;
    for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
        float4 q = h[Cell(float2(id.xy)+float2(x,y))];
        grad += float2(x,y) * (q.x + q.y*flow-base) / (length(float2(x,y))+0.0001);
    }
    forces[id.xy] = -grad/(6*g_f.scale.x) + g_f.force.xy;
}
float2 Start(uint2 id) {
    float dx = g_f.scale.x, it = g_f.grid.w;
    return frac(float2(it, it*3.241)/(N()-1) +
        float2(Random2(float2(id.x*1.8*dx,id.y/49.2*dx)),
               Random2(float2(id.x/1.345*dx+203.12,id.y*dx+502.23))))*(N()-1);
}
bool Eligible(uint2 id, float2 p) {
    RWTexture2D<float4> h = ResourceDescriptorHeap[g_f.state.x];
    RWTexture2D<float2> forces = ResourceDescriptorHeap[g_f.state.z];
    int2 c = Cell(p);
    float curvature = 0;
    for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x)
        curvature += (h[Cell(float2(c)+float2(x,y))].x-h[c].x)/(8*g_f.scale.x);
    float ratio = g_f.scale.x/g_f.scale.z;
    float threshold = 1-pow(ratio, ratio>1 ? 2 : 2-2*g_f.detail.y)/g_f.erosion.w;
    return max(length(forces[c]),-curvature)>g_f.motion.x &&
        Random2(float2(id.x+1042.1,id.y+g_f.grid.w))>threshold && MaskAt(g_f.masks.x,c,1)>0;
}
[numthreads(8,8,1)]
void CsElect(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= N())) return;
    float2 p = Start(id.xy);
    if (!Eligible(id.xy,p)) return;
    RWTexture2D<uint> owner = ResourceDescriptorHeap[g_f.work.x];
    InterlockedMin(owner[Cell(p)], id.y*N()+id.x);
}
// 64 bit 和を下位・上位 32 bit に分ける。加算途中は読まず UAV barrier 後に復元する。
void Accumulate(int2 p, float dh) {
    RWTexture2D<uint> sums = ResourceDescriptorHeap[g_f.state.w];
    RWTexture2D<uint> count = ResourceDescriptorHeap[g_f.work.y];
    int delta = int(round(clamp(dh/g_f.scale.x,-1024.0,1024.0)*65536));
    uint value=asuint(delta), previous;
    InterlockedAdd(sums[p], value, previous);
    InterlockedAdd(sums[p+int2(N(),0)], (delta<0 ? 0xffffffff : 0) + uint(previous+value<previous));
    value = uint(abs(delta));
    InterlockedAdd(sums[p+int2(0,N())], value, previous);
    InterlockedAdd(sums[p+int2(N(),N())], uint(previous+value<previous));
    InterlockedAdd(count[p],1);
}
[numthreads(8,8,1)]
void CsTrace(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= N())) return;
    RWTexture2D<uint> owner = ResourceDescriptorHeap[g_f.work.x];
    RWTexture2D<float4> h = ResourceDescriptorHeap[g_f.state.x];
    RWTexture2D<float2> forces = ResourceDescriptorHeap[g_f.state.z];
    float2 p = Start(id.xy);
    if (owner[Cell(p)] != id.y*N()+id.x) return;
    float2 v=0;
    float initialSlope=length(forces[Cell(p)]), carry=0;
    float ratio=g_f.scale.x/g_f.scale.z;
    for (uint i=0;i<uint(g_f.detail.z);++i) {
        p += g_f.motion.w*v/(1+initialSlope);
        if (any(p<0) || any(p>N()-1)) break;
        int2 c=Cell(p);
        float2 f=forces[c];
        v = v*pow(1-g_f.erosion.z,ratio)+f*ratio;
        float2 vs=v/(length(v)+0.0001);
        vs *= (abs(vs.x)+abs(vs.y))/(length(vs)+0.0001);
        float a=h[c].x, b=h[Cell(p+vs+g_f.force.zw)].x, d=h[Cell(p-vs+g_f.force.zw)].x;
        float slope=length(f);
        if (slope<g_f.motion.y || slope>=g_f.motion.z) break;
        float protection=MaskAt(g_f.masks.x,c,1)*(1-max(g_f.detail.w,MaskAt(g_f.masks.y,c,0)));
        float next=lerp(a,(b+d)*0.5,g_f.erosion.x);
        next -= g_f.erosion.y*max(next-a,0);
        carry += a-next;
        if (carry<0) { next=a-carry; carry=0; }
        next=lerp(a,clamp(next,min(b,d),max(b,d)),protection);
        Accumulate(c,next-a);
    }
}
[numthreads(8,8,1)]
void CsApply(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= N())) return;
    RWTexture2D<float4> h = ResourceDescriptorHeap[g_f.state.x];
    RWTexture2D<float4> target = ResourceDescriptorHeap[g_f.state.y];
    RWTexture2D<uint> sums = ResourceDescriptorHeap[g_f.state.w];
    RWTexture2D<uint> counts = ResourceDescriptorHeap[g_f.work.y];
    uint2 p=id.xy;
    uint4 s=uint4(sums[p],sums[p+uint2(N(),0)],sums[p+uint2(0,N())],sums[p+uint2(N(),N())]);
    float denom=65536.0*max(counts[id.xy],1u);
    bool negative=(s.y & 0x80000000)!=0;
    uint lo=negative ? 0u-s.x : s.x;
    uint hi=negative ? ~s.y+uint(s.x==0) : s.y;
    float delta=(float(hi)*4294967296.0+float(lo))*(negative ? -1 : 1)/denom*g_f.scale.x;
    float amount=(float(s.w)*4294967296.0+float(s.z))/denom*g_f.scale.x;
    float4 v=h[id.xy];
    v.x += delta;
    v.y += max(0,(amount-delta)*0.5);
    v.z += max(0,(amount+delta)*0.5);
    v.w *= pow(0.5,amount*10/g_f.scale.x);
    target[id.xy]=v;
}
[numthreads(8,8,1)]
void CsResolve(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=g_f.grid.z)) return;
    RWTexture2D<float> h=ResourceDescriptorHeap[g_f.work.w];
    // 最終状態は差分に変換済みなので、元入力への SRV / UAV 同時アクセスがない。
    float delta=ReadState((id.xy+0.5)/g_f.grid.z,g_f.state.x,N()).x;
    float2 uv=(id.xy+0.5)/g_f.grid.z;
    float protection=MaskAtUv(g_f.masks.x,uv,1)*(1-max(g_f.detail.w,MaskAtUv(g_f.masks.y,uv,0)));
    h[id.xy]=saturate(h[id.xy]+delta*protection/g_f.scale.y);
}
[numthreads(8,8,1)]
void CsDelta(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=N())) return;
    RWTexture2D<float4> h=ResourceDescriptorHeap[g_f.state.x];
    float4 v=h[id.xy]; v.x-=Original(id.xy,N()); h[id.xy]=v;
}
[numthreads(8,8,1)]
void CsMask(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=g_f.grid.z)) return;
    RWTexture2D<float> t=ResourceDescriptorHeap[g_f.masks.z];
    if (g_f.masks.w>2) { t[id.xy]=0; return; }
    float4 v=ReadState((id.xy+0.5)/g_f.grid.z,g_f.state.x,N());
    float amount=v[min(g_f.masks.w+1,3u)];
    t[id.xy]=saturate(1-exp(-max(amount,0)/max(g_f.scale.x,0.0001)));
}
