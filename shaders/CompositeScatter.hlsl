// 散布（Scatter）。terrain-editor の Scatter を移したもの。
//
// 地形の上に**単純な形**（半球 / 円錐）をばら撒き、地形へ足すと同時に
// 「そこに何かある」ことを表す Mask と、個体ごとに違う値を持つ Unique Mask を出す。
// 植生や小石の分布を決めるための土台で、形そのものを作り込むノードではない。
//
//   CsScatter  1 スレッド 1 テクセル。近くの散布点を見て、一番強い形を採る
//   CsResolve  積んだ高さを合成の Height へ足す
//   CsMask     マスク出力（形 / 個体ごとの乱数）を焼く
//
// **散布点は「格子 + ずらし」で決める。** 地形の実寸を `散布の間隔`（m）で割った
// 格子の各マスへ 1 個ずつ置き、ハッシュで中心をずらす。テクセルは自分の周りの
// マスだけを見ればよいので、粒子を走らせずに 1 パスで済む
// （崩落 Crumbling が 1 スレッド 1 粒子なのに対し、こちらは 1 スレッド 1 テクセル）。
//
// **重なった所は「一番高くなるほう」が勝つ**（高さの max による union）。
// 足し合わせると、密度を上げたときに個体の形が消えて一様な盛り上がりになる。
//
// **形（Mask）の勝者と、高さの勝者は別に持つ。** 形だけで勝者を決めて
// その個体の高さを足すと、個体ごとの高さの差（`高さのばらつき`）のぶん、
// 境目で足す量が飛んで**三日月形の段差**になる（terrain-editor の式はこれ）。
// 高さは「高さ × 形」の最大で選べば、段差は出ず折り目だけが残る。
//
// `なめらかさ` を上げると max を soft max へ寄せ、折り目も溶ける（metaball 風）。
//
// **形と乱数は 1 枚の R32_UINT にパックする**（上位 16 bit が形、下位 16 bit が乱数）。
// 高さは別の R32_FLOAT に積む。**Height は読みと書きで状態を分ける**ので、
// 足し戻しは CsResolve で行う（同じテクスチャを SRV と UAV に同時にはできない）。

#include "CompositeCommon.hlsli"

#define TG_SCATTER_SHAPE_HEMISPHERE 0
#define TG_SCATTER_SHAPE_CONE       1

#define TG_SCATTER_ORIENT_FLAT     0  // 地形の傾きを見ない
#define TG_SCATTER_ORIENT_FOLLOW   1  // 斜面に沿わせ、法線の上向き成分で高さを落とす
#define TG_SCATTER_ORIENT_SLOPE    2  // 個体の向きを斜面の向きへ寄せる

// 形 / 乱数のパック幅。どちらも 0〜1 なので 16 bit あれば十分。
#define TG_SCATTER_PACK_SCALE 65535.0f

struct ScatterConstants
{
    // x: 合成 Height の SRV、y: 高さの差分の UAV、z: 配置マスクの SRV、
    // w: 形 / 乱数（パック済み）の UAV
    uint4 indices0;
    // x: 合成解像度、y: 形（TG_SCATTER_SHAPE_*）、z: 向きの決め方（TG_SCATTER_ORIENT_*）、
    // w: 探索半径（散布セル）
    uint4 indices1;
    // x: 合成 Height の UAV（足し戻し用）、y: マスク出力の UAV、
    // z: マスク出力の種類（0 = 形、1 = 個体ごとの乱数）、w: 未使用
    uint4 indices2;
    // x: 散布の間隔（m）、y: 置く確率、z: 最小サイズ（散布セル）、w: 最大サイズ（散布セル）
    float4 params0;
    // x: 盛り上げる高さ（正規化ハイト）、y: 高さのばらつき、z: 向きのばらつき、
    // w: 細長さのばらつき
    float4 params1;
    // x: 届く範囲（散布セル）、y: シード、z: テクセルの大きさ（m）、w: 標高差（m）
    float4 params2;
    // x: なめらかさ（0 で max のまま）、yzw: 未使用
    float4 params3;
};

ConstantBuffer<ScatterConstants> g_scatter : register(b1);

uint ScatterResolution() { return g_scatter.indices1.x; }

