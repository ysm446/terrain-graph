// 水滴侵食（Droplet Erosion）。terrain-editor の droplet_erosion_compute.hlsl を移したもの。
//
// 容量ベースの粒子侵食（Sebastian Lague / Hans Beyer 流）。水滴を 1 つずつ地形に落とし、
// 慣性つきで斜面を下らせる。運べる量（容量 = 傾斜 × 速度 × 水量 × 係数）より
// 持っている土砂が少なければ削り、多ければ捨て、上りにぶつかれば窪みを埋める。
//
// CPU 版は逐次（1 滴ごとに地形を書き換え、次の滴がそれを見る）で、そのままでは並列に
// できない。ここでは**スナップショット方式**にする。
//   1 反復 = 凍らせた地形に対して全水滴を走らせ、削り / 堆積を固定小数点の int へ
//   InterlockedAdd で積み（HLSL に float の atomic は無く、固定小数点なら順序に依らず
//   決定的）、最後に飽和つきで足す（1 反復で 1 セルが動ける上限 deltaCap）。
// 次の反復は掘れた谷を見るので、反復を重ねるほど水系が深く枝分かれする。
// CPU 版とビット一致はしないが、見た目は同等。
//
// 放射状のブラシ（侵食半径）は持たない（atomic が多すぎる）。4 セルへの双線形の splat と
// 飽和で代える（terrain-editor の GPU 版と同じ）。
//
// **合成の Height とは別の解析グリッドで回す。** 粗いレベル（64²）から倍々に上げる
// マルチグリッドで、粗いレベルが大きな谷を、細かいレベルが枝を決める。結果は差分として
// 合成解像度へ足し戻す（素材の凹凸は壊さない）。作業ハイトは **m 単位**で持つ
// （容量や蒸発の係数が m で決めてあるため）。
//
//   CsOriginal    合成の Height を解析グリッド（最終解像度）へ落として控える（差分用）
//   CsInit        合成の Height を最初のレベルへ落とす
//   CsCopyToSrc   前のレベルの作業ハイトを拡大元へ写す
//   CsUpsample    拡大元を次のレベルへ双線形で拡大する
//   CsClearLevel  流量 / 堆積を 0 に
//   CsClearIter   1 反復の差分を 0 に
//   CsTrace       1 スレッド 1 水滴。生む → 下る → 削る / 積む
//   CsApply       差分を飽和つきで作業ハイトへ足す
//   CsResolve     最終レベルの作業ハイト − 元の高さ を合成の Height へ足す
//   CsMaskMax*    マスクの正規化用の最大値
//   CsMask        マスク（流量 / 堆積）を合成解像度で焼く
//
// レベルは同じテクスチャの左上 n×n を使う（テクスチャは最終解像度で確保する）。

#include "CompositeCommon.hlsli"

struct DropletConstants
{
    // UAV: x: 作業ハイト（m）、y: 拡大元、z: 元の高さ（最終解像度、m）、w: 1 反復の差分（固定小数 int）
    uint4 indices0;
    // x: 流量 UAV（固定小数 int）、y: 堆積 UAV（固定小数 int）、z: 合成 Height SRV、w: 合成 Height UAV
    uint4 indices1;
    // x: このレベルの一辺 n、y: 拡大元の一辺 srcN、z: この反復の水滴の数、w: 歩数の上限
    uint4 indices2;
    // x: シード、y: レベルのシード、z: 反復の番号、w: 合成解像度
    uint4 indices3;
    // x: 最大値 R32_UINT の UAV、y: マスク出力 UAV、z: マスクの種類（0 流量、1 堆積）、
    // w: 解析グリッドの一辺（最終解像度）
    uint4 indices4;
    // SRV: x: 作業ハイト、y: 元の高さ、z: 流量、w: 堆積
    uint4 indices5;
    // x: セルの大きさ（m）、y: 慣性、z: 容量係数、w: 最小傾斜
    float4 params0;
    // x: 侵食率、y: 堆積率、z: 1 歩の蒸発係数、w: 重力
    float4 params1;
    // x: 1 反復で 1 セルが動ける上限（m）、y: 端のフェード（セル）、z: 標高差（m）、
    // w: Height へ書くか（1 / 0）
    float4 params2;
};

ConstantBuffer<DropletConstants> g_droplet : register(b1);

// 固定小数点の刻み（1 m あたり）。int の範囲で ±500 km まで持てる。
static const float kDropletFixedScale = 4096.0f;

uint LevelN() { return g_droplet.indices2.x; }
uint SourceN() { return g_droplet.indices2.y; }
uint FinalN() { return g_droplet.indices4.w; }

