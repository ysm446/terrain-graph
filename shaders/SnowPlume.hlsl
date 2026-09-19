// 稜線から風下へ伸びる雪煙（Snow Plume ノード）。
//
// 頂点バッファは使わない。地形を seedsPerSide² の格子に切り、各マスの種で Source マスクを引いて、
// 強い所からだけ帯を 1 本（sheets 枚）生やす。帯の中心線は頂点シェーダが毎フレーム組み立てる
// （風下へ伸び、持ち上がってから沈み、地形の下へは潜らない）。幅の向きは「中心線を軸にカメラへ回す」
// ので、横から見ても板が線に潰れない。
//
// 濃さはワールド座標で引く 4D の値ノイズ（空間 3 軸 + 時間）で作る。風下へ風速で流し、時間の軸を
// 進めても、loopSeconds ごとに完全に同じ絵へ戻る（継ぎ目のないループ。空間には周期が無い）。
//
// 大気の合成の後に、深度を読み比べて描く（深度バッファは束ねない）。雲との前後は、合成が残した
// 半解像度の雲（透過率と平均距離）を読み、雲の向こう側にあるぶんを透過率で薄めて決める。
#include "AtmosphereCommon.hlsli"

// ModelPreview.hlsl と同じ並び（C++ の SceneShadowData）。
struct SceneShadowData {
    float4x4 view;
    float4x4 matrices[4];
    uint4 indices;
    float4 splits, biases;
    float nearDistance, texel, blend; uint count;
};

// SnowPlume.cpp の SnowPlumeConstants と同じ並び。
struct SnowPlumeConstants {
    float4x4 viewProjection;
    float3 cameraPosition; float time;        // time は 0〜loopSeconds に巻いた秒
    float3 lightDirection; float lightIlluminance;
    float3 lightColor; float iblIntensity;
    uint maskIndex, heightIndex, depthIndex, irradianceIndex;
    float planeSize, heightScale, nearZ, farZ;
    float2 wind; float windSpeed; uint seedsPerSide;
    float threshold, coverage, lengthMeters, widthStart;
    float widthEnd, lift, sink, opacity;
    float puffSize, turbulence, gust, loopSeconds;
    float anisotropy; uint seed, sheets, useHeight;
    SceneShadowData shadows;
    AtmosphericParameters atmosphere;
    uint cloudNoiseIndex, atmosphericMode; float upwind, slopeFollow;
    // 雲との前後。大気の合成が残した半解像度の雲（a: 透過率）と距離（y: 雲の平均距離）。無効なら比べない。
    uint cloudIndex, cloudDepthIndex; float cloudFarDistance;
    // 環境光を雲あり / 雲なしで混ぜる値（地形と同じ。SampleAmbientIrradiance を参照）。
    uint clearIrradianceIndex;
    float ambientLow, ambientHigh, ambientOcclusion, pad0;
};
ConstantBuffer<SnowPlumeConstants> g_plume : register(b1);

static const uint kSegments = 20;
static const uint kInvalidIndex = 0xffffffffu;
// 帯の 1 区間（四角形）を三角形 2 枚で描く。x: 区間の始点 0 / 終点 1、y: 幅の左 -1 / 右 1。
static const float2 kCorners[6] = {
    float2(0, -1), float2(0, 1), float2(1, -1), float2(1, -1), float2(0, 1), float2(1, 1)};

