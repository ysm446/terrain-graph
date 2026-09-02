// 被写界深度。**トーンマップの前、線形 HDR のシーンカラーに対して掛ける。**
//
// 露出を掛けた後のカラーでぼかすと、明るい点が飽和してから広がるため、
// ボケの芯が白く潰れて玉ボケにならない。HDR のまま混ぜれば、
// 明るいサンプルが重みなしでもそのまま強く出る（ハイライト強調の細工が要らない）。
//
// 集め方は 1 パスの gather 近似。
//   1. 深度からカメラ前方距離を復元する
//   2. 焦点距離・F 値・フォーカス距離・センサー高さから錯乱円（CoC）を出す
//   3. CoC をピクセル半径に直し、絞りの形に沿って周りを混ぜる
//   4. 手前のボケは別に集め、ピントの合った背景へにじませる
//
// レンズの値は**カメラと露出からそのまま持ってくる**（焦点距離は画角から、
// F 値は露出の絞りから）。被写界深度のためだけの二重指定を作らない。

#include "Common.hlsli"

struct DofConstants
{
    uint sourceIndex;   // 線形 HDR のシーンカラー（SRV）
    uint depthIndex;    // 深度（SRV, R32_FLOAT）
    uint outputIndex;   // ぼかした結果（UAV）
    uint width;

    uint height;
    float focalLengthMm;
    float fStop;
    float focusDistance;   // メートル。ワールドの 1 単位を 1m とみなす

    float nearZ;
    float farZ;
    float maxBlurPixels;
    float apertureRotation;  // ラジアン

    // 0 = 円、3 以上なら多角形の羽根数。
    float apertureBlades;
    // 錯乱円に掛ける表示用の倍率。1 で現実どおり。
    float blurScale;
    float pad0;
    float pad1;
};

ConstantBuffer<DofConstants> g_dof : register(b0);

// センサーの高さ (mm)。**C++ 側の renderer::kSensorHeightMm と揃えること。**
// 画角と焦点距離の換算に使っているのと同じ 35mm フルサイズ基準。
static const float kSensorHeightMm = 24.0f;

// 周りを混ぜるサンプル数。多角形の形が出て、かつ 1 パスで収まる数。
static const int kSampleCount = 48;
// 黄金角。少ない点数でも渦を巻かずに散る。
static const float kGoldenAngle = 2.39996323f;

// 深度から「カメラ前方の距離」を戻す。
// XMMatrixPerspectiveFovRH の逆算で、d=0 で near、d=1 で far になる。
float ViewDistanceFromDepth(float depth)
{
    const float denominator = g_dof.farZ - depth * (g_dof.farZ - g_dof.nearZ);
    return g_dof.nearZ * g_dof.farZ / max(denominator, 1e-6f);
}

// 符号付きの錯乱円の半径（ピクセル）。正なら奥、負なら手前のボケ。
//
//   CoC(直径) = A * f * |d - D| / (d * (D - f))     A = f / N（絞りの実口径）
//
// センサー高さで割って画像の高さを掛けるとピクセルになる。**半径にするので 2 で割る。**
float SignedCocPixels(float viewDistance, float imageHeightPixels)
{
    const float focalLength = max(g_dof.focalLengthMm, 1.0f) * 0.001f;  // m
    const float aperture = focalLength / max(g_dof.fStop, 0.5f);
    // ピント面と物体は、どちらもレンズより手前に来ないようにする（式が発散する）。
    const float focus = max(g_dof.focusDistance, focalLength + 1e-3f);
    const float distance = max(viewDistance, focalLength + 1e-3f);

    const float coc = aperture * focalLength * (distance - focus) /
                      max(distance * (focus - focalLength), 1e-6f);
    const float sensorHeight = kSensorHeightMm * 0.001f;
    // **倍率は最後に掛ける。** これはスケールの補正ではなく、意図的な誇張。
    // 2m 角の地面を 3.2m から広角で撮れば現実のカメラでも全域にピントが合うので、
    // 物理の関係は保ったまま、見せたい量まで持ち上げるための係数として掛ける。
    const float pixels = coc / sensorHeight * imageHeightPixels * 0.5f * g_dof.blurScale;
    return clamp(pixels, -g_dof.maxBlurPixels, g_dof.maxBlurPixels);
}

// 多角形の絞りにするための半径の縮め方。角の方向で 1、辺の中央で cos(半セクタ)。
float PolygonRadiusScale(float angle, float blades)
{
    const float sector = 2.0f * kPi / max(blades, 3.0f);
    const float local = fmod(angle + sector * 0.5f + kPi * 4.0f, sector) - sector * 0.5f;
    return saturate(cos(sector * 0.5f) / max(cos(local), 1e-3f));
}

