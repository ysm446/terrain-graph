// ペイントマスクを一様な値で塗りつぶす。新規作成時の初期化と、全消去 / 全塗りに使う。
//
// ClearUnorderedAccessViewFloat はシェーダ非可視のディスクリプタを別途要求するため、
// bindless 前提の本プロジェクトではコンピュートで書くほうが素直になる。

#include "Common.hlsli"

struct FillConstants
{
    uint outputIndex;
    uint width;
    uint height;
    float value;
};

ConstantBuffer<FillConstants> g_fill : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_fill.width || dispatchThreadId.y >= g_fill.height)
    {
        return;
    }

    RWTexture2D<float> output = ResourceDescriptorHeap[g_fill.outputIndex];
    output[dispatchThreadId.xy] = g_fill.value;
}