uint HashUint(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float HashUnit(uint x) { return float(HashUint(x) & 0x00ffffffu) / 16777216.0; }

float TerrainHeight(float2 xz) {
    if (g_plume.useHeight == 0) return 0;
    Texture2D<float> height = ResourceDescriptorHeap[g_plume.heightIndex];
    const float2 uv = xz / g_plume.planeSize + 0.5;
    return (height.SampleLevel(g_samplerLinearClamp, uv, 0) - 0.5) * g_plume.heightScale;
}

// --- 地形の影（ModelPreview.hlsl の ModelVisibility と同じ手順）---------------
float CascadeShadow(float3 position, uint cascade) {
    const uint index = g_plume.shadows.indices[cascade];
    if (index == kInvalidIndex) return 1;
    const float4 clip = mul(g_plume.shadows.matrices[cascade], float4(position, 1));
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (clip.w <= 0 || any(uv < 0) || any(uv > 1) || ndc.z > 1) return 1;
    Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(index)];
    return ndc.z - g_plume.shadows.biases[cascade] <= shadowMap.SampleLevel(g_samplerPointClamp, uv, 0) ? 1 : 0;
}
float TerrainVisibility(float3 position) {
    if (g_plume.shadows.count == 0) return 1;
    if (g_plume.shadows.count == 1) return CascadeShadow(position, 0);
    const float distance = -mul(g_plume.shadows.view, float4(position, 1)).z;
    const uint lastCascade = g_plume.shadows.count - 1;
    if (distance > g_plume.shadows.splits[lastCascade]) return 1;
    uint cascade = 0;
    while (cascade < lastCascade && distance > g_plume.shadows.splits[cascade]) ++cascade;
    return CascadeShadow(position, cascade);
}

// --- 帯 1 本ぶんの性質 ------------------------------------------------------
struct Ribbon {
    float3 root;
    float2 along, across;   // 風下 / 横（水平の単位ベクトル）
    float length;
    float strength;         // Source の強さ（しきい値からの割合）
    float phase;            // 帯ごとの位相（ラジアン）
    float widthScale, lift, opacity;
    float gust;             // -1〜1。突風の今の強さ
    float upwind;           // 風上の助走（m）。root は稜線の点で、帯はそこから風上へこの長さだけ延びる
    float follow;           // 風下の斜面に沿って下がる割合（0〜1）
    uint hash;
};

float LoopPhase() { return 6.28318530718 * g_plume.time / g_plume.loopSeconds; }

// 種の間隔（m）。
float SeedSpacing() { return g_plume.planeSize / float(max(g_plume.seedsPerSide, 1u)); }

// 根元は種の間隔より広くして、隣の帯と必ず重ねる（発生点が点々と離れて見えないように）。
float Width(Ribbon r, float t) {
    const float start = max(g_plume.widthStart, 1.3 * SeedSpacing());
    return lerp(start, max(g_plume.widthEnd, start), pow(t, 0.8)) * r.widthScale;
}

// 帯の上の位置 p（風上の端 0〜先端 1）を、稜線からの距離 s（m。風上が負）に直す。
float AlongMeters(Ribbon r, float p) { return p * (r.upwind + r.length) - r.upwind; }

// 帯の幅。s は稜線からの距離。助走の区間は設定の幅まで細くする（斜面を這う地吹雪）。
// 種の間隔で広げた幅のまま地表に貼り付けると、凸凹の地形に切られて細片になり、筋に見える。
// 隣と重ねるための幅へは、稜線の手前で広げる。
float WidthAt(Ribbon r, float s) {
    const float width = Width(r, saturate(s / r.length));
    const float runUp = min(width, 1.5 * g_plume.widthStart * r.widthScale);
    return lerp(runUp, width, smoothstep(-0.5 * max(r.upwind, 1e-3), 0.0, s));
}

// 中心線。p は風上の端 0〜先端 1。時間は LoopPhase の整数倍でしか入れない（ループを閉じるため）。
// 稜線より風上（助走）は地表に貼り付き、稜線を越えた所で剥がれて、風下の谷へ覆いかぶさる。
float3 Centerline(Ribbon r, float p) {
    const float loopPhase = LoopPhase();
    const float s = AlongMeters(r, p);
    const float t = saturate(s / r.length);   // 稜線 0〜先端 1。助走の区間は 0
    const float meander = g_plume.turbulence * r.length * t *
        (0.10 * sin(r.phase + t * 4.0 - loopPhase * 2.0) + 0.04 * sin(r.phase + 1.7 + t * 9.0 - loopPhase * 3.0));
    const float2 xz = r.root.xz + r.along * s + r.across * meander;
    const float ground = TerrainHeight(xz);
    // 浮かせる量は設定の幅で決める（種の間隔で広げた幅を使うと、大きな地形で根元が稜線から何十 m も浮く）。
    const float nominalWidth = lerp(g_plume.widthStart, max(g_plume.widthEnd, g_plume.widthStart), pow(t, 0.8)) * r.widthScale;
    const float clearance = max(2.0, 1.5 + 0.2 * nominalWidth);
    if (s <= 0) return float3(xz.x, ground + clearance, xz.y);
    // 稜線を越えて持ち上がり、風下へ行くほど沈む。風下の斜面が落ちるぶんも、割合を掛けて追う
    // （追わないと、谷の上にまっすぐ浮いた帯になる）。
    const float rise = r.lift * (1.0 - exp(-t * 5.0));
    const float drop = g_plume.sink * t * t;
    const float slopeDrop = r.follow * max(r.root.y - ground, 0.0);
    const float bob = g_plume.turbulence * 0.04 * r.length * t * sin(r.phase + 2.3 + t * 6.0 - loopPhase * 2.0);
    const float y = r.root.y + clearance + rise - drop - slopeDrop + bob;
    // 風下の斜面へは潜らせない。
    return float3(xz.x, max(y, ground + clearance), xz.y);
}