// terrain-editor の Scatter と同じハッシュ。**同じシードなら同じ配置**になるよう、
// 式をそのまま移してある（変えると既存のプロジェクトの見た目が変わる）。
uint ScatterHash(int x, int y, int s)
{
    uint h = asuint(x) * 0x27d4eb2du + asuint(y) * 0x9e3779b9u + asuint(s) * 0x85ebca6bu;
    h ^= h >> 16;
    h *= 0x21f0aaadu;
    h ^= h >> 15;
    h *= 0x735a2d97u;
    h ^= h >> 15;
    return h;
}

float ScatterHash01(int x, int y, int s)
{
    return float(ScatterHash(x, y, s) & 0xFFFFFFu) / float(0xFFFFFFu);
}

// 散布セルの座標を、合成テクスチャのテクセルへ直す。
int2 ScatterCellToTexel(float2 cell)
{
    const uint resolution = ScatterResolution();
    const float density = max(g_scatter.params0.x, 0.1f);
    const float texelMeters = max(g_scatter.params2.z, 1e-6f);
    const float sizeMeters = texelMeters * float(resolution);
    const float2 meters = cell * density + sizeMeters * 0.5f;
    return clamp(int2(meters / texelMeters), int2(0, 0),
                 int2(int(resolution) - 1, int(resolution) - 1));
}

// 地形の傾き（m / m）。**個体の中心で 1 回だけ読む。**
// テクセルごとに読むと、1 つの個体の中でも足元の傾きで高さや向きが変わり、
// 球が歪んで切り欠きが入る。個体は「中心の傾きに沿った 1 つの形」として置く。
float2 SampleGroundGradient(int2 texel)
{
    const uint resolution = ScatterResolution();
    const float texelMeters = max(g_scatter.params2.z, 1e-6f);
    const float heightMeters = max(g_scatter.params2.w, 1e-6f);
    Texture2D<float> heightIn = ResourceDescriptorHeap[g_scatter.indices0.x];

    const int last = int(resolution) - 1;
    const int2 xm = int2(max(texel.x - 1, 0), texel.y);
    const int2 xp = int2(min(texel.x + 1, last), texel.y);
    const int2 zm = int2(texel.x, max(texel.y - 1, 0));
    const int2 zp = int2(texel.x, min(texel.y + 1, last));
    // Height は正規化なので、標高差を掛けて m に直してから傾きにする。
    const float invTwoMeters = heightMeters / (2.0f * texelMeters);
    return float2((heightIn.Load(int3(xp, 0)) - heightIn.Load(int3(xm, 0))) * invTwoMeters,
                  (heightIn.Load(int3(zp, 0)) - heightIn.Load(int3(zm, 0))) * invTwoMeters);
}

// なめらかな max。k = 0 なら普通の max。
// 2 つの高さが k の範囲で近いとき、角を丸めて溶け合わせる（多項式 smooth max）。
float SmoothMax(float a, float b, float k)
{
    if (k <= 0.0f)
    {
        return max(a, b);
    }
    const float h = saturate(0.5f + 0.5f * (a - b) / k);
    return lerp(b, a, h) + k * h * (1.0f - h);
}

// 散布点の中心（散布セル座標）で配置マスクを読む。
// **個体の中心 1 点だけ**を見る。テクセルごとに読むと、マスクの縁で個体が
// 切り取られて形が崩れる（半分だけの半球が並ぶ）。
float SamplePlacementMask(float cellX, float cellZ)
{
    if (g_scatter.indices0.z == kInvalidTextureIndex)
    {
        return 1.0f;
    }
    Texture2D<float> placement = ResourceDescriptorHeap[g_scatter.indices0.z];
    return saturate(placement.Load(int3(ScatterCellToTexel(float2(cellX, cellZ)), 0)));
}

