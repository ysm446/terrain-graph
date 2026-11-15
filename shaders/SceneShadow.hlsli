#ifndef TG_SCENE_SHADOW_HLSLI
#define TG_SCENE_SHADOW_HLSLI

// シーンのカスケード影（C++ の ShadowCascades.h の SceneShadowData と同じ並び）。
// 配置モデル（ModelPreview.hlsl）、生成メッシュ（GeneratedMesh.hlsl）、雪煙（SnowPlume.hlsl）で共有する。
// 地形（MeshPbr.hlsl）は定数の並びも下駄の式（tan に比例）も違うので、別に持っている。
struct SceneShadowData {
    float4x4 view;
    float4x4 matrices[4];
    uint4 indices;
    float4 splits, biases;
    float nearDistance, texel, blend; uint count;
};

#endif  // TG_SCENE_SHADOW_HLSLI

// 3x3 の PCF と、隣の区間との混ぜ合わせ。
// **2 回インクルードする。** 1 回目は上の構造体だけ（定数バッファの宣言に要る）。定数バッファを宣言した後、
// TG_SCENE_SHADOWS をその中の SceneShadowData（例: g_model.shadows）へ定義してからもう一度インクルードすると、
// 下の関数が出る。構造体を引数で渡すと 4 つの行列を画素ごとに複製するので、マクロで直接引く。
#if defined(TG_SCENE_SHADOWS) && !defined(TG_SCENE_SHADOW_FUNCTIONS)
#define TG_SCENE_SHADOW_FUNCTIONS
float SceneCascadeShadow(float3 position, float nDotL, uint cascade) {
    const uint index = TG_SCENE_SHADOWS.indices[cascade];
    if (index == 0xFFFFFFFFu) return 1;
    const float4 clip = mul(TG_SCENE_SHADOWS.matrices[cascade], float4(position, 1));
    const float3 ndc = clip.xyz / clip.w;
    const float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
    if (clip.w <= 0 || any(uv < 0) || any(uv > 1) || ndc.z > 1) return 1;
    Texture2D<float> shadowMap = ResourceDescriptorHeap[NonUniformResourceIndex(index)];
    float visibility = 0;
    const float bias = TG_SCENE_SHADOWS.biases[cascade] * (1 + 3 * (1 - saturate(nDotL)));
    [unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x)
        visibility += ndc.z - bias <= shadowMap.SampleLevel(g_samplerPointClamp, uv + float2(x, y) * TG_SCENE_SHADOWS.texel, 0) ? 1 : 0;
    return visibility / 9;
}
// カメラ前方距離で区間を選び、隣の区間と重なる範囲で混ぜる。
float SceneShadowVisibility(float3 position, float nDotL) {
    if (TG_SCENE_SHADOWS.count == 0) return 1;
    if (TG_SCENE_SHADOWS.count == 1) return SceneCascadeShadow(position, nDotL, 0);
    const float distance = -mul(TG_SCENE_SHADOWS.view, float4(position, 1)).z;
    const uint lastCascade = TG_SCENE_SHADOWS.count - 1;
    if (distance > TG_SCENE_SHADOWS.splits[lastCascade]) return 1;
    uint cascade = 0;
    while (cascade < lastCascade && distance > TG_SCENE_SHADOWS.splits[cascade]) ++cascade;
    const float visibility = SceneCascadeShadow(position, nDotL, cascade);
    const float start = cascade == 0 ? TG_SCENE_SHADOWS.nearDistance : TG_SCENE_SHADOWS.splits[cascade - 1];
    const float end = TG_SCENE_SHADOWS.splits[cascade];
    const float blendStart = end - (end - start) * TG_SCENE_SHADOWS.blend;
    if (distance <= blendStart) return visibility;
    const float next = cascade < lastCascade ? SceneCascadeShadow(position, nDotL, min(cascade + 1, lastCascade)) : 1;
    return lerp(visibility, next, smoothstep(blendStart, end, distance));
}
#endif  // TG_SCENE_SHADOW_FUNCTIONS
