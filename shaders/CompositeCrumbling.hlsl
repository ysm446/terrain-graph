// 崩落（Crumbling）。terrain-editor の Crumbling を移したもの。
//
// 発生源のマスクが明るい所から岩片を生み、地形の低い方へ歩かせ、止まった位置に
// 岩片の形を積む。terrain-editor は CPU で「粒子を作る → 分離 → 描く」を順に
// 回すが、ここは **1 スレッド = 1 粒子**で「生む → 歩く → 描く」まで通す。
//
//   CsClear    積む先（パック済みの R32_UINT）を 0 で埋める
//   CsScatter  1 スレッド 1 粒子。生成・棄却・歩行・描き込み
//   CsResolve  積んだ高さを合成の Height へ足す
//   CsMask     マスク出力（厚み / 岩片ごとの乱数）を焼く
//
// **積む先は 1 枚の R32_UINT にパックする。**
//
//   上位 20 bit: 岩屑の高さ（岩片の最大の高さで割った 0〜1）
//   下位 12 bit: その岩片ごとの乱数
//
// 非負の値どうしなら uint の大小がそのまま高さの大小になるので、
// `InterlockedMax` 1 回で「一番高い岩片が勝ち、その乱数も一緒に残る」が成り立つ。
// 高さと付随データを別々に書くと、勝者が入れ替わる競合を避けられない。
//
// **粒子どうしの分離パスは持たない。** terrain-editor は止まった後に近すぎる
// 岩片を押し分けるが、あれは全粒子を見渡す処理で、1 スレッド 1 粒子とは相性が悪い。
// 代わりに歩行中の横ぶれ（spread）だけで散らす。

#include "CompositeCommon.hlsli"

#define TG_ROCK_STYLE_CLASSIC   0
#define TG_ROCK_STYLE_POLYGONAL 1
#define TG_ROCK_STYLE_SHARD     2

// 高さのパック幅。残り 12 bit を岩片ごとの乱数に使う。
#define TG_CRUMBLING_HEIGHT_BITS 20u
#define TG_CRUMBLING_UNIQUE_MASK 0xFFFu

struct CrumblingConstants
{
    // x: 積む先の UAV、y: 合成 Height の SRV、z: 発生マスクの SRV、w: 合成 Height の UAV
    uint4 indices0;
    // x: 合成解像度、y: 粒子の試行回数、z: 歩数、w: 岩片の形（TG_ROCK_STYLE_*）
    uint4 indices1;
    // x: マスクの出力 UAV、y: 出力の種類（0 = 厚み、1 = 乱数）、zw: 未使用
    uint4 indices2;
    // x: 最小サイズ（テクセル）、y: 最大サイズ（テクセル）、z: gravity、w: spread
    float4 params0;
    // x: 岩片の高さの最大（正規化ハイト）、y: シード、z: テクセルの大きさ（m）、w: 標高差（m）
    float4 params1;
    // x: 岩屑の量（高さに効く）、yzw: 未使用
    float4 params2;
};

ConstantBuffer<CrumblingConstants> g_crumbling : register(b1);

static const float kCrumblingPi = 3.14159265358979323846f;

uint CrumblingResolution() { return g_crumbling.indices1.x; }

// terrain-editor と同じ線形合同法。**同じシードなら同じ結果**になるようにする。
float Hash01(inout uint state)
{
    state = state * 1664525u + 1013904223u;
    return float((state >> 8) & 0xFFFFFFu) / float(0xFFFFFFu);
}

float SampleCrumblingHeight(float2 texel)
{
    Texture2D<float> height = ResourceDescriptorHeap[g_crumbling.indices0.y];
    const float resolution = float(CrumblingResolution());
    const float2 uv = clamp((texel + 0.5f) / resolution, 0.0f, 1.0f);
    return height.SampleLevel(g_samplerLinearClamp, uv, 0.0f);
}

float SampleEmission(float2 texel)
{
    if (g_crumbling.indices0.z == kInvalidTextureIndex)
    {
        return 1.0f;
    }
    Texture2D<float> emission = ResourceDescriptorHeap[g_crumbling.indices0.z];
    const float resolution = float(CrumblingResolution());
    const float2 uv = clamp((texel + 0.5f) / resolution, 0.0f, 1.0f);
    return saturate(emission.SampleLevel(g_samplerLinearClamp, uv, 0.0f));
}

[numthreads(8, 8, 1)]
void CsClear(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = CrumblingResolution();
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<uint> packed = ResourceDescriptorHeap[g_crumbling.indices0.x];
    packed[texel] = 0u;
}