bool MakeRibbon(uint instance, out Ribbon r) {
    r = (Ribbon)0;
    const uint sheets = max(g_plume.sheets, 1u);
    const uint seedIndex = instance / sheets;
    const uint sheet = instance % sheets;
    const uint n = g_plume.seedsPerSide;
    if (seedIndex >= n * n) return false;
    const uint2 cell = uint2(seedIndex % n, seedIndex / n);
    const uint hash = HashUint(seedIndex * 1973u + g_plume.seed * 9277u + 17u);
    const float2 jitter = float2(HashUnit(hash), HashUnit(hash ^ 0x9e3779b9u));
    // マスの中を 4 × 4 で探し、一番強い所を根元にする。Spindrift は稜線に沿った細い線なので、
    // マスの 1 点だけを引くと、マスより細い線をほとんど取りこぼす。
    Texture2D<float4> mask = ResourceDescriptorHeap[g_plume.maskIndex];
    float source = 0;
    float2 uv = (float2(cell) + 0.5) / float(n);
    [unroll] for (uint k = 0; k < 16; ++k) {
        const float2 offset = (float2(k % 4, k / 4) + 0.25 + 0.5 * jitter) / 4.0;
        const float2 candidate = (float2(cell) + offset) / float(n);
        const float value = mask.SampleLevel(g_samplerLinearClamp, candidate, 0).r;
        if (value > source) { source = value; uv = candidate; }
    }
    r.strength = saturate((source - g_plume.threshold) / max(1.0 - g_plume.threshold, 1e-3));
    if (r.strength <= 0 || HashUnit(hash * 3u + 1u) >= g_plume.coverage * smoothstep(0.0, 0.3, r.strength))
        return false;

    const float2 rootXz = (uv - 0.5) * g_plume.planeSize;
    // root は稜線の地表の点。浮かせる量は Centerline が足す。
    r.root = float3(rootXz.x, TerrainHeight(rootXz), rootXz.y);
    r.along = g_plume.wind;
    r.across = float2(g_plume.wind.y, -g_plume.wind.x);
    // 位相は稜線に沿った位置で決める。隣どうしが少しずつずれて揃い、揺れが稜線を波のように伝わる。
    // 使う側は位相に倍率を掛けず、定数を足してずらす（掛けると隣との差も倍になり、波が崩れる）。
    const float wave = max(6.0 * SeedSpacing(), 4.0 * g_plume.puffSize);
    r.phase = 6.28318530718 * (dot(rootXz, r.across) / wave + 0.15 * HashUnit(hash * 7u + 3u));
    r.hash = hash;
    const float loopPhase = LoopPhase();
    r.gust = 0.6 * sin(loopPhase + r.phase) + 0.4 * sin(2.0 * loopPhase + r.phase + 1.3);
    const float lengthJitter = 0.9 + 0.2 * HashUnit(hash * 5u + 2u);
    r.length = g_plume.lengthMeters * lengthJitter * (1.0 + 0.3 * g_plume.gust * r.gust);
    // 2 枚目以降は、上に薄く広い層を重ねる（厚みと奥行きを出す）。
    const float layer = float(sheet);
    r.widthScale = 1.0 + 0.35 * layer;
    r.lift = g_plume.lift * (1.0 + 0.6 * layer);
    r.opacity = sheet == 0 ? 1.0 : 0.55 / layer;
    r.phase += layer * 2.1;
    r.length *= 1.0 + 0.2 * layer;
    // 上の層ほど助走が短く、斜面も追わない（下の層が斜面を覆い、上の層がまっすぐ流れて、楔の形になる）。
    r.upwind = g_plume.upwind / (1.0 + layer);
    r.follow = g_plume.slopeFollow / (1.0 + layer);
    return true;
}

