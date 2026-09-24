// 1候補1スレッドでモデル選択と包囲球判定、距離による LOD の選択。
// 元IDを区画ごとに詰め直し、間接描画の件数を作る。
//
// 区画: [0, L) は LOD ごとの通常の区画、[L, 2L) は切り替え中の区画（L は LOD の段数）。
// 切り替え中のインスタンスは去る段と来る段の両方へ入れ、ピクセルシェーダが相補的な
// ディザで抜く（ModelPreview.hlsl の PsDither）。要素の y は進み具合 t（0〜1）で、
// 去る段には t + 2 を入れて区別する。通常の区画は 1。
struct CullConstants {
    float4 planes[6];
    uint points, visible, arguments, count;
    uint seed, argumentCount; float weightStart, weightEnd;
    float scaleMin, scaleMax, modelSize, radius;
    float3 camera; float maxDistance;
    float offset; uint usePointSize; uint lodCount; float fadeBand;
    // lodStart[k]: LOD k に替わる距離（等倍、倍率を掛け済み）。[0] は使わない。
    float4 lodStart;
    // 区画ごとの先頭の描画引数。区画の件数はここに数える。
    uint4 segmentFirst[2];
    // statCounters: 区画ごとの件数を 1 フレームぶん足し込む先。statOffset は本描画 0、影 8。
    uint segmentCount, statCounters, statOffset, padding;
};
ConstantBuffer<CullConstants> g_cull : register(b1);
struct DrawArguments { uint indexCount, instanceCount, startIndex; int baseVertex; uint startInstance; };
uint InstanceHash(uint x) { x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; return x ^ (x >> 16); }
float InstanceRandom(uint x) { return float(InstanceHash(x) >> 8) / 16777216.0; }
uint SegmentFirst(uint segment) { return g_cull.segmentFirst[segment >> 2][segment & 3]; }
void Append(uint segment, uint id, float fade) {
    RWStructuredBuffer<DrawArguments> arguments = ResourceDescriptorHeap[g_cull.arguments];
    RWStructuredBuffer<uint2> visible = ResourceDescriptorHeap[g_cull.visible];
    uint destination;
    InterlockedAdd(arguments[SegmentFirst(segment)].instanceCount,1,destination);
    visible[segment*g_cull.count+destination] = uint2(id,asuint(fade));
}
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
    const float cameraDistance = distance(placement.xyz,g_cull.camera);
    if (g_cull.maxDistance > 0 && cameraDistance > g_cull.maxDistance+radius) return;
    // 切り替え距離はインスタンスの倍率に合わせて伸び縮みさせる（大きい株ほど遠くまで詳細）。
    uint lod = 0;
    for (uint k=1;k<g_cull.lodCount;++k)
        if (cameraDistance >= g_cull.lodStart[k]*abs(scale)) lod = k;
    if (lod > 0 && g_cull.fadeBand > 0) {
        const float start = g_cull.lodStart[lod]*abs(scale);
        const float t = (cameraDistance-start)/max(start*g_cull.fadeBand,1e-4);
        if (t < 1) {
            Append(g_cull.lodCount+lod,id.x,t);
            Append(g_cull.lodCount+lod-1,id.x,t+2);
            return;
        }
    }
    Append(lod,id.x,1);
}
// 区画の先頭の件数を、同じ区画の他のパーツの描画引数へ写す。
[numthreads(64,1,1)]
void CsFinish(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_cull.argumentCount) return;
    uint first = 0;
    for (uint segment=0;segment<g_cull.segmentCount;++segment) {
        const uint candidate = SegmentFirst(segment);
        if (candidate <= id.x) first = candidate;
    }
    if (first == id.x) return;
    RWStructuredBuffer<DrawArguments> arguments = ResourceDescriptorHeap[g_cull.arguments];
    arguments[id.x].instanceCount = arguments[first].instanceCount;
}
// 区画ごとの件数を統計へ足す（CsCull / CsFinish の後に 1 グループで呼ぶ）。
[numthreads(8,1,1)]
void CsAccumulate(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_cull.segmentCount) return;
    RWStructuredBuffer<DrawArguments> arguments = ResourceDescriptorHeap[g_cull.arguments];
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[g_cull.statCounters];
    counters[g_cull.statOffset+id.x] += arguments[SegmentFirst(id.x)].instanceCount;
}