// 1 スレッド 1 粒子。生んで、歩かせて、止まった所へ描く。
[numthreads(64, 1, 1)]
void CsScatter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint particle = dispatchThreadId.x;
    if (particle >= g_crumbling.indices1.y) { return; }

    const uint resolution = CrumblingResolution();
    const float limit = float(resolution - 1u);
    uint state = (particle * 747796405u + 2891336453u) ^ asuint(g_crumbling.params1.y);
    // 最初の数回は下位ビットの相関が残るので捨てる。
    Hash01(state);
    Hash01(state);

    // --- 生成 ---------------------------------------------------------------
    const float x0 = Hash01(state) * limit;
    const float z0 = Hash01(state) * limit;
    // **発生マスクが暗い所は棄却する。** terrain-editor は目標数に届くまで
    // 試行を繰り返すが、ここは試行回数を固定してマスクの明るさで受け入れる
    // （マスクが狭いほど岩屑が減る、という素直な効き方になる）。
    if (Hash01(state) > SampleEmission(float2(x0, z0)))
    {
        return;
    }

    const float minSize = g_crumbling.params0.x;
    const float maxSize = g_crumbling.params0.y;
    const float sizeTexels = minSize + Hash01(state) * (maxSize - minSize);
    const float sizeMeters = sizeTexels * g_crumbling.params1.z;
    const float gravity = g_crumbling.params0.z;
    const float spread = g_crumbling.params0.w;
    const uint style = g_crumbling.indices1.w;
    const bool polygonal = (style != TG_ROCK_STYLE_CLASSIC);
    const bool shard = (style == TG_ROCK_STYLE_SHARD);

    // --- 歩行 ---------------------------------------------------------------
    float x = x0;
    float z = z0;
    float dirX = Hash01(state) * 2.0f - 1.0f;
    float dirZ = Hash01(state) * 2.0f - 1.0f;
    float dirLen = sqrt(dirX * dirX + dirZ * dirZ);
    if (dirLen > 1e-4f)
    {
        dirX /= dirLen;
        dirZ /= dirLen;
    }
    const float stepTexels = clamp(sizeTexels * (0.18f + gravity * 0.34f), 0.25f, 8.0f);
    const uint steps = g_crumbling.indices1.z;
    for (uint step = 0u; step < steps; ++step)
    {
        // 低い方の向き。勾配の逆。
        const float hL = SampleCrumblingHeight(float2(x - 1.0f, z));
        const float hR = SampleCrumblingHeight(float2(x + 1.0f, z));
        const float hD = SampleCrumblingHeight(float2(x, z - 1.0f));
        const float hU = SampleCrumblingHeight(float2(x, z + 1.0f));
        float downX = -(hR - hL);
        float downZ = -(hU - hD);
        const float downLen = sqrt(downX * downX + downZ * downZ);
        if (downLen < 1e-9f)
        {
            break;
        }
        downX /= downLen;
        downZ /= downLen;

        // gravity が低いほど下り方向から逸れ、spread が高いほど横へ散る。
        const float gravityWander = (Hash01(state) - 0.5f) * (1.0f - gravity) * 0.75f;
        const float spreadWander = (spread > 0.0f)
            ? (Hash01(state) - 0.5f) * spread * (0.45f + 0.65f * (1.0f - gravity))
            : 0.0f;
        const float wander = gravityWander + spreadWander;
        const float cosW = cos(wander);
        const float sinW = sin(wander);
        const float wx = downX * cosW - downZ * sinW;
        const float wz = downX * sinW + downZ * cosW;
        const float follow = 0.35f + gravity * 0.55f;
        dirX = lerp(dirX, wx, follow);
        dirZ = lerp(dirZ, wz, follow);
        dirLen = sqrt(dirX * dirX + dirZ * dirZ);
        if (dirLen < 1e-5f)
        {
            break;
        }
        dirX /= dirLen;
        dirZ /= dirLen;

        const float sideStep =
            (spread > 0.0f) ? (Hash01(state) - 0.5f) * spread * stepTexels * 0.45f : 0.0f;
        const float nextX = x + dirX * stepTexels - dirZ * sideStep;
        const float nextZ = z + dirZ * stepTexels + dirX * sideStep;
        if (nextX <= 0.0f || nextX >= limit || nextZ <= 0.0f || nextZ >= limit)
        {
            break;
        }
        const float h0 = SampleCrumblingHeight(float2(x, z));
        const float h1 = SampleCrumblingHeight(float2(nextX, nextZ));
        x = nextX;
        z = nextZ;
        // ほとんど下っていない所まで来たら止める（谷底で延々と彷徨わせない）。
        // 落差は m で見る。ハイトは正規化値なので標高差を掛けて戻す。
        const float dropMeters = (h0 - h1) * g_crumbling.params1.w;
        if (dropMeters < g_crumbling.params1.z * 0.003f && step > steps / 4u)
        {
            break;
        }
    }

    // --- 岩片の形 -----------------------------------------------------------
    const float amount = g_crumbling.params2.x;
    const float heightMeters =
        sizeMeters * (0.10f + 0.18f * amount) * (0.65f + 0.7f * Hash01(state));
    const float rotation =
        atan2(dirZ, dirX) + (Hash01(state) - 0.5f) * kCrumblingPi * (shard ? 0.35f : 1.0f);
    const float aspectBoost = shard ? 1.2f : (polygonal ? 0.45f : 0.15f);
    const float aspect = pow(2.0f, aspectBoost * Hash01(state));
    const float unique = Hash01(state);

    // 正規化ハイトへ。さらに「岩片の最大の高さ」で割って 0〜1 に載せる。
    const float heightNormalized = heightMeters / max(g_crumbling.params1.w, 1e-6f);
    const float heightUnit = saturate(heightNormalized / max(g_crumbling.params1.x, 1e-9f));
    if (heightUnit <= 0.0f)
    {
        return;
    }

    RWTexture2D<uint> packed = ResourceDescriptorHeap[g_crumbling.indices0.x];
    const float radius = max(0.5f, sizeTexels * 0.5f);
    const float reach = radius * max(aspect, 1.0f / aspect) * 1.1f;
    const int minX = int(max(floor(x - reach), 0.0f));
    const int maxX = int(min(ceil(x + reach), limit));
    const int minZ = int(max(floor(z - reach), 0.0f));
    const int maxZ = int(min(ceil(z + reach), limit));
    const float cosT = cos(rotation);
    const float sinT = sin(rotation);
    const int facets = shard ? 4 : 6;
    const float inradius = radius * cos(kCrumblingPi / float(facets));
    const uint uniqueBits = uint(saturate(unique) * float(TG_CRUMBLING_UNIQUE_MASK));
    const float heightScale = float((1u << TG_CRUMBLING_HEIGHT_BITS) - 1u);

    for (int tz = minZ; tz <= maxZ; ++tz)
    {
        for (int tx = minX; tx <= maxX; ++tx)
        {
            const float dx = float(tx) - x;
            const float dz = float(tz) - z;
            // 岩片のローカル座標へ。aspect で伸ばした楕円にする。
            const float rx = (dx * cosT + dz * sinT) / aspect;
            const float rz = (-dx * sinT + dz * cosT) * aspect;
            const float dist = sqrt(rx * rx + rz * rz);
            if (dist >= radius)
            {
                continue;
            }
            float t = saturate(1.0f - dist / radius);
            if (polygonal)
            {
                // 多角形の内側までの距離。外なら描かない（角のある輪郭になる）。
                float polyDist = 3.4e38f;
                for (int i = 0; i < facets; ++i)
                {
                    const float a = (float(i) + unique) * (2.0f * kCrumblingPi / float(facets));
                    const float interior = inradius - (rx * cos(a) + rz * sin(a));
                    polyDist = min(polyDist, interior);
                }
                if (polyDist <= 0.0f)
                {
                    continue;
                }
                t = saturate(polyDist / max(inradius, 1e-4f));
            }
            // Shard は尖らせるため線形、それ以外は丸いドームにする。
            const float dome = shard ? t : (t * t * (3.0f - 2.0f * t));
            const uint bits = uint(saturate(heightUnit * dome) * heightScale);
            if (bits == 0u)
            {
                continue;
            }
            const uint value = (bits << 12u) | uniqueBits;
            uint previous;
            InterlockedMax(packed[uint2(tx, tz)], value, previous);
        }
    }
}