struct VsOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    // x: 稜線からの距離（m。風上の助走は負）、y: 幅の中の位置 -1〜1
    float2 ribbon : TEXCOORD1;
    // x: t（稜線 0〜先端 1。助走は負）、y: 帯の不透明度、z: 風上の端からの距離（m）、w: 帯の幅（m）
    float4 params : TEXCOORD2;
    // x: 太陽の見え具合（地形の影と雲影）、y: 帯の軸に沿って覗き込むときに薄める係数
    float2 light : TEXCOORD3;
    // 濃さのノイズを引く位置。帯を軸に近い向きから見るときだけ、ワールド座標を軸の向きに縮める
    float3 noisePosition : TEXCOORD4;
};

VsOutput VsMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    VsOutput output = (VsOutput)0;
    Ribbon r;
    if (!MakeRibbon(instanceId, r)) {
        // 出さない帯は面積 0 の三角形にする（ラスタライザで捨てられる）。
        output.position = float4(2, 2, 2, 1);
        return output;
    }
    const float2 corner = kCorners[vertexId % 6];
    const float p = (float(vertexId / 6) + corner.x) / float(kSegments);
    const float s = AlongMeters(r, p);
    const float3 center = Centerline(r, p);
    // 帯の軸（風上の端→先端）を軸にカメラへ回す。風の向きに沿って覗き込むときだけ横向きの軸に落とす。
    // **向きは帯 1 本で 1 つに決める。**頂点ごとの接線とカメラの向きで決めると、帯に沿って覗き込む所で
    // 隣の頂点と向きが大きく回り、四角形がねじれて折り重なる（折り目が二重に合成されて筋になり、
    // 折り目は幅の縁ではないので縁のフェードも効かない）。
    const float3 rootCenter = Centerline(r, 0.0);
    const float3 tipCenter = Centerline(r, 1.0);
    const float3 axis = normalize(tipCenter - rootCenter);
    const float3 toCamera = normalize(g_plume.cameraPosition - 0.5 * (rootCenter + tipCenter));
    float3 side = cross(axis, toCamera);
    const float sideLength = length(side);
    side = sideLength > 1e-4 ? side / sideLength : float3(r.across.x, 0, r.across.y);
    const float width = WidthAt(r, s);
    const float3 world = center + side * (corner.y * 0.5 * width);

    output.position = mul(g_plume.viewProjection, float4(world, 1));
    output.world = world;
    output.ribbon = float2(s, corner.y);
    const float gustOpacity = 1.0 + 0.4 * g_plume.gust * r.gust;
    output.params = float4(s / r.length, g_plume.opacity * r.strength * r.opacity * gustOpacity, s + r.upwind, width);
    // 地形の影は、中心線から少し持ち上げた点を幅の向きに 3 つ引いて均す。影は点で引く 0 / 1 なので、
    // 地表のすぐ上を通る中心線の 1 点だけで決めると、頂点ごとに明暗がばらつき、幅の向きの縞（毛羽）になる。
    const float3 shadowCenter = center + float3(0, max(0.15 * width, 6.0), 0);
    float visibility = (TerrainVisibility(shadowCenter) +
                        TerrainVisibility(shadowCenter + side * (0.3 * width)) +
                        TerrainVisibility(shadowCenter - side * (0.3 * width))) / 3.0;
    if (g_plume.atmosphericMode != 0)
        visibility *= CloudShadow(center, g_plume.atmosphere, g_plume.cloudNoiseIndex);
    output.light = float2(visibility, smoothstep(0.3, 0.7, sideLength));
    // 帯を軸に近い向きから見ると、板が視線に対して斜めになり、画面の上で模様が軸の向きに押し潰されて
    // 毛のような筋になる（いちばん粗い層まで潰れるので、細かい層を抜いても消えない）。
    // ノイズを引く位置を、軸の向きに「見えている角度の sin」だけ縮めておくと、画面の上で等方になる。
    // 横から見る帯（sin ≈ 1）は変わらないので、重なった帯どうしで模様が続く性質は保たれる。
    // 縮めすぎると帯の中で模様がほぼ一様になり、帯の輪郭だけがヒレのように並んで見えるので、下限を置く
    // （それより浅い角度の帯は、上の係数で消える）。
    const float3 middle = 0.5 * (rootCenter + tipCenter);
    const float alongAxis = dot(world - middle, axis);
    output.noisePosition = world - axis * (alongAxis * (1.0 - clamp(sideLength, 0.4, 1.0)));
    return output;
}

