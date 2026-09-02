// 堆積（Sediment）。terrain-editor の同名ノードを移植したもの。
//
// **可動な土砂**を重力で再分配する。安息角（talus）を超えた斜面から
// 低い隣のセルへ滑らせるのを何度も繰り返すと、谷底に厚く積もり、
// 尾根が痩せた樹枝状の堆積になる（GeoGen 風）。
//
// 1 回の「滑らせ」は**競合しないよう 2 掃引に分ける**。
//   CsSweep1: 各セルが 4 近傍へ出す量を outgoing（方向ごと）へ書く
//   CsSweep2: 自分の流出と、4 近傍が自分へ向けて出した量を差し引きする
// これで同じセルへの同時書き込みが起きない。
//
// 方向の並びは terrain-editor と同じ 0:東 1:西 2:南 3:北。逆向きは {1,0,3,2}。
//
// **合成の Height とは別のグリッドで回す。** 反復回数がそのまま効くので、
// 形が決まる粗さで十分（結果は差分として合成解像度へ足し戻す）。

#include "CompositeCommon.hlsli"

struct SedimentConstants
{
    // 書き込み（UAV）: 基盤、土砂、流出（RGBA = 東西南北）、元の高さ
    uint4 indices0;
    // 読み取り（SRV）: 基盤、土砂、元の高さ、合成の Height
    uint4 indices1;
    // x: グリッドの一辺、y: 地形を土砂として扱うか、
    // z: 合成の Height の UAV、w: 合成解像度
    uint4 indices2;
    // x: 最大値をためる R32_UINT の UAV、y: マスクの出力 UAV、zw: 未使用
    uint4 indices3;
    // x: 安息角ぶんの落差（正規化ハイト）、y: 1 反復あたりの供給量、
    // z: マスクのコントラスト、w: マスクが 1 になる厚み（正規化ハイト。0 なら最大値で正規化）
    float4 params;
};

ConstantBuffer<SedimentConstants> g_sediment : register(b1);

static const int2 kSedimentOffset[4] = {
    int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
};
static const uint kSedimentOpposite[4] = {1u, 0u, 3u, 2u};

uint SedimentResolution() { return g_sediment.indices2.x; }

bool OutsideSediment(uint2 cell)
{
    const uint resolution = SedimentResolution();
    return cell.x >= resolution || cell.y >= resolution;
}

float LoadOutgoing(uint2 cell, uint direction)
{
    RWTexture2D<float4> outgoing = ResourceDescriptorHeap[g_sediment.indices0.z];
    const float4 packed = outgoing[cell];
    if (direction == 0u) { return packed.x; }
    if (direction == 1u) { return packed.y; }
    if (direction == 2u) { return packed.z; }
    return packed.w;
}

// 合成の Height を解析用グリッドへ落とし、基盤と土砂に分ける。
[numthreads(8, 8, 1)]
void CsSetup(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSediment(cell)) { return; }

    Texture2D<float> source = ResourceDescriptorHeap[g_sediment.indices1.w];
    RWTexture2D<float> bedrock = ResourceDescriptorHeap[g_sediment.indices0.x];
    RWTexture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices0.y];
    RWTexture2D<float> original = ResourceDescriptorHeap[g_sediment.indices0.w];

    const float2 uv = (float2(cell) + 0.5f) / float(SedimentResolution());
    const float height = source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);

    // **足し戻すのは差分**なので、始まりの高さを覚えておく。
    original[cell] = height;
    if (g_sediment.indices2.y != 0u)
    {
        // 地形そのものを可動な土砂として扱う。基盤は平らな 0。
        bedrock[cell] = 0.0f;
        sediment[cell] = height;
    }
    else
    {
        // 入力は動かない基盤。供給したぶんだけが流れる。
        bedrock[cell] = height;
        sediment[cell] = 0.0f;
    }
}

// 供給。全セルへ一様に土砂を積む。
[numthreads(8, 8, 1)]
void CsEmit(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSediment(cell)) { return; }
    RWTexture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices0.y];
    sediment[cell] += g_sediment.params.y;
}