// 積んだ岩屑を合成の Height へ足す。
[numthreads(8, 8, 1)]
void CsResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = CrumblingResolution();
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    RWTexture2D<uint> packed = ResourceDescriptorHeap[g_crumbling.indices0.x];
    RWTexture2D<float> heightTarget = ResourceDescriptorHeap[g_crumbling.indices0.w];

    const uint value = packed[texel];
    if (value == 0u) { return; }

    const float heightScale = float((1u << TG_CRUMBLING_HEIGHT_BITS) - 1u);
    const float unit = float(value >> 12u) / heightScale;
    heightTarget[texel] = saturate(heightTarget[texel] + unit * g_crumbling.params1.x);
}

// マスク出力。厚み（岩片の形そのもの）か、岩片ごとの乱数を焼く。
[numthreads(8, 8, 1)]
void CsMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = CrumblingResolution();
    if (texel.x >= resolution || texel.y >= resolution) { return; }

    Texture2D<uint> packed = ResourceDescriptorHeap[g_crumbling.indices0.x];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_crumbling.indices2.x];

    const uint value = packed.Load(int3(int2(texel), 0));
    const float heightScale = float((1u << TG_CRUMBLING_HEIGHT_BITS) - 1u);
    const float unit = float(value >> 12u) / heightScale;
    if (g_crumbling.indices2.y == 0u)
    {
        // 厚み。岩片の最大の高さで割ってあるので、そのまま 0〜1 の形になる。
        mask[texel] = unit;
    }
    else
    {
        // 岩片ごとの乱数。岩屑が無い所は 0。
        const float unique = float(value & TG_CRUMBLING_UNIQUE_MASK) /
                             float(TG_CRUMBLING_UNIQUE_MASK);
        mask[texel] = (value == 0u) ? 0.0f : unique;
    }
}
