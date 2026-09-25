// インポスターの焼き込み。1 方向ずつ、モデルを正射影でアトラスのマスへ描く。
// C++ 側は renderer/Impostor.cpp。方向とマスの対応は ImpostorCommon.hlsli。
//
// 出力は 3 枚（どれも RGBA8 UNORM）:
//   色     : rgb = ベースカラー（sRGB で符号化）、a = 覆い（アルファ抜き込みで 0 / 1）
//   法線   : rg = モデル空間の法線（8 面体）、b = 深度（手前ほど大きい）、a = ラフネス
//   色むら : r = 色むらを受ける割合（色むらを持つマテリアルで 1）、gba は予約（0）。
//            色むらそのものは焼かず、描画で株ごとに掛ける（ModelPreview.hlsl の PsImpostor）
// 焼いた後、抜けた画素へ近くの色を広げる（CsDilate）。ミップで縁が黒ずまないように。
#include "CompositeCommon.hlsli"
#include "ImpostorCommon.hlsli"

// Impostor.cpp の BakeConstants と一致させる。
struct BakeConstants {
    uint baseColorIndex, normalIndex, roughnessIndex, mapChannels;
    float3 baseColorTint; float roughnessValue;
    float2 colorAdjust; float brightness; float alphaCutoff;
    float3 center; float radius;
    // 撮るマスの番号。方向と画面の向きは ImpostorCommon.hlsli から求める（描画と同じ式）。
    uint2 frame; uint frames; uint fullSphere;
    uint flipNormalGreen; float variationWeight; uint2 padding;
};
ConstantBuffer<BakeConstants> g_bake : register(b1);

struct VertexInput { float3 position:POSITION; float3 normal:NORMAL; float4 tangent:TANGENT; float2 uv:TEXCOORD0; };
struct BakeInput { float4 clip:SV_POSITION; float3 position:POSITION; float3 normal:NORMAL; float4 tangent:TANGENT; float2 uv:TEXCOORD0; };
struct BakeOutput { float4 color:SV_Target0; float4 normalDepth:SV_Target1; float4 variation:SV_Target2; };

float BakeMapLod(uint index, float2 deltaX, float2 deltaY) {
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    float2 dimensions; float levels;
    map.GetDimensions(0, dimensions.x, dimensions.y, levels);
    const float2 dx = deltaX * dimensions, dy = deltaY * dimensions;
    return 0.5f * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-8f));
}
float4 BakeSample(uint index, float2 uv, float lod) {
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    return map.SampleLevel(g_samplerLinearWrap, uv, lod);
}

float3 BakeDirection() { return ImpostorFrameDirection(g_bake.frame, g_bake.frames, g_bake.fullSphere != 0); }

BakeInput VsBake(VertexInput input) {
    const float3 direction = BakeDirection();
    float3 right, up;
    ImpostorFrameBasis(direction, right, up);
    const float3 relative = input.position - g_bake.center;
    BakeInput output;
    // 正射影。中心から半径 radius の球がマスにちょうど収まる。深度は手前（カメラ側）ほど小さい。
    output.clip = float4(dot(relative, right) / g_bake.radius, dot(relative, up) / g_bake.radius,
                         0.5f - dot(relative, direction) / (2 * g_bake.radius), 1);
    output.position = input.position; output.normal = input.normal;
    output.tangent = input.tangent; output.uv = input.uv;
    return output;
}