// 絞りの中のサンプル位置（半径 1 の円 or 多角形）。
float2 ApertureSample(int index, float blades)
{
    const float t = (float(index) + 0.5f) / float(kSampleCount);
    const float angle = float(index) * kGoldenAngle + g_dof.apertureRotation;
    // sqrt で面積が均等になる。中心へ寄り過ぎるとボケの縁が立たない。
    float ring = sqrt(t);

    if (blades >= 3.0f)
    {
        // 4 点に 1 点は縁へ寄せる。多角形の辺がはっきり出る。
        const float edge = step(2.5f, fmod(float(index), 4.0f));
        ring = lerp(ring * 0.92f, 0.985f, edge);
        return float2(cos(angle), sin(angle)) * ring * PolygonRadiusScale(angle, blades);
    }
    return float2(cos(angle), sin(angle)) * ring;
}

float LoadDepth(Texture2D<float> depthMap, int2 pixel)
{
    const int2 clamped = clamp(pixel, int2(0, 0), int2(int(g_dof.width) - 1, int(g_dof.height) - 1));
    return depthMap.Load(int3(clamped, 0));
}

[numthreads(8, 8, 1)]
void CsMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= g_dof.width || dispatchThreadId.y >= g_dof.height)
    {
        return;
    }

    Texture2D<float4> source = ResourceDescriptorHeap[g_dof.sourceIndex];
    Texture2D<float> depthMap = ResourceDescriptorHeap[g_dof.depthIndex];
    RWTexture2D<float4> output = ResourceDescriptorHeap[g_dof.outputIndex];

    const int2 center = int2(dispatchThreadId.xy);
    const float imageHeight = max(float(g_dof.height), 1.0f);
    const float3 sharp = source[center].rgb;

    const float centerDistance = ViewDistanceFromDepth(LoadDepth(depthMap, center));
    const float centerRadius = abs(SignedCocPixels(centerDistance, imageHeight));
    const float blades = g_dof.apertureBlades;

    float3 sum = 0.0f;
    float weightSum = 0.0f;
    // 手前のボケがこの画素をどれだけ覆っているか。にじみの強さに使う。
    float foregroundCoverSum = 0.0f;

    [unroll]
    for (int i = 0; i < kSampleCount; ++i)
    {
        const float2 offset = ApertureSample(i, blades);

        // --- 奥のボケ（この画素の CoC のぶんだけ周りから集める）-------------
        if (centerRadius >= 0.5f)
        {
            const int2 tap = center + int2(offset * centerRadius);
            const float tapDistance = ViewDistanceFromDepth(LoadDepth(depthMap, tap));
            const float tapRadius = abs(SignedCocPixels(tapDistance, imageHeight));
            // **ピントの合っている手前の物を、後ろのボケへ吸い込ませない。**
            // ぼけていないサンプルの重みを落とすことで、輪郭の滲み出しを抑える。
            const float weight = max(saturate(tapRadius / max(centerRadius, 1e-3f)), 0.25f);
            sum += source[clamp(tap, int2(0, 0), int2(int(g_dof.width) - 1, int(g_dof.height) - 1))].rgb *
                   weight;
            weightSum += weight;
        }

        // --- 手前のボケ（周りのボケた物が、この画素へはみ出してくる）--------
        // 奥のボケと違い、**自分の CoC ではなく相手の CoC で決まる**ので、
        // 上限の半径まで探しに行って「届いているか」を判定する。
        const float2 farOffset = offset * g_dof.maxBlurPixels;
        const int2 farTap = center + int2(farOffset);
        const float farDistance = ViewDistanceFromDepth(LoadDepth(depthMap, farTap));
        const float farSigned = SignedCocPixels(farDistance, imageHeight);
        if (farSigned < -0.5f && farDistance < centerDistance - 1e-3f)
        {
            const float reach = abs(farSigned) - length(farOffset);
            const float cover = saturate((reach + 1.0f) * 0.5f);
            if (cover > 0.0f)
            {
                sum += source[clamp(farTap, int2(0, 0),
                                    int2(int(g_dof.width) - 1, int(g_dof.height) - 1))]
                           .rgb * cover;
                weightSum += cover;
                foregroundCoverSum += cover;
            }
        }
    }

    if (weightSum < 1e-4f)
    {
        output[center] = float4(sharp, 1.0f);
        return;
    }

    const float3 blurred = sum / weightSum;
    // 半径が 1 画素に満たないうちは混ぜない。ピント面が甘く見えるのを防ぐ。
    const float centerBlend = smoothstep(0.5f, max(g_dof.maxBlurPixels, 1.0f), centerRadius);
    const float foregroundBlend = saturate(foregroundCoverSum / 8.0f);
    const float blend = max(centerBlend, foregroundBlend);
    output[center] = float4(lerp(sharp, blurred, blend), 1.0f);
}