// 掃引 1。各セルが 4 近傍へ出す量を決める。
//
// 落差が安息角ぶん（talusH）を超えたぶんだけが動く。**割る数は「流す先 + 1」**で、
// 1 回の適用で傾きが talusH へ収まる（terrain-editor と同じ式）。
// 手持ちの土砂が足りなければ、全方向を同じ比率で減らす。
[numthreads(8, 8, 1)]
void CsSweep1(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSediment(cell)) { return; }

    RWTexture2D<float> bedrock = ResourceDescriptorHeap[g_sediment.indices0.x];
    RWTexture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices0.y];
    RWTexture2D<float4> outgoing = ResourceDescriptorHeap[g_sediment.indices0.z];

    const int resolution = int(SedimentResolution());
    const float height = bedrock[cell] + sediment[cell];
    const float talus = g_sediment.params.x;

    float drops[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float totalDrop = 0.0f;
    int activeCount = 0;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        const int2 neighbour = int2(cell) + kSedimentOffset[k];
        if (neighbour.x < 0 || neighbour.x >= resolution || neighbour.y < 0 ||
            neighbour.y >= resolution)
        {
            continue;
        }
        const uint2 target = uint2(neighbour);
        const float difference = height - bedrock[target] - sediment[target];
        if (difference > talus)
        {
            drops[k] = difference - talus;
            totalDrop += drops[k];
            ++activeCount;
        }
    }

    if (activeCount == 0 || totalDrop <= 0.0f)
    {
        outgoing[cell] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float divisor = float(activeCount + 1);
    const float ideal = totalDrop / divisor;
    const float available = max(sediment[cell], 0.0f);
    const float actual = min(available, ideal);
    const float scale = (ideal > 0.0f) ? (actual / ideal) : 0.0f;

    outgoing[cell] = float4(drops[0], drops[1], drops[2], drops[3]) * (scale / divisor);
}

// 掃引 2。自分の流出を引き、4 近傍が自分へ向けて出したぶんを足す。
[numthreads(8, 8, 1)]
void CsSweep2(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSediment(cell)) { return; }

    RWTexture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices0.y];
    RWTexture2D<float4> outgoing = ResourceDescriptorHeap[g_sediment.indices0.z];

    const int resolution = int(SedimentResolution());
    const float4 own = outgoing[cell];
    const float total = own.x + own.y + own.z + own.w;

    float incoming = 0.0f;
    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        const int2 neighbour = int2(cell) + kSedimentOffset[k];
        if (neighbour.x < 0 || neighbour.x >= resolution || neighbour.y < 0 ||
            neighbour.y >= resolution)
        {
            continue;
        }
        incoming += LoadOutgoing(uint2(neighbour), kSedimentOpposite[k]);
    }

    sediment[cell] = max(0.0f, sediment[cell] - total + incoming);
}

// 合成の Height へ**差分だけ**足し戻す。
// 解析グリッドは粗いので、そのまま置き換えると合成解像度の細部が消える。
// 動いたぶん（基盤 + 土砂 − 元の高さ）だけを足せば、細部はそのまま残る。
//
// **ただし積もった所では細部を埋める。** 土砂が積もっているのに下の凸凹が
// そのまま出ていると、砂利の上に泥を塗っただけに見える。厚み d だけ積もった
// なら、細部の振れ幅を d だけ 0 へ寄せる（|細部| − d、下限 0）。
//
//   - 出っ張りの**頂上は動かない**（周りが d 上がるので、相対的に d 低くなる）。
//   - 窪みは埋まる。
//   - 厚みが細部の振れ幅を超えたら、完全に平ら（＝解析グリッドの面）になる。
//
// 出っ張りは何ももらわず窪みが 2d もらう形になるので、細部の分布が上下対称なら
// 平均は d のままで、体積の帳尻は合う。パラメータを持たないのはこのため。
[numthreads(8, 8, 1)]
void CsApply(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // ここだけは合成解像度で回す。
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_sediment.indices2.w;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<float> bedrock = ResourceDescriptorHeap[g_sediment.indices1.x];
    Texture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices1.y];
    Texture2D<float> original = ResourceDescriptorHeap[g_sediment.indices1.z];
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_sediment.indices2.z];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    const float coarseOriginal = original.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float coarseResult = bedrock.SampleLevel(g_samplerLinearClamp, uv, 0.0f) +
                               sediment.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    const float delta = coarseResult - coarseOriginal;

    // 合成解像度でしか持っていない細部（粗いグリッドとの差）。
    const float detail = heightTarget[texel] - coarseOriginal;
    // 積もった厚み。削れた所は 0（埋めるものが無い）。
    const float deposit = max(delta, 0.0f);
    const float buried = sign(detail) * max(abs(detail) - deposit, 0.0f);

    heightTarget[texel] = saturate(coarseOriginal + delta + buried);
}

