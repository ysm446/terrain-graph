// 選択した外周を、距離に応じて指定標高へ滑らかに寄せる。
#include "CompositeCommon.hlsli"
struct FlattenConstants {
    uint4 indices; // Height UAV、Mask SRV、解像度、辺のビット
    float4 shape;  // 幅、目標標高、持ち上げ、強さ
    float4 scale;  // 地形一辺 m、標高差 m
};
ConstantBuffer<FlattenConstants> g_flat : register(b1);
[numthreads(8,8,1)]
void CsFlatten(uint3 id : SV_DispatchThreadID) {
    uint n=g_flat.indices.z;
    if(any(id.xy>=n)) return;
    RWTexture2D<float> h=ResourceDescriptorHeap[g_flat.indices.x];
    float2 p=float2(id.xy)*g_flat.scale.x/n;
    float2 distance=max(g_flat.scale.xx,g_flat.shape.xx);
    if(g_flat.indices.w&1) distance.x=min(distance.x,p.x);
    if(g_flat.indices.w&2) distance.x=min(distance.x,g_flat.scale.x-p.x);
    if(g_flat.indices.w&4) distance.y=min(distance.y,p.y);
    if(g_flat.indices.w&8) distance.y=min(distance.y,g_flat.scale.x-p.y);
    float w=g_flat.shape.x;
    float edge=w>0 ? smoothstep(0,w,length(max(w-distance,0))) : 0;
    float mask=1;
    if(g_flat.indices.y!=kInvalidTextureIndex) {
        Texture2D<float> m=ResourceDescriptorHeap[g_flat.indices.y];
        mask=saturate(m.SampleLevel(g_samplerLinearClamp,(id.xy+0.5)/n,0));
    }
    float original=h[id.xy]*g_flat.scale.y;
    float lifted=original+g_flat.shape.z;
    float result=lerp(lifted,g_flat.shape.y,edge*g_flat.shape.w);
    h[id.xy]=saturate(lerp(original,result,mask)/g_flat.scale.y);
}