bool OutsideLevel(uint2 cell)
{
    const uint n = LevelN();
    return cell.x >= n || cell.y >= n;
}

// --- 乱数（terrain-editor の GPU 版と同じ。同じシードで同じ結果） --------------------

uint SeedFor(int seedValue, int levelSeed, int iteration, int particle)
{
    uint h = uint(seedValue) * 2654435761u;
    h = (h ^ uint(levelSeed + 1)) * 2246822519u;
    h = (h ^ uint(iteration + 1)) * 3266489917u;
    h = (h ^ uint(particle)) * 668265263u;
    return h ^ (h >> 15);
}

float DropletHash01(inout uint state)
{
    state += 0x9e3779b9u;
    uint z = state;
    z = (z ^ (z >> 16)) * 0x21f0aaadu;
    z = (z ^ (z >> 15)) * 0x735a2d97u;
    z = z ^ (z >> 15);
    return float(z >> 8) * (1.0f / 16777216.0f);
}

// --- 双線形の読み / splat ------------------------------------------------------

void BilinearCorners(float px, float pz, uint n, out int2 c0, out int2 c1, out float u, out float v)
{
    const int last = int(n) - 1;
    c0.x = clamp(int(floor(px)), 0, last);
    c0.y = clamp(int(floor(pz)), 0, last);
    c1.x = min(c0.x + 1, last);
    c1.y = min(c0.y + 1, last);
    u = px - float(c0.x);
    v = pz - float(c0.y);
}

// 作業ハイトの双線形の値と、その解析的な勾配（セルあたりの m）。
void SampleHeightGradient(float px, float pz, out float height, out float gradX, out float gradZ)
{
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_droplet.indices0.x];
    int2 c0, c1;
    float u, v;
    BilinearCorners(px, pz, LevelN(), c0, c1, u, v);
    const float nw = heights[uint2(c0.x, c0.y)];
    const float ne = heights[uint2(c1.x, c0.y)];
    const float sw = heights[uint2(c0.x, c1.y)];
    const float se = heights[uint2(c1.x, c1.y)];
    gradX = lerp(ne - nw, se - sw, v);
    gradZ = lerp(sw - nw, se - ne, u);
    height = lerp(lerp(nw, ne, u), lerp(sw, se, u), v);
}

int ToFixed(float meters)
{
    return int(meters * kDropletFixedScale + (meters >= 0.0f ? 0.5f : -0.5f));
}

void SplatFixed(uint uavIndex, float px, float pz, float amount)
{
    RWTexture2D<int> target = ResourceDescriptorHeap[uavIndex];
    int2 c0, c1;
    float u, v;
    BilinearCorners(px, pz, LevelN(), c0, c1, u, v);
    int previous;
    InterlockedAdd(target[uint2(c0.x, c0.y)], ToFixed(amount * (1.0f - u) * (1.0f - v)), previous);
    InterlockedAdd(target[uint2(c1.x, c0.y)], ToFixed(amount * u * (1.0f - v)), previous);
    InterlockedAdd(target[uint2(c0.x, c1.y)], ToFixed(amount * (1.0f - u) * v), previous);
    InterlockedAdd(target[uint2(c1.x, c1.y)], ToFixed(amount * u * v), previous);
}

// --- 解析グリッドへ落とす / レベル間の拡大 ------------------------------------------

[numthreads(8, 8, 1)]
void CsOriginal(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    const uint n = FinalN();
    if (cell.x >= n || cell.y >= n) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_droplet.indices1.z];
    RWTexture2D<float> original = ResourceDescriptorHeap[g_droplet.indices0.z];
    // セルの平均で落とす（間引くと材質の凹凸がエイリアスする。堆積と同じ）。
    original[cell] = DownsampleHeight(source, cell, n) * g_droplet.params2.z;
}

[numthreads(8, 8, 1)]
void CsInit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideLevel(cell)) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_droplet.indices1.z];
    RWTexture2D<float> heights = ResourceDescriptorHeap[g_droplet.indices0.x];
    heights[cell] = DownsampleHeight(source, cell, LevelN()) * g_droplet.params2.z;
}

[numthreads(8, 8, 1)]
void CsCopyToSrc(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    const uint n = SourceN();
    if (cell.x >= n || cell.y >= n) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_droplet.indices0.x];
    RWTexture2D<float> source = ResourceDescriptorHeap[g_droplet.indices0.y];
    source[cell] = heights[cell];
}

