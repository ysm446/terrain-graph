#ifndef TG_COMPOSITE_PATH_HLSLI
#define TG_COMPOSITE_PATH_HLSLI

// パスの線分列の読み込み。Mask Path / Mask Area（CompositeMaskPath.hlsl）と
// Surface のパス UV（CompositeLayer.hlsl）で共有する。
//
// 線分 1 本は float 12 個（48 バイト）。C++ 側の kPathSegmentStride と一致させること。
//   [0..3]: ax, ay, bx, by（正規化 UV）
//   [4..7]: widthA, widthB, featherA, featherB（m）
//   [8..11]: intensityA, intensityB, alongA, alongB（along は鎖の始点からの弧長。正規化 UV 単位）
#define TG_PATH_SEGMENT_BYTES 48u

struct PathSegmentData
{
    float2 a;
    float2 b;
    float widthA;
    float widthB;
    float featherA;
    float featherB;
    float intensityA;
    float intensityB;
    float alongA;
    float alongB;
};

// 座標と弧長は一辺の長さ（m）を掛けて実寸にして返す。
PathSegmentData LoadSegment(ByteAddressBuffer buffer, uint index, float sizeMeters)
{
    const uint base = index * TG_PATH_SEGMENT_BYTES;
    const float4 ends = asfloat(buffer.Load4(base));
    const float4 widths = asfloat(buffer.Load4(base + 16u));
    const float4 rest = asfloat(buffer.Load4(base + 32u));
    PathSegmentData segment;
    segment.a = ends.xy * sizeMeters;
    segment.b = ends.zw * sizeMeters;
    segment.widthA = widths.x;
    segment.widthB = widths.y;
    segment.featherA = widths.z;
    segment.featherB = widths.w;
    segment.intensityA = rest.x;
    segment.intensityB = rest.y;
    segment.alongA = rest.z * sizeMeters;
    segment.alongB = rest.w * sizeMeters;
    return segment;
}

// 中心線からの距離 → 0〜1。幅の半分までは 1、その外側をフェザーで 0 へ。
float PathDistanceValue(float distance, float width, float feather)
{
    const float half = max(width, 0.0f) * 0.5f;
    if (distance <= half)
    {
        return 1.0f;
    }
    if (feather <= 1e-4f)
    {
        return 0.0f;
    }
    return saturate(1.0f - (distance - half) / feather);
}

// 最寄りの線分から見たパス座標（帯の座標系）。
//   along    : 鎖の始点からの弧長（m）
//   across   : 中心線からの符号付き距離（m）。進行方向の右手が正
//   direction: 進行方向（単位ベクトル）
//   width / feather / coverage: その位置の幅（m）・フェザー（m）・帯の内側なら 1
//   intensity: その位置の点の強さ（0〜1）。coverage には掛けていない。使う側が決める
struct PathFrame
{
    float along;
    float across;
    float2 direction;
    float width;
    float feather;
    float coverage;
    float intensity;
};

PathFrame ComputePathFrame(ByteAddressBuffer segments, uint count, float2 position, float sizeMeters)
{
    PathFrame frame;
    frame.along = 0.0f;
    frame.across = 0.0f;
    frame.direction = float2(1.0f, 0.0f);
    frame.width = 0.0f;
    frame.feather = 0.0f;
    frame.coverage = 0.0f;
    frame.intensity = 1.0f;
    float nearest = 1e30f;
    [loop]
    for (uint i = 0; i < count; ++i)
    {
        const PathSegmentData segment = LoadSegment(segments, i, sizeMeters);
        const float2 ab = segment.b - segment.a;
        const float lengthSq = dot(ab, ab);
        // 距離（帯の内外の判定）は線分の内側に切り詰めた最寄り点で取る。
        // 座標（弧長・横距離）は切り詰めない位置で取る。端点の外（丸いキャップ）でも
        // 線分を延長した向きに弧長が進み、模様が端で止まって伸びない。
        const float tRaw = (lengthSq > 1e-8f) ? dot(position - segment.a, ab) / lengthSq : 0.0f;
        const float t = saturate(tRaw);
        const float distance = length(position - (segment.a + ab * t));
        if (distance >= nearest)
        {
            continue;
        }
        nearest = distance;
        const float2 direction = (lengthSq > 1e-8f) ? ab * rsqrt(lengthSq) : float2(1.0f, 0.0f);
        frame.direction = direction;
        frame.along = lerp(segment.alongA, segment.alongB, tRaw);
        // 進行方向を V 軸、その右手（direction を -90 度回した向き）を U 軸に取る。
        // 横距離は線分を無限に延ばした直線からの符号付き距離。
        frame.across = dot(position - segment.a, float2(direction.y, -direction.x));
        frame.width = lerp(segment.widthA, segment.widthB, t);
        frame.feather = lerp(segment.featherA, segment.featherB, t);
        frame.coverage = PathDistanceValue(distance, frame.width, frame.feather);
        frame.intensity = saturate(lerp(segment.intensityA, segment.intensityB, t));
    }
    return frame;
}

#endif
