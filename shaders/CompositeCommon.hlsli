#ifndef TG_COMPOSITE_COMMON_HLSLI
#define TG_COMPOSITE_COMMON_HLSLI

#include "Common.hlsli"

// 合成の出力チャンネル。
//   BaseColor : R11G11B10_FLOAT   リニア色
//   Normal    : R16G16_FLOAT      タンジェント空間法線の xy（z は再構成）
//   Surface   : R8G8B8A8_UNORM    R=Roughness, G=Metallic, B=AO
//   Height    : R16_FLOAT         高さ（合成の駆動値かつ Displacement）

// 「テクスチャなし」を表す SRV インデックス。C++ 側の kInvalidTextureIndex と揃える。
static const uint kInvalidTextureIndex = 0xFFFFFFFFu;

// スカラーのマップは「テクスチャ + どのチャンネルを読むか」で指定する。
// Megascans の _ORD のように 1 枚へ複数のマップを詰めたテクスチャがあるため。
// C++ 側の TextureChannel と一致させること。
float SelectChannel(float4 value, uint channel)
{
    if (channel == 1u) { return value.g; }
    if (channel == 2u) { return value.b; }
    if (channel == 3u) { return value.a; }
    return value.r;
}

// チャンネル指定は 4bit ずつ 1 つの uint へ詰めて渡す。
// ルート定数の枠を節約するため。スロットの並びは C++ 側と一致させること。
uint UnpackChannel(uint packed, uint slotIndex)
{
    return (packed >> (slotIndex * 4u)) & 0xFu;
}

#define TG_CHANNEL_SLOT_ROUGHNESS 0u
#define TG_CHANNEL_SLOT_METALLIC  1u
#define TG_CHANNEL_SLOT_AO        2u
#define TG_CHANNEL_SLOT_HEIGHT    3u
#define TG_CHANNEL_SLOT_MASK      4u

float3 DecodeTangentNormal(float2 xy)
{
    const float z = sqrt(saturate(1.0f - dot(xy, xy)));
    return float3(xy, z);
}

float2 EncodeTangentNormal(float3 normal)
{
    return normalize(normal).xy;
}

// Reoriented Normal Mapping。
// base に detail を載せた法線を返す。lerp は使わない。
//   detail が平坦 (0,0,1) なら base をそのまま返す
//   base が平坦 (0,0,1) なら detail をそのまま返す
float3 ReorientNormal(float3 base, float3 detail)
{
    const float3 t = base + float3(0.0f, 0.0f, 1.0f);
    const float3 u = detail * float3(-1.0f, -1.0f, 1.0f);
    return normalize(t * (dot(t, u) / max(t.z, 1e-5f)) - u);
}

// 法線を平坦方向へ寄せる。合成の重みに応じて base / detail を弱めるのに使う。
float3 FlattenNormal(float3 normal, float amount)
{
    return normalize(float3(normal.xy * amount, normal.z));
}

// ハイトベースブレンド。
//   下地の重み = 1 - mask、レイヤーの重み = mask として、
//   「高さ + 重み」の大きいほうが上に出る。
//   range を小さくすると硬い置き換え、大きくするとハイトの影響が薄れて
//   マスクによる従来の合成に近づく。
float HeightBlendWeight(float baseHeight, float layerHeight, float mask, float range)
{
    // **高さだけで決めた重み。** 起伏が拮抗する所で 0.5、レイヤーが range 分
    // 勝てば 1、負ければ 0。境界を材質の凹凸なりにぎざぎざさせるのがこの項の役目。
    const float t = saturate(0.5f + (layerHeight - baseHeight) / (2.0f * max(range, 1e-4f)));

    // **マスクは被覆率。** t を上下にずらすことで、高い所から順に出る。
    // mask = 0 なら必ず 0、mask = 1 なら必ず 1（t は 0〜1 なので端は必ず飽和する）。
    //
    // 以前はマスクを高さと同じ土俵（`下地 + (1 - mask)` と `レイヤー + mask`）で
    // 競合させていた。端の値は同じだが、**中間はマスク 0.5 付近の
    // 狭いしきい値として振る舞う**（沿わせたレイヤーでは概ね 0.43〜0.58）。
    // 0〜0.4 しか持たないマスク（堆積の厚みなど）がまったく絵に出ず、
    // 「マスクが効かない」と見えるため、被覆率として素直に効く形へ変えた。
    // シェイプが `weight = mask` なのとも揃う。
    return saturate(t + (mask * 2.0f - 1.0f));
}

// マスクのカーブ。
//   contrast = 1  線形（そのまま）
//   contrast > 1  S 字を強め、0 / 1 に寄せる（境界がはっきりする）
//   contrast < 1  中間へ寄せ、全体をなだらかにする
float ApplyMaskCurve(float value, float contrast)
{
    const float x = saturate(value);

    if (contrast > 1.0f)
    {
        const float smooth = x * x * (3.0f - 2.0f * x);
        return saturate(lerp(x, smooth, saturate(contrast - 1.0f)));
    }
    if (contrast < 1.0f)
    {
        return saturate(lerp(x, 0.5f, saturate(1.0f - contrast)));
    }
    return x;
}

// マスクのレベル調整と反転。
float ApplyMaskLevels(float value, float low, float high, bool invert)
{
    const float range = max(high - low, 1e-4f);
    float result = saturate((value - low) / range);
    return invert ? (1.0f - result) : result;
}

#endif  // TG_COMPOSITE_COMMON_HLSLI