// --- 時間方向に閉じた 4D の値ノイズ ------------------------------------------
// 空間の 3 軸（x: 風下、y: 横、z: 高さ）と時間の軸 w。w を periodW で巻き、1 周するごとに
// x を shiftX 格子ぶんずらして同じ値へ戻す：
//   N(x - shiftX, y, z, w + periodW) = N(x, y, z, w)
// 風で x を 1 ループに shiftX 格子流し、w を periodW 進めると、ループの終わりが始まりと一致する。
// 空間には周期が無い（流れた距離ごとに同じ模様が並ぶことがない）。時間を空間の軸に混ぜると、
// 巻き方のせいで高さ方向が風下方向のずらした写しになり、斜めの筋が出る。
float LatticeValue(int3 p, int w, int shiftX, int periodW) {
    const int wraps = w >= 0 ? w / periodW : -((-w + periodW - 1) / periodW);
    p.x += shiftX * wraps;
    w -= periodW * wraps;
    return HashUnit(uint(p.x) * 73856093u ^ uint(p.y) * 19349663u ^ uint(p.z) * 83492791u ^ uint(w) * 2654435761u);
}
float NoiseSlice(int3 i, float3 u, int w, int shiftX, int periodW) {
    const float x00 = lerp(LatticeValue(i, w, shiftX, periodW), LatticeValue(i + int3(1, 0, 0), w, shiftX, periodW), u.x);
    const float x10 = lerp(LatticeValue(i + int3(0, 1, 0), w, shiftX, periodW), LatticeValue(i + int3(1, 1, 0), w, shiftX, periodW), u.x);
    const float x01 = lerp(LatticeValue(i + int3(0, 0, 1), w, shiftX, periodW), LatticeValue(i + int3(1, 0, 1), w, shiftX, periodW), u.x);
    const float x11 = lerp(LatticeValue(i + int3(0, 1, 1), w, shiftX, periodW), LatticeValue(i + int3(1, 1, 1), w, shiftX, periodW), u.x);
    return lerp(lerp(x00, x10, u.y), lerp(x01, x11, u.y), u.z);
}
float LoopNoise(float3 p, float w, int shiftX, int periodW) {
    const float3 cell = floor(p);
    const float3 f = p - cell;
    const float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float wCell = floor(w);
    const float fw = w - wCell;
    const float uw = fw * fw * fw * (fw * (fw * 6.0 - 15.0) + 10.0);
    const int3 i = int3(cell);
    const int iw = int(wCell);
    return lerp(NoiseSlice(i, u, iw, shiftX, periodW), NoiseSlice(i, u, iw + 1, shiftX, periodW), uw);
}