[numthreads(8, 8, 1)]
void CsUpsample(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideLevel(cell)) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_droplet.indices0.x];
    RWTexture2D<float> source = ResourceDescriptorHeap[g_droplet.indices0.y];
    const uint n = LevelN();
    const uint srcN = SourceN();
    // セル中心どうしを対応させる。
    const float sx = (float(cell.x) + 0.5f) * float(srcN) / float(n) - 0.5f;
    const float sz = (float(cell.y) + 0.5f) * float(srcN) / float(n) - 0.5f;
    int2 c0, c1;
    float u, v;
    BilinearCorners(sx, sz, srcN, c0, c1, u, v);
    const float nw = source[uint2(c0.x, c0.y)];
    const float ne = source[uint2(c1.x, c0.y)];
    const float sw = source[uint2(c0.x, c1.y)];
    const float se = source[uint2(c1.x, c1.y)];
    heights[cell] = lerp(lerp(nw, ne, u), lerp(sw, se, u), v);
}

// --- 反復の準備 -------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CsClearLevel(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideLevel(cell)) { return; }

    RWTexture2D<int> flow = ResourceDescriptorHeap[g_droplet.indices1.x];
    RWTexture2D<int> deposit = ResourceDescriptorHeap[g_droplet.indices1.y];
    flow[cell] = 0;
    deposit[cell] = 0;
}

[numthreads(8, 8, 1)]
void CsClearIter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideLevel(cell)) { return; }

    RWTexture2D<int> delta = ResourceDescriptorHeap[g_droplet.indices0.w];
    delta[cell] = 0;
}

// --- 水滴 1 つ（CPU 版の内側のループの移植） -----------------------------------------

[numthreads(64, 1, 1)]
void CsTrace(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particle = dispatchThreadId.x;
    if (particle >= g_droplet.indices2.z) { return; }

    const uint n = LevelN();
    const uint steps = g_droplet.indices2.w;
    const float cellMeters = g_droplet.params0.x;
    const float inertia = g_droplet.params0.y;
    const float capacityFactor = g_droplet.params0.z;
    const float minSlope = g_droplet.params0.w;
    const float erodeRate = g_droplet.params1.x;
    const float depositRate = g_droplet.params1.y;
    const float evapStepFactor = g_droplet.params1.z;
    const float gravity = g_droplet.params1.w;
    const float edgeFadeCells = g_droplet.params2.y;

    uint rng = SeedFor(int(g_droplet.indices3.x), int(g_droplet.indices3.y),
                       int(g_droplet.indices3.z), int(particle));
    // 始点は [1, n − 2]（端の 1 セルは使わない）。
    const float spawnRange = float(int(n) - 3);
    float px = 1.0f + DropletHash01(rng) * spawnRange;
    float pz = 1.0f + DropletHash01(rng) * spawnRange;
    float dirX = 0.0f;
    float dirZ = 0.0f;
    float speed = 1.0f;
    float water = 1.0f;
    float sediment = 0.0f;
    const float last = float(int(n) - 2);

    [loop]
    for (uint stepIndex = 0; stepIndex < steps; ++stepIndex)
    {
        float h, gx, gz;
        SampleHeightGradient(px, pz, h, gx, gz);

        dirX = dirX * inertia - gx * (1.0f - inertia);
        dirZ = dirZ * inertia - gz * (1.0f - inertia);
        const float dirLength = sqrt(dirX * dirX + dirZ * dirZ);
        if (dirLength < 1e-6f) { break; }
        dirX /= dirLength;
        dirZ /= dirLength;

        const float npx = px + dirX;
        const float npz = pz + dirZ;
        if (npx < 1.0f || npx > last || npz < 1.0f || npz > last) { break; }

        float nh, ngx, ngz;
        SampleHeightGradient(npx, npz, nh, ngx, ngz);
        const float dH = nh - h;

        {
            RWTexture2D<int> flow = ResourceDescriptorHeap[g_droplet.indices1.x];
            int previous;
            InterlockedAdd(flow[uint2(uint(px), uint(pz))], ToFixed(water), previous);
        }

        // 端に近い所では削り / 堆積を弱める。外へ出る水滴は全部端で終わるので、
        // 出口のセルが毎回削られて針のような切れ込みになるのを防ぐ。
        const float edgeDistance = min(min(px, pz), min(float(int(n) - 1) - px, float(int(n) - 1) - pz));
        const float edgeFade = saturate((edgeDistance - 1.0f) / edgeFadeCells);

        // 容量は実際の傾斜（落差 / 距離）で決める（解像度に依らない）。
        const float slope = -dH / cellMeters;
        const float capacity = max(slope, minSlope) * speed * water * capacityFactor;

        if (dH > 0.0f)
        {
            // 上りにぶつかった。いる所の窪みを埋めて先へ進めるようにする。
            const float deposit = min(dH, sediment);
            sediment -= deposit;
            SplatFixed(g_droplet.indices0.w, px, pz, deposit);
            SplatFixed(g_droplet.indices1.y, px, pz, deposit);
        }
        else if (sediment > capacity)
        {
            const float deposit = (sediment - capacity) * depositRate * edgeFade;
            sediment -= deposit;
            SplatFixed(g_droplet.indices0.w, px, pz, deposit);
            SplatFixed(g_droplet.indices1.y, px, pz, deposit);
        }
        else
        {
            const float erode = min((capacity - sediment) * erodeRate, -dH) * edgeFade;
            SplatFixed(g_droplet.indices0.w, px, pz, -erode);
            sediment += erode;
        }

        speed = sqrt(max(0.0f, speed * speed + (-dH) * gravity));
        water *= evapStepFactor;
        if (water < 1e-4f) { break; }

        px = npx;
        pz = npz;
    }
}

