#ifndef TG_COMMON_HLSLI
#define TG_COMMON_HLSLI

// 全パス共通のルートシグネチャに対応する宣言。
// b0 は各シェーダが自前のルート定数構造体を宣言するため、ここでは定義しない。
//
// リソースは bindless で引く。SRV / UAV はディスクリプタのインデックスを
// ルート定数で受け取り、ResourceDescriptorHeap[index] でアクセスする。

SamplerState g_samplerPointClamp  : register(s0);
SamplerState g_samplerLinearClamp : register(s1);
SamplerState g_samplerLinearWrap  : register(s2);
SamplerState g_samplerAnisoWrap   : register(s3);
// equirectangular 用。経度方向はラップ、天頂方向はクランプ。
SamplerState g_samplerEquirect    : register(s4);

static const float kPi = 3.14159265358979323846f;

float3 SrgbToLinear(float3 c)
{
    return select(c <= 0.04045f, c / 12.92f, pow(abs(c + 0.055f) / 1.055f, 2.4f));
}

float3 LinearToSrgb(float3 c)
{
    return select(c <= 0.0031308f, c * 12.92f, 1.055f * pow(abs(c), 1.0f / 2.4f) - 0.055f);
}

// 決定的なハッシュノイズ。マスク生成の足場として使う。
float Hash21(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031f);
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.x + p3.y) * p3.z);
}

// 合成はタイリング前提（wrap サンプラ、書き出した素材もタイルして使う）なので、
// **ノイズも UV の 0 / 1 の境界で継ぎ目なく巻ける**必要がある。
// 格子の座標を周期で巻いてからハッシュすることで周期化する。
// 周期が整数のときだけ厳密にタイルする（シェーダ側で周波数を丸める）。
float2 WrapCell(float2 cell, float period)
{
    return cell - period * floor(cell / period);
}

float ValueNoise(float2 p, float period)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0f - 2.0f * f);

    float a = Hash21(WrapCell(i + float2(0.0f, 0.0f), period));
    float b = Hash21(WrapCell(i + float2(1.0f, 0.0f), period));
    float c = Hash21(WrapCell(i + float2(0.0f, 1.0f), period));
    float d = Hash21(WrapCell(i + float2(1.0f, 1.0f), period));

    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float Fbm(float2 p, int octaves, float period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < octaves; ++i)
    {
        sum += ValueNoise(p, period) * amplitude;
        p *= 2.0f;
        period *= 2.0f;
        amplitude *= 0.5f;
    }
    return sum;
}

// 尾根状のノイズ。値を折り返すことで稜線が立つ。地形や岩の割れ目に向く。
float RidgedFbm(float2 p, int octaves, float period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < octaves; ++i)
    {
        const float ridge = 1.0f - abs(ValueNoise(p, period) * 2.0f - 1.0f);
        sum += ridge * ridge * amplitude;
        p *= 2.0f;
        period *= 2.0f;
        amplitude *= 0.5f;
    }
    return saturate(sum);
}

float2 Hash22(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
    p3 += dot(p3, p3.yzx + 33.33f);
    return frac((p3.xx + p3.yz) * p3.zy);
}

// Worley（セル）ノイズ。最近傍の特徴点までの距離を返す。
// 石畳や砂利のような塊状のパターンに向く。
float Worley(float2 p, float period)
{
    const float2 cell = floor(p);
    const float2 local = frac(p);

    float nearest = 1e9f;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y);
            const float2 feature = offset + Hash22(WrapCell(cell + offset, period));
            nearest = min(nearest, dot(local - feature, local - feature));
        }
    }
    return saturate(sqrt(nearest));
}

float WorleyFbm(float2 p, int octaves, float period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float weight = 0.0f;
    for (int i = 0; i < octaves; ++i)
    {
        sum += Worley(p, period) * amplitude;
        weight += amplitude;
        p *= 2.0f;
        period *= 2.0f;
        amplitude *= 0.5f;
    }
    return sum / max(weight, 1e-5f);
}

// ノイズの種類。C++ 側の NoiseType と一致させること。
#define TG_NOISE_FBM    0
#define TG_NOISE_RIDGED 1
#define TG_NOISE_WORLEY 2

// uv（0〜1 でタイル 1 枚）にスケールとオフセットを掛けて評価する。
// タイルするよう、実際の周波数は**整数へ丸めた周期**を使う。
// UV スケールが整数でないレイヤーではタイルしない（テクスチャと同じ制約）。
float SampleNoise(uint type, float2 uv, float scale, float offset, int octaves)
{
    const float period = max(round(scale), 1.0f);
    const float2 p = uv * period + offset;
    if (type == TG_NOISE_RIDGED)
    {
        return RidgedFbm(p, octaves, period);
    }
    if (type == TG_NOISE_WORLEY)
    {
        return WorleyFbm(p, octaves, period);
    }
    return Fbm(p, octaves, period);
}

#endif  // TG_COMMON_HLSLI
