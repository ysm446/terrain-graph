// 1候補1スレッドでモデル選択と包囲球判定。元IDを詰め直し、間接描画の件数を作る。
struct CullConstants {
    float4 planes[6];
    uint points, visible, arguments, count;
    uint seed, partCount; float weightStart, weightEnd;
    float scaleMin, scaleMax, modelSize, radius;
    float3 camera; float maxDistance;
    float offset; uint usePointSize; uint2 padding;
};
ConstantBuffer<CullConstants> g_cull : register(b1);
struct DrawArguments { uint indexCount, instanceCount, startIndex; int baseVertex; uint startInstance; };
uint InstanceHash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }
float InstanceRandom(uint x) { return float(InstanceHash(x) >> 8) / 16777216.0; }
[numthreads(64,1,1)]
void CsCull(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_cull.count) return;
    Texture2D<float4> points = ResourceDescriptorHeap[g_cull.points];
    float4 placement = points.Load(int3(id.x%1024,id.x/1024,0));
    float choice = InstanceRandom(id.x ^ g_cull.seed);
    if (placement.w <= 0 || choice < g_cull.weightStart || choice >= g_cull.weightEnd) return;
    float scale = lerp(g_cull.scaleMin,g_cull.scaleMax,InstanceRandom(id.x ^ g_cull.seed ^ 0x3187u));
    if (g_cull.usePointSize != 0) scale *= placement.w/g_cull.modelSize;
    // 回転・法線追従後も含む、底面ピボット中心の保守的な包囲球。
    float radius = g_cull.radius*abs(scale)+abs(g_cull.offset);
    for (uint i=0;i<6;++i)
        if (dot(g_cull.planes[i],float4(placement.xyz,1)) < -radius) return;
    if (g_cull.maxDistance > 0 && distance(placement.xyz,g_cull.camera) > g_cull.maxDistance+radius) return;
    RWStructuredBuffer<DrawArguments> arguments = ResourceDescriptorHeap[g_cull.arguments];
    RWStructuredBuffer<uint> visible = ResourceDescriptorHeap[g_cull.visible];
    uint destination;
    InterlockedAdd(arguments[0].instanceCount,1,destination);
    visible[destination] = id.x;
}
[numthreads(64,1,1)]
void CsFinish(uint3 id : SV_DispatchThreadID) {
    if (id.x == 0 || id.x >= g_cull.partCount) return;
    RWStructuredBuffer<DrawArguments> arguments = ResourceDescriptorHeap[g_cull.arguments];
    arguments[id.x].instanceCount = arguments[0].instanceCount;
}