// --- 差分を足す（飽和つき） -----------------------------------------------------------

[numthreads(8, 8, 1)]
void CsApply(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideLevel(cell)) { return; }

    RWTexture2D<float> heights = ResourceDescriptorHeap[g_droplet.indices0.x];
    RWTexture2D<int> delta = ResourceDescriptorHeap[g_droplet.indices0.w];
    const float sum = float(delta[cell]) / kDropletFixedScale;
    if (sum != 0.0f)
    {
        // 多くの水滴が通ったセルほど動く（水系が育つフィードバック）が、
        // 1 反復に deltaCap 以上は動かない（重なった水滴が針を立てない）。
        const float cap = g_droplet.params2.x;
        heights[cell] += sum / (1.0f + abs(sum) / cap);
    }
}

// --- 合成解像度へ足し戻す ---------------------------------------------------------------

[numthreads(8, 8, 1)]
void CsResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_droplet.indices3.w;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> heights = ResourceDescriptorHeap[g_droplet.indices5.x];
    Texture2D<float> original = ResourceDescriptorHeap[g_droplet.indices5.y];
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_droplet.indices1.w];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    const float delta = heights.SampleLevel(g_samplerLinearClamp, uv, 0.0f) -
                        original.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    heightTarget[texel] = saturate(heightTarget[texel] + delta / g_droplet.params2.z);
}

// --- マスク（流量 / 堆積） -----------------------------------------------------------

// セルの値。流量は裾が長いので log で圧縮する（terrain-editor と同じ）。
float DropletMaskCell(uint2 cell)
{
    Texture2D<int> flow = ResourceDescriptorHeap[g_droplet.indices5.z];
    Texture2D<int> deposit = ResourceDescriptorHeap[g_droplet.indices5.w];
    if (g_droplet.indices4.z == 0u)
    {
        const float value = float(flow.Load(int3(cell, 0))) / kDropletFixedScale;
        return log(1.0f + max(value, 0.0f));
    }
    return max(float(deposit.Load(int3(cell, 0))) / kDropletFixedScale, 0.0f);
}

[numthreads(1, 1, 1)]
void CsMaskMaxClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_droplet.indices4.x];
    maxScratch[uint2(0, 0)] = 0u;
}

[numthreads(8, 8, 1)]
void CsMaskMaxReduce(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    const uint n = FinalN();
    if (cell.x >= n || cell.y >= n) { return; }

    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_droplet.indices4.x];
    uint previous;
    // 非負の float は uint として大小が保たれる。
    InterlockedMax(maxScratch[uint2(0, 0)], asuint(DropletMaskCell(cell)), previous);
}

[numthreads(8, 8, 1)]
void CsMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // マスクは合成解像度で出す。値は解析グリッドから双線形で引く。
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_droplet.indices3.w;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_droplet.indices4.x];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_droplet.indices4.y];

    const uint n = FinalN();
    const float sx = (float(texel.x) + 0.5f) * float(n) / float(resolution) - 0.5f;
    const float sz = (float(texel.y) + 0.5f) * float(n) / float(resolution) - 0.5f;
    int2 c0, c1;
    float u, v;
    BilinearCorners(sx, sz, n, c0, c1, u, v);
    const float nw = DropletMaskCell(uint2(c0.x, c0.y));
    const float ne = DropletMaskCell(uint2(c1.x, c0.y));
    const float sw = DropletMaskCell(uint2(c0.x, c1.y));
    const float se = DropletMaskCell(uint2(c1.x, c1.y));
    const float value = lerp(lerp(nw, ne, u), lerp(sw, se, u), v);
    const float denominator = max(asfloat(maxScratch[uint2(0, 0)]), 1e-6f);
    mask[texel] = saturate(value / denominator);
}
