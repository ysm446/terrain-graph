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
SamplerState g_samplerAnisoClamp  : register(s5);

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

// --- 勾配ノイズ（Perlin）-----------------------------------------------
// 値ノイズ（格子点の値を補間）に対し、こちらは**格子点の傾きを補間する**。
// 同じ周波数でも起伏が滑らかで、方向のあるうねりになる。
// 格子の座標は値ノイズと同じく周期で巻くので、そのままタイルする。

// 格子点の勾配。ハッシュから単位ベクトルを作る。
float2 NoiseGradient(float2 cell, float period)
{
    const float angle = Hash21(WrapCell(cell, period)) * 6.28318530718f;
    return float2(cos(angle), sin(angle));
}

// おおむね -1〜1。補間は quintic（2 階微分まで連続。格子が目立たない）。
float PerlinNoise(float2 p, float period)
{
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);

    const float a = dot(NoiseGradient(i + float2(0.0f, 0.0f), period), f - float2(0.0f, 0.0f));
    const float b = dot(NoiseGradient(i + float2(1.0f, 0.0f), period), f - float2(1.0f, 0.0f));
    const float c = dot(NoiseGradient(i + float2(0.0f, 1.0f), period), f - float2(0.0f, 1.0f));
    const float d = dot(NoiseGradient(i + float2(1.0f, 1.0f), period), f - float2(1.0f, 1.0f));

    // 2 次元の勾配ノイズは最大でも 1 / sqrt(2) 程度にしかならないので、伸ばして返す。
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y) * 1.41421356f;
}

float PerlinFbm(float2 p, int octaves, float period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < octaves; ++i)
    {
        sum += PerlinNoise(p, period) * amplitude;
        p *= 2.0f;
        period *= 2.0f;
        amplitude *= 0.5f;
    }
    // -1〜1 を 0〜1 へ。
    return saturate(sum * 0.5f + 0.5f);
}

// 雲状（billow）。勾配ノイズの絶対値を積む。丸い塊が寄り集まった見た目になり、
// 雲や苔、風化した岩肌に向く。**尾根状の裏返し**にあたる。
float BillowFbm(float2 p, int octaves, float period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float weight = 0.0f;
    for (int i = 0; i < octaves; ++i)
    {
        sum += abs(PerlinNoise(p, period)) * amplitude;
        weight += amplitude;
        p *= 2.0f;
        period *= 2.0f;
        amplitude *= 0.5f;
    }
    return saturate(sum / max(weight, 1e-5f));
}

// 割れ目（Worley の F2 − F1）。**セルの境目**が明るくなる。
// 乾いた泥のひび、岩の割れ目、石畳の目地に向く（セル状は塊そのものを出す）。
float WorleyEdge(float2 p, float period)
{
    const float2 cell = floor(p);
    const float2 local = frac(p);

    float nearest = 1e9f;
    float second = 1e9f;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(x, y);
            const float2 feature = offset + Hash22(WrapCell(cell + offset, period));
            const float distance = dot(local - feature, local - feature);
            // 1 番目と 2 番目を同時に更新する。**順序を間違えると 2 番目が壊れる。**
            second = min(second, max(distance, nearest));
            nearest = min(nearest, distance);
        }
    }
    // 差が 0（＝境目）で 1 になるよう反転する。1.5 は線の太さの目安。
    return saturate(1.0f - (sqrt(second) - sqrt(nearest)) * 1.5f);
}

float WorleyEdgeFbm(float2 p, int octaves, float period)
{
    float sum = 0.0f;
    float amplitude = 0.5f;
    float weight = 0.0f;
    for (int i = 0; i < octaves; ++i)
    {
        sum += WorleyEdge(p, period) * amplitude;
        weight += amplitude;
        p *= 2.0f;
        period *= 2.0f;
        amplitude *= 0.5f;
    }
    return saturate(sum / max(weight, 1e-5f));
}

// ノイズの種類。C++ 側の NoiseType と一致させること。
// **並びを変えない。** 保存したプロジェクトの見た目が変わる。
#define TG_NOISE_FBM    0
#define TG_NOISE_RIDGED 1
#define TG_NOISE_WORLEY 2
#define TG_NOISE_PERLIN 3
#define TG_NOISE_BILLOW 4
#define TG_NOISE_CRACKS 5

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
    if (type == TG_NOISE_PERLIN)
    {
        return PerlinFbm(p, octaves, period);
    }
    if (type == TG_NOISE_BILLOW)
    {
        return BillowFbm(p, octaves, period);
    }
    if (type == TG_NOISE_CRACKS)
    {
        return WorleyEdgeFbm(p, octaves, period);
    }
    return Fbm(p, octaves, period);
}

#endif  // TG_COMMON_HLSLI
