// 合成結果を、書き出す形に詰め直す。
//
// 合成の中間表現（R11G11B10_FLOAT のベースカラー、xy だけの法線、
// R8G8B8A8 のサーフェス、R16_FLOAT のハイト）は、そのままではファイルに書けない。
// **形式の変換とチャンネルの詰め替えは GPU 側でまとめてやる。**
// CPU へ落とすのは「そのまま並べれば PNG になるバイト列」だけにするため。

#include "CompositeCommon.hlsli"

// 出力の種類。C++ 側の io::ExportMap と並びを合わせること。
#define TG_EXPORT_BASE_COLOR 0
#define TG_EXPORT_NORMAL     1
#define TG_EXPORT_ROUGHNESS  2
#define TG_EXPORT_METALLIC   3
#define TG_EXPORT_AO         4
#define TG_EXPORT_HEIGHT     5
#define TG_EXPORT_ORD        6  // AO / Roughness / Height
#define TG_EXPORT_ORM        7  // AO / Roughness / Metallic

struct ExportConstants
{
    uint outputIndex;   // RGBA8 の UAV
    uint baseColorIndex;
    uint normalIndex;
    uint surfaceIndex;

    uint heightIndex;
    uint resolution;
    uint map;           // TG_EXPORT_*
    uint pad0;
};

ConstantBuffer<ExportConstants> g_export : register(b0);

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_export.resolution || dispatchThreadId.y >= g_export.resolution)
    {
        return;
    }

    RWTexture2D<float4> output = ResourceDescriptorHeap[g_export.outputIndex];
    const uint2 texel = dispatchThreadId.xy;

    float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f);

    switch (g_export.map)
    {
        case TG_EXPORT_BASE_COLOR:
        {
            Texture2D<float4> baseColor = ResourceDescriptorHeap[g_export.baseColorIndex];
            // 合成はリニアで持っている。**画像には sRGB で書く。**
            // リニアのまま 8bit へ落とすと暗部の段差が目に見える。
            result.rgb = LinearToSrgb(saturate(baseColor[texel].rgb));
            break;
        }
        case TG_EXPORT_NORMAL:
        {
            Texture2D<float2> normalMap = ResourceDescriptorHeap[g_export.normalIndex];
            // 合成は xy だけを持ち、z は都度再構成している。書き出しでは
            // 一般的な接空間法線マップの並び（0.5 が平坦）へ直す。
            const float3 normal = DecodeTangentNormal(normalMap[texel]);
            result.rgb = normal * 0.5f + 0.5f;
            break;
        }
        case TG_EXPORT_HEIGHT:
        {
            Texture2D<float> height = ResourceDescriptorHeap[g_export.heightIndex];
            // 8bit へ落とす経路。段差が問題になる用途では EXR 側を使う。
            result.rgb = saturate(height[texel]).xxx;
            break;
        }
        default:
        {
            Texture2D<float4> surface = ResourceDescriptorHeap[g_export.surfaceIndex];
            const float3 packed = surface[texel].rgb;  // R=Roughness, G=Metallic, B=AO
            switch (g_export.map)
            {
                case TG_EXPORT_ROUGHNESS: result.rgb = packed.rrr; break;
                case TG_EXPORT_METALLIC:  result.rgb = packed.ggg; break;
                case TG_EXPORT_AO:        result.rgb = packed.bbb; break;
                case TG_EXPORT_ORD:
                {
                    Texture2D<float> height = ResourceDescriptorHeap[g_export.heightIndex];
                    // Megascans の並び。AO / Roughness / Height。
                    result.rgb = float3(packed.b, packed.r, saturate(height[texel]));
                    break;
                }
                default:
                    // ORM（glTF / Unreal）。AO / Roughness / Metallic。
                    result.rgb = float3(packed.b, packed.r, packed.g);
                    break;
            }
            break;
        }
    }

    output[texel] = result;
}