// --- 厚みをマスクにする -----------------------------------------------------
//
// 積もった土砂の厚みを 0〜1 のマスクにする。既定は**実寸で正規化**し、
// 基準の厚みが 0 のときだけ「一番厚い所で 1」に落とす。
// 非負の float はビット列の大小が uint と同じ順序なので、そのまま
// InterlockedMax に掛けられる（川筋の最大値集計と同じ手）。
//
// **厚み = 基盤 + 土砂 − 元の高さ**（削れた所は 0）。土砂のテクスチャを
// そのまま読むと、「地形を土砂にする」が入のときに**地形の高さそのもの**に
// なってしまう（あの設定では土砂 = 動かせる地形なので）。

// 解析グリッドの 1 セルに積もった厚み。
float SedimentDeposit(uint2 cell)
{
    Texture2D<float> bedrock = ResourceDescriptorHeap[g_sediment.indices1.x];
    Texture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices1.y];
    Texture2D<float> original = ResourceDescriptorHeap[g_sediment.indices1.z];
    const int3 at = int3(int2(cell), 0);
    return max(bedrock.Load(at) + sediment.Load(at) - original.Load(at), 0.0f);
}

// 同じものを UV で補間して引く。厚みは 3 枚の**線形な足し引き**なので、
// 1 枚ずつ補間してから引いても、引いてから補間しても同じ値になる。
// マスクは合成解像度で出すため、こちらを使わないとグリッドの目が出る。
float SedimentDepositAt(float2 uv)
{
    Texture2D<float> bedrock = ResourceDescriptorHeap[g_sediment.indices1.x];
    Texture2D<float> sediment = ResourceDescriptorHeap[g_sediment.indices1.y];
    Texture2D<float> original = ResourceDescriptorHeap[g_sediment.indices1.z];
    return max(bedrock.SampleLevel(g_samplerLinearClamp, uv, 0.0f) +
                   sediment.SampleLevel(g_samplerLinearClamp, uv, 0.0f) -
                   original.SampleLevel(g_samplerLinearClamp, uv, 0.0f),
               0.0f);
}

[numthreads(1, 1, 1)]
void CsMaskMaxClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_sediment.indices3.x];
    maxScratch[uint2(0, 0)] = 0u;
}

[numthreads(8, 8, 1)]
void CsMaskMaxReduce(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 cell = dispatchThreadId.xy;
    if (OutsideSediment(cell)) { return; }

    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_sediment.indices3.x];

    uint previous;
    InterlockedMax(maxScratch[uint2(0, 0)], asuint(SedimentDeposit(cell)), previous);
}

[numthreads(8, 8, 1)]
void CsMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // マスクは合成解像度で出す。厚みは大きなスケールの値なので、
    // 解析グリッドから UV で引いて構わない。
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = g_sediment.indices2.w;
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<uint> maxScratch = ResourceDescriptorHeap[g_sediment.indices3.x];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_sediment.indices3.y];

    const float2 uv = (float2(texel) + 0.5f) / float(resolution);
    const float thickness = SedimentDepositAt(uv);

    // **基準の厚みが与えられていれば実寸で正規化する。**
    // 0 のときだけ「一番厚い所で 1」に落とす（少数の分厚い点が基準になるので、
    // 残りが 0 付近へ潰れる。マスクとしては使いにくい）。
    const float reference = g_sediment.params.w;
    const float denominator =
        (reference > 0.0f) ? reference : max(asfloat(maxScratch[uint2(0, 0)]), 1e-6f);

    float value = saturate(thickness / denominator);
    // コントラスト。0 で線形、上げるほど「積もった / 積もっていない」が分かれる。
    value = ApplyMaskCurve(value, 1.0f + saturate(g_sediment.params.z) * 7.0f);
    mask[texel] = value;
}