// 雪煙の濃さ（0〜1）。**ワールド座標の 3 軸（風下・横・高さ）で引く**ので、重なった帯どうしで
// 模様が続き、1 枚のつながった雪煙に見える。x を風速で流し、w を時間で進める。
// 細かい層ほど w が速く進む（ちぎれて変わる）。
float PlumeDensity(float3 world) {
    const float phase = g_plume.time / g_plume.loopSeconds;
    // 1 ループで流れる距離を格子の整数個に丸める。風速はわずかにずれるが、ループが閉じる。
    // 0 でもよい（無風なら流さず、時間の軸だけで変わる）。
    const int shiftX = max(0, int(round(g_plume.windSpeed * g_plume.loopSeconds / g_plume.puffSize)));
    const int periodW = 3;
    const float along = dot(world.xz, g_plume.wind);
    const float across = dot(world.xz, float2(g_plume.wind.y, -g_plume.wind.x));
    float3 p = float3(along / g_plume.puffSize - float(shiftX) * phase,
                      across / g_plume.puffSize,
                      world.y / g_plume.puffSize);
    const float w = float(periodW) * phase;
    // 大きな塊で位置をゆがめ、格子に沿った筋に見えないようにする。
    const float warp = LoopNoise(p + float3(0, 31.7, 0), w, shiftX, periodW);
    p.xz += (warp - 0.5) * float2(1.5, 0.8) * g_plume.turbulence;
    float sum = 0, amplitude = 0.55, norm = 0;
    [unroll] for (int octave = 0; octave < 4; ++octave) {
        const float frequency = float(1u << uint(octave));
        sum += amplitude * LoopNoise(p * frequency, w * frequency, shiftX << octave, periodW << octave);
        norm += amplitude;
        amplitude *= 0.5;
    }
    return sum / norm;
}

float HenyeyGreenstein(float cosTheta, float g) {
    const float denominator = 1.0 + g * g - 2.0 * g * cosTheta;
    return (1.0 - g * g) / (4.0 * kPi * denominator * sqrt(denominator));
}

float LinearDepth(float z) {
    return g_plume.nearZ * g_plume.farZ / (g_plume.farZ - z * (g_plume.farZ - g_plume.nearZ));
}

// 雲の向こう側にある雪煙を、雲の透過率で薄める係数（1 で手前、雲の透過率で奥）。
// 雪煙は大気の合成の後に重ねるので、何もしないと雲の向こうの稜線の雪煙が雲を突き抜けて見える。
// 雲は体積なので面の深度は無い。合成が残した「雲の平均距離」を境に、幅を持たせて切り替える。
// 雲は地形の深度までしか積分されていないが、雪煙は地形より手前にしか描かないので、間の雲は必ず入っている。
float CloudOcclusion(float2 pixel, float2 screenSize, float plumeDistance) {
    if (g_plume.cloudIndex == kInvalidIndex || g_plume.cloudDepthIndex == kInvalidIndex) return 1;
    Texture2D<float4> clouds = ResourceDescriptorHeap[g_plume.cloudIndex];
    Texture2D<float2> distances = ResourceDescriptorHeap[g_plume.cloudDepthIndex];
    const float2 uv = pixel / screenSize;
    const float transmittance = saturate(clouds.SampleLevel(g_samplerLinearClamp, uv, 0).a);
    // 平均距離は補間しない（寄与なしの 0 と混ざると、雲の縁で距離が手前へ寄る）。
    uint2 halfSize;
    distances.GetDimensions(halfSize.x, halfSize.y);
    float cloudDistance = distances.Load(int3(min(uint2(pixel * 0.5), halfSize - 1), 0)).y;
    // 手前の積分に寄与が無く、遠景パスの雲だけが掛かっているとき。
    if (cloudDistance <= 0) cloudDistance = g_plume.cloudFarDistance;
    if (cloudDistance <= 0) return 1;
    const float band = max(0.2 * cloudDistance, 200.0);
    const float behind = smoothstep(cloudDistance - band, cloudDistance + band, plumeDistance);
    return lerp(1.0, transmittance, behind);
}

