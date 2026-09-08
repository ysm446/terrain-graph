// Schott et al., Terrain Amplification using Multi-scale Erosion (SIGGRAPH 2024).
// 論文 Sections 3–4 からの実装。U → E → T → D を解像度ごとに分離して実行する。
// state: x=高さ(m), y=集水面積(m²), z=浮遊土砂(内部量), w=流出重みの分母。
// 読み書きは別テクスチャ。分母は別パスで計算し、近傍の再走査を避ける。
#include "CompositeCommon.hlsli"

struct MultiScaleConstants {
    uint4 indices; // input state UAV, output state UAV, input height SRV, output height UAV
    uint4 sizes;   // active n, previous n, composite n, mask SRV
    float4 erosion; // cell meters, maximum step(m), routing exponent, slope exponent
    float4 limits; // drainage exponent, maximum slope, maximum area(m²), height meters
    float4 sediment; // talus slope, thermal step(m), creation coefficient, deposition rate
    float4 conversion; // 土砂の高さ換算、復元強度、尾根の集水閾値（セル数）、未使用
};
ConstantBuffer<MultiScaleConstants> g_mse : register(b1);

static const int2 Neighbors[8] = {
    int2(-1,-1), int2(0,-1), int2(1,-1), int2(-1,0),
    int2(1,0), int2(-1,1), int2(0,1), int2(1,1)
};
bool Inside(int2 p, uint n) { return all(p >= 0) && all(p < int(n)); }
float4 State(int2 p) {
    RWTexture2D<float4> input = ResourceDescriptorHeap[g_mse.indices.x];
    return input[p];
}
float Distance(int2 d) { return length(float2(d)) * g_mse.erosion.x; }
float DownSlope(int2 p, int2 q) {
    return max(0.0f, (State(p).x - State(q).x) / Distance(p-q));
}
// Eq. (1)。寄与元の下り勾配を正規化し、受信側で集める（書き込み競合なし）。
float RoutingWeight(int2 from, int2 to) {
    float sum = State(from).w;
    return sum > 0.0f ? pow(DownSlope(from,to), g_mse.erosion.z) / sum : 0.0f;
}
[numthreads(8,8,1)]
void CsRouting(uint3 tid : SV_DispatchThreadID) {
    int2 p=tid.xy;
    if(!Inside(p,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(p);
    value.w=0;
    [unroll] for(int j=0;j<8;++j) {
        int2 q=p+Neighbors[j];
        if(Inside(q,g_mse.sizes.x)) value.w+=pow(DownSlope(p,q),g_mse.erosion.z);
    }
    output[p]=value;
}
float3 Flow(int2 p) {
    float area = g_mse.erosion.x * g_mse.erosion.x;
    float transported = 0.0f;
    float slope = 0.0f;
    [unroll] for (int j=0; j<8; ++j) {
        int2 q = p + Neighbors[j];
        if (!Inside(q,g_mse.sizes.x)) continue;
        slope = max(slope,DownSlope(p,q));
        if (State(q).x > State(p).x) {
            float w = RoutingWeight(q,p);
            area += w * State(q).y;
            transported += w * State(q).z;
        }
    }
    return float3(area,transported,slope);
}
float Power(float area, float slope) {
    return pow(min(slope,g_mse.limits.y),g_mse.erosion.w) *
           pow(min(area,g_mse.limits.z),g_mse.limits.x);
}
float4 Cubic(float4 a, float4 b, float4 c, float4 d, float t) {
    return b + 0.5f*t*(c-a+t*(2*a-5*b+4*c-d+t*(3*(b-c)+d-a)));
}
float4 Upsampled(int2 cell) {
    RWTexture2D<float4> input = ResourceDescriptorHeap[g_mse.indices.x];
    float2 p = (float2(cell)+0.5f)*float(g_mse.sizes.y)/float(g_mse.sizes.x)-0.5f;
    int2 base = int2(floor(p));
    float2 t = frac(p);
    float4 rows[4];
    [unroll] for (int y=0;y<4;++y) {
        float4 v[4];
        [unroll] for (int x=0;x<4;++x)
            v[x] = input[clamp(base+int2(x-1,y-1),0,int(g_mse.sizes.y)-1)];
        rows[y]=Cubic(v[0],v[1],v[2],v[3],t.x);
    }
    return Cubic(rows[0],rows[1],rows[2],rows[3],t.y);
}
[numthreads(8,8,1)]
void CsInit(uint3 tid : SV_DispatchThreadID) {
    uint2 p=tid.xy;
    if (!Inside(p,g_mse.sizes.x)) return;
    Texture2D<float> height=ResourceDescriptorHeap[g_mse.indices.z];
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    // セルの被覆領域で平均する。非整数比でも全テクセルを面積で重み付けする。
    float2 lo=float2(p)*float(g_mse.sizes.z)/float(g_mse.sizes.x);
    float2 hi=float2(p+1)*float(g_mse.sizes.z)/float(g_mse.sizes.x);
    float sum=0, area=0;
    for(int y=int(floor(lo.y));y<int(ceil(hi.y));++y)
        for(int x=int(floor(lo.x));x<int(ceil(hi.x));++x) {
            float weight=max(0.0f,min(hi.x,float(x+1))-max(lo.x,float(x))) *
                         max(0.0f,min(hi.y,float(y+1))-max(lo.y,float(y)));
            sum+=height.Load(int3(clamp(int2(x,y),0,int(g_mse.sizes.z)-1),0))*weight;
            area+=weight;
        }
    output[p]=float4(sum/max(area,1e-8f)*g_mse.limits.w,
                    g_mse.erosion.x*g_mse.erosion.x,0,0);
}
[numthreads(8,8,1)]
void CsUpsample(uint3 tid : SV_DispatchThreadID) {
    if (!Inside(tid.xy,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=Upsampled(tid.xy);
    // 面積は m² のまま補間。新しいセル面積を下限にする。堆積段階は g0=0。
    output[tid.xy]=float4(value.x,max(value.y,g_mse.erosion.x*g_mse.erosion.x),0,0);
}
[numthreads(8,8,1)]
void CsErode(uint3 tid : SV_DispatchThreadID) {
    int2 p=tid.xy;
    if (!Inside(p,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(p);
    float3 flow=Flow(p);
    // Eq. (5) を最大深さ/反復数で正規化。上限はレベルごとに独立。
    float maximumPower=pow(g_mse.limits.y,g_mse.erosion.w)*pow(g_mse.limits.z,g_mse.limits.x);
    value.x-=g_mse.erosion.y*Power(value.y,flow.z)/max(maximumPower,1e-12f);
    value.y=flow.x;
    value.z=0;
    output[p]=value;
}
[numthreads(8,8,1)]
void CsThermal(uint3 tid : SV_DispatchThreadID) {
    int2 p=tid.xy;
    if (!Inside(p,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(p);
    float balance=0;
    [unroll] for(int j=0;j<8;++j) {
        int2 q=p+Neighbors[j];
        if(!Inside(q,g_mse.sizes.x)) continue;
        float slope=(State(q).x-value.x)/Distance(p-q);
        balance += slope > g_mse.sediment.x ? 1.0f :
                   (slope < -g_mse.sediment.x ? -1.0f : 0.0f);
    }
    value.x+=g_mse.sediment.y*balance;
    value.z=0;
    output[p]=value;
}
[numthreads(8,8,1)]
void CsDeposit(uint3 tid : SV_DispatchThreadID) {
    int2 p=tid.xy;
    if(!Inside(p,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(p);
    float3 flow=Flow(p);
    float power=Power(value.y,flow.z);
    // Section 4.4: ci=kc*ei, ti=Σw(q,p)*gi(q), di=min(ti,kd*max(ti-ei,0))。
    float deposited=min(flow.y,g_mse.sediment.w*max(flow.y-power,0.0f));
    value.x+=deposited*g_mse.conversion.x;
    value.y=flow.x;
    value.z=max(0.0f,g_mse.sediment.z*power+flow.y-deposited);
    output[p]=value;
}
[numthreads(8,8,1)]
void CsFlowReset(uint3 tid : SV_DispatchThreadID) {
    if(!Inside(tid.xy,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(tid.xy);
    value.y=g_mse.erosion.x*g_mse.erosion.x;
    output[tid.xy]=value;
}
[numthreads(8,8,1)]
void CsFlowOnly(uint3 tid : SV_DispatchThreadID) {
    if(!Inside(tid.xy,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(tid.xy);
    value.y=Flow(tid.xy).x;
    output[tid.xy]=value;
}
float OriginalHeight(int2 p) {
    Texture2D<float> input=ResourceDescriptorHeap[g_mse.indices.z];
    float2 pos=(float2(p)+0.5f)*float(g_mse.sizes.z)/float(g_mse.sizes.x)-0.5f;
    int2 lo=int2(floor(pos));
    float2 t=frac(pos);
    int2 a=clamp(lo,0,int(g_mse.sizes.z)-1), b=clamp(lo+1,0,int(g_mse.sizes.z)-1);
    return lerp(lerp(input.Load(int3(a,0)),input.Load(int3(b.x,a.y,0)),t.x),
                lerp(input.Load(int3(a.x,b.y,0)),input.Load(int3(b,0)),t.x),t.y)*g_mse.limits.w;
}
// Section 5.1: 制約点の標高差を固定し、周囲へ誤差だけを拡散する。
[numthreads(8,8,1)]
void CsRetargetInit(uint3 tid : SV_DispatchThreadID) {
    if(!Inside(tid.xy,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(tid.xy);
    bool ridge=value.y < g_mse.conversion.z*g_mse.erosion.x*g_mse.erosion.x;
    float error=ridge ? OriginalHeight(tid.xy)-value.x : 0;
    output[tid.xy]=float4(value.x,error,error,ridge ? 1 : 0);
}
[numthreads(8,8,1)]
void CsRetargetStep(uint3 tid : SV_DispatchThreadID) {
    int2 p=tid.xy;
    if(!Inside(p,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(p);
    float sum=0, count=0;
    [unroll] for(int j=0;j<8;++j) {
        int2 q=p+Neighbors[j];
        if(Inside(q,g_mse.sizes.x)) { sum+=State(q).z; count+=1; }
    }
    value.z=value.w > 0 ? value.y : sum/max(count,1);
    output[p]=value;
}
[numthreads(8,8,1)]
void CsRetargetApply(uint3 tid : SV_DispatchThreadID) {
    if(!Inside(tid.xy,g_mse.sizes.x)) return;
    RWTexture2D<float4> output=ResourceDescriptorHeap[g_mse.indices.y];
    float4 value=State(tid.xy);
    value.x+=g_mse.conversion.y*value.z;
    output[tid.xy]=value;
}
[numthreads(8,8,1)]
void CsResolve(uint3 tid : SV_DispatchThreadID) {
    uint2 p=tid.xy;
    if(!Inside(p,g_mse.sizes.z)) return;
    RWTexture2D<float4> input=ResourceDescriptorHeap[g_mse.indices.x];
    RWTexture2D<float> output=ResourceDescriptorHeap[g_mse.indices.w];
    float2 pos=(float2(p)+0.5f)*float(g_mse.sizes.x)/float(g_mse.sizes.z)-0.5f;
    int2 lo=int2(floor(pos));
    float2 t=frac(pos);
    int2 a=clamp(lo,0,int(g_mse.sizes.x)-1), b=clamp(lo+1,0,int(g_mse.sizes.x)-1);
    float height=lerp(lerp(input[int2(a.x,a.y)].x,input[int2(b.x,a.y)].x,t.x),
                      lerp(input[int2(a.x,b.y)].x,input[int2(b.x,b.y)].x,t.x),t.y);
    float mask=1;
    if(g_mse.sizes.w != kInvalidTextureIndex) {
        Texture2D<float> maskTexture=ResourceDescriptorHeap[g_mse.sizes.w];
        mask=saturate(maskTexture.Load(int3(p,0)));
    }
    output[p]=lerp(output[p],saturate(height/g_mse.limits.w),mask);
}