[numthreads(8, 8, 1)]
void CsScatter(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = ScatterResolution();
    if (texel.x >= resolution || texel.y >= resolution)
    {
        return;
    }

    Texture2D<float> heightIn = ResourceDescriptorHeap[g_scatter.indices0.x];
    RWTexture2D<float> delta = ResourceDescriptorHeap[g_scatter.indices0.y];
    RWTexture2D<uint> packed = ResourceDescriptorHeap[g_scatter.indices0.w];

    const float density = max(g_scatter.params0.x, 0.1f);
    const float coverage = saturate(g_scatter.params0.y);
    const float sizeMinCells = g_scatter.params0.z;
    const float sizeMaxCells = g_scatter.params0.w;
    const float heightAmount = g_scatter.params1.x;
    const float heightJitter = saturate(g_scatter.params1.y);
    const float rotationVar = saturate(g_scatter.params1.z);
    const float aspectVar = saturate(g_scatter.params1.w);
    const float maxReach = g_scatter.params2.x;
    const int seed = int(g_scatter.params2.y);
    const float texelMeters = max(g_scatter.params2.z, 1e-6f);
    const float heightMeters = max(g_scatter.params2.w, 1e-6f);
    const uint shapeType = g_scatter.indices1.y;
    const uint orientation = g_scatter.indices1.z;
    const int searchRadius = int(g_scatter.indices1.w);

    // テクセルの中心を m で置き、散布セルの座標へ直す。**中心を原点にする。**
    // 端を原点にすると、地形の大きさを変えたときに散布の並びが全部ずれる。
    const float sizeMeters = texelMeters * float(resolution);
    const float worldX = (float(texel.x) + 0.5f) * texelMeters - sizeMeters * 0.5f;
    const float worldZ = (float(texel.y) + 0.5f) * texelMeters - sizeMeters * 0.5f;
    const float cellX = worldX / density;
    const float cellZ = worldZ / density;
    const int baseCx = int(floor(cellX));
    const int baseCz = int(floor(cellZ));

    // なめらかさは高さの尺度で効かせる（個体の高さの何割まで溶かすか）。
    const float smoothK = saturate(g_scatter.params3.x) * heightAmount * 0.5f;

    // 用途ごとにシードをずらす。並びは terrain-editor と同じ。
    const int sizeSeed = seed * 1583 + 22441;
    const int heightSeed = seed * 2017 + 39019;
    const int rotationSeed = seed * 4519 + 91173;
    const int aspectSeed = seed * 2381 + 33797;
    const int aspectAxisSeed = seed * 4093 + 51817;
    const int uniqueSeed = seed * 1877 + 73009;

    float bestShape = 0.0f;
    float bestHeight = 0.0f;
    float bestUnique = 0.0f;

    for (int dz = -searchRadius; dz <= searchRadius; ++dz)
    {
        for (int dx = -searchRadius; dx <= searchRadius; ++dx)
        {
            const int gx = baseCx + dx;
            const int gz = baseCz + dz;

            // マスの中でずらして置く。格子のままだと並びが目に見えてしまう。
            const float jitterX = ScatterHash01(gx, gz, seed) * 0.9f - 0.45f;
            const float jitterZ = ScatterHash01(gx, gz, seed + 73) * 0.9f - 0.45f;
            const float centerX = float(gx) + 0.5f + jitterX;
            const float centerZ = float(gz) + 0.5f + jitterZ;

            // 置くかどうか。配置マスクは**個体の中心**で読み、確率に掛ける。
            const float placement = SamplePlacementMask(centerX, centerZ);
            if (ScatterHash01(gx, gz, seed + 17) > coverage * placement)
            {
                continue;
            }

            const float offsetX = cellX - centerX;
            const float offsetZ = cellZ - centerZ;
            if (sqrt(offsetX * offsetX + offsetZ * offsetZ) >= maxReach)
            {
                continue;
            }

            const float sizeCells =
                sizeMinCells + ScatterHash01(gx, gz, sizeSeed) * (sizeMaxCells - sizeMinCells);
            const float radiusCells = max(sizeCells * 0.5f, 1e-4f);

            // 地形の傾きは**この個体の中心**で読む（Flat のときは要らない）。
            float2 gradient = float2(0.0f, 0.0f);
            float slopeLength = 0.0f;
            float normalUp = 1.0f;
            if (orientation != TG_SCATTER_ORIENT_FLAT)
            {
                gradient = SampleGroundGradient(ScatterCellToTexel(float2(centerX, centerZ)));
                slopeLength = length(gradient);
                normalUp = 1.0f / sqrt(1.0f + slopeLength * slopeLength);
            }

            // 向き。Slope Oriented は斜面の向きへ寄せ、そこへばらつきを足す。
            const float randomTheta =
                (ScatterHash01(gx, gz, rotationSeed) - 0.5f) * 6.28318530718f * rotationVar;
            const float slopeTheta = (slopeLength > 1e-4f) ? atan2(gradient.y, gradient.x) : 0.0f;
            const float theta = (orientation == TG_SCATTER_ORIENT_SLOPE && slopeLength > 1e-4f)
                                    ? (slopeTheta + randomTheta)
                                    : randomTheta;
            const float cosTheta = cos(theta);
            const float sinTheta = sin(theta);

            // 細長さ。片方の軸を伸ばし、もう片方を同じだけ縮める（面積を保つ）。
            const float aspectExponent =
                aspectVar * (2.0f * ScatterHash01(gx, gz, aspectSeed) - 1.0f);
            const float aspect = pow(2.0f, aspectExponent);
            const bool longX = ScatterHash01(gx, gz, aspectAxisSeed) < 0.5f;
            const float aspectX = longX ? aspect : (1.0f / aspect);
            const float aspectZ = 1.0f / aspectX;

            const float rotatedX = offsetX * cosTheta + offsetZ * sinTheta;
            const float rotatedZ = -offsetX * sinTheta + offsetZ * cosTheta;
            const float localX = rotatedX / aspectX;
            const float localZ = rotatedZ / aspectZ;
            // Follow Ground / Slope Oriented は、斜面に沿った距離も測る
            // （急な斜面では、平面で見た足元より実際の面は長い）。
            const float slopeAlong = (orientation != TG_SCATTER_ORIENT_FLAT)
                                         ? (gradient.x * offsetX + gradient.y * offsetZ)
                                         : 0.0f;
            const float normalized =
                sqrt(localX * localX + localZ * localZ + slopeAlong * slopeAlong) / radiusCells;
            if (normalized >= 1.0f)
            {
                continue;
            }

            const float shape = (shapeType == TG_SCATTER_SHAPE_CONE)
                                    ? saturate(1.0f - normalized)
                                    : sqrt(max(0.0f, 1.0f - normalized * normalized));

            const float jitter = ScatterHash01(gx, gz, heightSeed);
            const float orientationScale =
                (orientation == TG_SCATTER_ORIENT_FOLLOW) ? normalUp : 1.0f;
            const float instanceHeight =
                heightAmount * orientationScale *
                (1.0f - heightJitter + heightJitter * 2.0f * jitter);

            // **形（Mask / Unique）と高さは別々に決める。**
            // 形だけで勝者を決めてその高さを足すと、個体ごとの高さの差のぶん、
            // 境目で足す量が飛んで段差になる。
            if (shape > bestShape)
            {
                bestShape = shape;
                bestUnique = ScatterHash01(gx, gz, uniqueSeed);
            }
            bestHeight = SmoothMax(bestHeight, instanceHeight * shape, smoothK);
        }
    }

    delta[texel] = bestHeight;
    packed[texel] = (uint(saturate(bestShape) * TG_SCATTER_PACK_SCALE) << 16u) |
                    uint(saturate(bestUnique) * TG_SCATTER_PACK_SCALE);
}

[numthreads(8, 8, 1)]
void CsResolve(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = ScatterResolution();
    if (texel.x >= resolution || texel.y >= resolution)
    {
        return;
    }

    RWTexture2D<float> delta = ResourceDescriptorHeap[g_scatter.indices0.y];
    RWTexture2D<float> height = ResourceDescriptorHeap[g_scatter.indices2.x];
    height[texel] = height[texel] + delta[texel];
}

[numthreads(8, 8, 1)]
void CsMask(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 texel = dispatchThreadId.xy;
    const uint resolution = ScatterResolution();
    if (texel.x >= resolution || texel.y >= resolution)
    {
        return;
    }

    Texture2D<uint> packed = ResourceDescriptorHeap[g_scatter.indices0.w];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_scatter.indices2.y];

    const uint value = packed.Load(int3(int2(texel), 0));
    // 0 = 形（そこに何かある）、1 = 個体ごとの乱数（色や材質のばらつき用）。
    const uint stored = (g_scatter.indices2.z == 0u) ? (value >> 16u) : (value & 0xFFFFu);
    mask[texel] = float(stored) / TG_SCATTER_PACK_SCALE;
}
