#ifndef TG_COMPOSITE_COMMON_HLSLI
#define TG_COMPOSITE_COMMON_HLSLI

#include "Common.hlsli"

// 合成の出力チャンネル。
//   BaseColor : R11G11B10_FLOAT   リニア色
//   Normal    : R16G16_FLOAT      タンジェント空間法線の xy（z は再構成）
//   Surface   : R8G8B8A8_UNORM    R=Roughness, G=Metallic, B=AO
//   Height    : R32_FLOAT         高さ（合成の駆動値かつ Displacement）
//                                 R16 だと標高差 600 m で 1 ULP が約 0.3 m になり、
//                                 合成後の Height から作る法線が階段になる

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

// 合成の Height（正方形）を、解析グリッド（gridResolution^2）の 1 セルへ落とす。
//
// **セルが覆う矩形の平均を取る**（中心 1 点の間引きではない）。合成の Height には
// 材質スケールの凹凸（下地サーフェスのハイトマップ）が入っていて、間引くと
// それが粗いグリッドへエイリアスして、堆積 / 積雪 / 川筋のマスクに
// 「見えているハイトと一致しない細かい模様」として出る。
// グリッドのほうが細かい（平均する相手が無い）ときは線形補間で拾う。
// 矩形の中心は (cell + 0.5) × 比 で、間引いていた頃のサンプル位置と同じ。
float DownsampleHeight(Texture2D<float> source, uint2 cell, uint gridResolution)
{
    uint sourceWidth = 0u;
    uint sourceHeight = 0u;
    source.GetDimensions(sourceWidth, sourceHeight);

    if (gridResolution >= sourceWidth)
    {
        const float2 uv = (float2(cell) + 0.5f) / float(gridResolution);
        return source.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
    }

    const float ratio = float(sourceWidth) / float(gridResolution);
    const int2 begin = int2(floor(float2(cell) * ratio));
    int2 end = int2(floor(float2(cell + 1u) * ratio));
    end = max(end, begin + int2(1, 1));
    end = min(end, int2(sourceWidth, sourceHeight));

    float sum = 0.0f;
    [loop]
    for (int y = begin.y; y < end.y; ++y)
    {
        [loop]
        for (int x = begin.x; x < end.x; ++x)
        {
            sum += source.Load(int3(x, y, 0));
        }
    }
    const int2 extent = end - begin;
    return sum / float(max(extent.x * extent.y, 1));
}

// ベースカラーの色相と彩度を調整する。**合成もサムネイルも球のプレビューも
// 必ずこの関数を通すこと。** 別々に書くと、プレビューと本番で色が違うという
// 一番たちの悪い壊れ方をする。
//
// **リニア空間のまま扱う。** sRGB へ往復する HSV 変換を合成の途中へ挟むと、
// 暗部の色が壊れる（このアプリの合成はリニアで回している）。
//   色相: 灰色の軸（1,1,1）まわりの回転。灰色は灰色のまま残る
//   彩度: 輝度（Rec.709）へ寄せる / 離す。1 でそのまま、0 で無彩色
//
// 回転はわずかに負の成分を作ることがあるので、最後に 0 で止める。
float3 AdjustBaseColor(float3 color, float hueRadians, float saturation)
{
    if (hueRadians != 0.0f)
    {
        const float3 axis = float3(0.57735027f, 0.57735027f, 0.57735027f);  // 1 / sqrt(3)
        const float cosHue = cos(hueRadians);
        const float sinHue = sin(hueRadians);
        // ロドリゲスの回転。灰色の軸まわりに回すので、明るさはほぼ保たれる。
        color = color * cosHue + cross(axis, color) * sinHue +
                axis * dot(axis, color) * (1.0f - cosHue);
    }
    if (saturation != 1.0f)
    {
        const float luma = dot(color, float3(0.2126f, 0.7152f, 0.0722f));
        color = lerp(float3(luma, luma, luma), color, saturation);
    }
    return max(color, 0.0f);
}

#endif  // TG_COMPOSITE_COMMON_HLSLI