float4 PsMain(VsOutput input) : SV_Target {
    // 地形より奥なら捨て、手前でも近い所は柔らかく消す（地面に刺さった板の縁を見せない）。
    Texture2D<float> depth = ResourceDescriptorHeap[g_plume.depthIndex];
    const float sceneZ = depth.Load(int3(input.position.xy, 0));
    if (input.position.z >= sceneZ) discard;
    const float sceneDistance = sceneZ >= 1.0 ? 1e9 : LinearDepth(sceneZ);
    const float width = input.params.w;
    // 柔らかく消す距離は、稜線の近くでは短くする。幅に比例させたままだと、幅の広い帯（大きな地形）で
    // 稜線から何十 m も雪煙が消え、地面と雪煙の間に隙間ができて、空中から湧いたように見える。
    const float softDistance = max(lerp(0.08, 0.25, smoothstep(0.0, 0.5, input.params.x)) * width, 3.0);
    const float soft = saturate((sceneDistance - LinearDepth(input.position.z)) / softDistance);

    const float t = saturate(input.params.x);
    const float density = PlumeDensity(input.noisePosition);
    // 輪郭をノイズで崩し、先へ行くほど濃い所だけが残る（ちぎれて消えていく）。
    // 帯の輪郭は重なった隣の帯と揃わないので、弱めに効かせ、形はワールドのノイズに任せる
    // （輪郭が強いと、帯の縁の弧が何本も並んで刷毛目に見える）。
    // 板の端（幅の縁と根元）へは、濃さに関わらず広く滑らかに 0 へ落とす。ノイズはその途中を崩すだけ
    // （濃い塊が高いアルファのまま板の端に届くと、板の直線の切れ目が見える）。
    // 濃い所ほど外まで残り、薄い所ほど内へ削れる（puffs と同じ向き）。
    const float profile = 1.0 - smoothstep(0.3, 1.0, abs(input.ribbon.y));
    const float edge = smoothstep(0.0, 0.6, profile + (density - 0.5) * 1.2 * (0.4 + g_plume.turbulence)) *
                       smoothstep(0.0, 0.35, profile);
    // 風上の端も同じ。助走の区間で立ち上がり、**稜線でいちばん濃くなる**（濃い塊は助走の途中から、
    // 薄い所は稜線の手前で立ち上がりきる）。助走が無いときは、帯の長さの 1 割ほどで立ち上げる。
    const float upwind = input.params.z - input.ribbon.x;
    const float fadeDistance = max(upwind, 0.12 * g_plume.lengthMeters) * lerp(1.0, 0.45, saturate((density - 0.35) * 3.0));
    const float fadeIn = smoothstep(0.0, fadeDistance, input.params.z);
    // 稜線を越えたら風下へ薄まり続ける（本物の雪煙は稜線がいちばん濃い）。先端では必ず 0 へ落とす。
    const float fadeOut = exp(-1.1 * t) * (1.0 - smoothstep(0.6, 1.0, t + (density - 0.5) * 0.4));
    const float puffs = saturate((density - lerp(0.3, 0.52, t)) * 3.0);
    uint2 screenSize;
    depth.GetDimensions(screenSize.x, screenSize.y);
    const float occlusion = CloudOcclusion(input.position.xy, float2(screenSize), length(input.world - g_plume.cameraPosition));
    const float alpha = saturate(input.params.y * edge * fadeIn * fadeOut * puffs * soft * input.light.y * occlusion);
    if (alpha < 0.002) discard;

    // 太陽は前方散乱寄り（逆光で縁が光る）と弱い後方散乱の 2 つの山。
    // 等方なら 4 × HG = 1/π で、白いランバート面と同じ明るさになる。
    const float3 viewRay = normalize(input.world - g_plume.cameraPosition);
    const float cosTheta = dot(viewRay, normalize(g_plume.lightDirection));
    const float phase = lerp(HenyeyGreenstein(cosTheta, -0.2), HenyeyGreenstein(cosTheta, g_plume.anisotropy), 0.7);
    const float albedo = 0.9;
    // 濃い所は自分の影で少し暗い。
    const float selfShadow = lerp(1.0, 0.7, puffs);
    float3 radiance = g_plume.lightColor * g_plume.lightIlluminance * input.light.x * 4.0 * phase * selfShadow;
    if (g_plume.irradianceIndex != kInvalidIndex) {
        // 雲海の上の稜線の雪煙は、雲なしの環境で照らす。
        radiance += SampleAmbientIrradiance(g_plume.irradianceIndex, g_plume.clearIrradianceIndex, float3(0, 1, 0),
                                            input.world.y, g_plume.ambientLow, g_plume.ambientHigh,
                                            g_plume.ambientOcclusion) * g_plume.iblIntensity;
    }
    return float4(min(radiance * albedo, 65000.0), alpha);
}