BakeOutput PsBake(BakeInput input, bool frontFace:SV_IsFrontFace) {
    const float3 normalGeometric = normalize(input.normal);
    const float3 faceNormal = normalize(cross(ddx(input.position), ddy(input.position)));
    const float2 uv = input.uv, deltaX = ddx(uv), deltaY = ddy(uv);
    float3 baseColor = g_bake.baseColorTint;
    if (g_bake.baseColorIndex != kInvalidTextureIndex) {
        const float lod = BakeMapLod(g_bake.baseColorIndex, deltaX, deltaY);
        const float4 sampled = BakeSample(g_bake.baseColorIndex, uv, lod);
        // 斜めから撮るマスではミップが進んでアルファが薄まる。ModelPreview.hlsl の ClipAlpha と
        // 同じ補正（ミップ 1 段ごとに 0.25 倍ずつ持ち上げる）で、見かけの被覆をメッシュに揃える。
        if (g_bake.alphaCutoff > 0) clip(sampled.a * (1 + max(lod, 0) * 0.25f) - g_bake.alphaCutoff);
        baseColor *= sampled.rgb;
    }
    baseColor = AdjustBaseColor(baseColor, g_bake.colorAdjust.x, g_bake.colorAdjust.y, g_bake.brightness);
    float roughness = g_bake.roughnessValue;
    if (g_bake.roughnessIndex != kInvalidTextureIndex)
        roughness = SelectChannel(BakeSample(g_bake.roughnessIndex, uv, BakeMapLod(g_bake.roughnessIndex, deltaX, deltaY)),
                                  UnpackChannel(g_bake.mapChannels, TG_CHANNEL_SLOT_ROUGHNESS));
    float3 normal = normalGeometric;
    if (g_bake.normalIndex != kInvalidTextureIndex) {
        float3 sampled = BakeSample(g_bake.normalIndex, uv, BakeMapLod(g_bake.normalIndex, deltaX, deltaY)).rgb * 2 - 1;
        if (g_bake.flipNormalGreen != 0) sampled.y = -sampled.y;
        const float3 tangent = normalize(input.tangent.xyz - normalGeometric * dot(input.tangent.xyz, normalGeometric));
        const float3 bitangent = cross(normalGeometric, tangent) * input.tangent.w;
        normal = normalize(tangent * sampled.x + bitangent * sampled.y + normalGeometric * sampled.z);
    }
    // 裏面は ModelPreview.hlsl と同じく面に対して鏡映す。
    if (!frontFace) normal = reflect(normal, faceNormal);
    BakeOutput output;
    output.color = float4(LinearToSrgb(saturate(baseColor)), 1);
    const float depth = saturate(0.5f + dot(input.position - g_bake.center, BakeDirection()) / (2 * g_bake.radius));
    output.normalDepth = float4(EncodeImpostorNormal(normal) * 0.5f + 0.5f, depth, saturate(roughness));
    output.variation = float4(g_bake.variationWeight, 0, 0, 0);
    return output;
}

// --- 抜けた画素の色埋め ---------------------------------------------------------
// 覆いの無い画素へ、同じマスの中で最も近い覆いのある画素の色と法線を写す（覆いは 0 のまま）。
// 近くは 1 画素刻み、遠くは 4 画素刻みで探す。見つからなければ中立の値。
struct DilateConstants {
    uint colorIn, normalIn, colorOut, normalOut;
    uint width, height, tileSize, variationIn;
    uint variationOut; uint3 padding;
};
ConstantBuffer<DilateConstants> g_dilate : register(b0);

[numthreads(8, 8, 1)]
void CsDilate(uint3 id : SV_DispatchThreadID) {
    if (id.x >= g_dilate.width || id.y >= g_dilate.height) return;
    Texture2D<float4> colorIn = ResourceDescriptorHeap[g_dilate.colorIn];
    Texture2D<float4> normalIn = ResourceDescriptorHeap[g_dilate.normalIn];
    RWTexture2D<float4> colorOut = ResourceDescriptorHeap[g_dilate.colorOut];
    RWTexture2D<float4> normalOut = ResourceDescriptorHeap[g_dilate.normalOut];
    Texture2D<float4> variationIn = ResourceDescriptorHeap[g_dilate.variationIn];
    RWTexture2D<float4> variationOut = ResourceDescriptorHeap[g_dilate.variationOut];
    const int2 pixel = int2(id.xy);
    const float4 color = colorIn[pixel];
    if (color.a > 0.5f) {
        colorOut[pixel] = color;
        normalOut[pixel] = normalIn[pixel];
        variationOut[pixel] = variationIn[pixel];
        return;
    }
    const int2 tileMin = int2(id.xy / g_dilate.tileSize * g_dilate.tileSize);
    const int2 tileMax = tileMin + int(g_dilate.tileSize) - 1;
    int2 best = int2(-1, -1);
    int bestDistance = 0x7fffffff;
    [loop] for (int pass = 0; pass < 2 && best.x < 0; ++pass) {
        const int step = pass == 0 ? 1 : 4, reach = pass == 0 ? 8 : 64;
        [loop] for (int y = -reach; y <= reach; y += step)
            [loop] for (int x = -reach; x <= reach; x += step) {
                const int2 candidate = pixel + int2(x, y);
                if (any(candidate < tileMin) || any(candidate > tileMax)) continue;
                const int distance = x * x + y * y;
                if (distance >= bestDistance || colorIn[candidate].a <= 0.5f) continue;
                bestDistance = distance;
                best = candidate;
            }
    }
    if (best.x < 0) {
        colorOut[pixel] = float4(0.5f, 0.5f, 0.5f, 0);
        normalOut[pixel] = float4(0.5f, 0.5f, 0.5f, 1);
        variationOut[pixel] = 0;
        return;
    }
    colorOut[pixel] = float4(colorIn[best].rgb, 0);
    normalOut[pixel] = normalIn[best];
    variationOut[pixel] = variationIn[best];
}
