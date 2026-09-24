#ifndef IMPOSTOR_COMMON_HLSLI
#define IMPOSTOR_COMMON_HLSLI

// インポスターの「方向 ↔ 画像のマス」の対応。焼き込み（ImpostorBake.hlsl）と
// 描画（ModelPreview.hlsl）で共有する。
//
// 方向は中心からカメラへ向かう向き（モデル空間、Y が上）。画像は frames × frames のマスで、
// マス (i, j) は格子点 uv = (i, j) / (frames - 1) * 2 - 1 の方向から撮る（端のマスが地平線や真下）。
// 半球は上半球だけを正方形全体に割り当てる（同じマス数で上からの解像度が倍になる）。
// 全球は 8 面体の展開で、下半球を四隅へ折り返す。

float ImpostorSign(float value) { return value >= 0 ? 1.0f : -1.0f; }

float3 ImpostorDirection(float2 uv, bool fullSphere) {
    if (fullSphere) {
        float3 n = float3(uv.x, 1 - abs(uv.x) - abs(uv.y), uv.y);
        if (n.y < 0) n.xz = (1 - abs(n.zx)) * float2(ImpostorSign(n.x), ImpostorSign(n.z));
        return normalize(n);
    }
    const float2 h = float2(uv.x + uv.y, uv.x - uv.y) * 0.5f;
    return normalize(float3(h.x, 1 - abs(h.x) - abs(h.y), h.y));
}

float2 ImpostorGridUv(float3 d, bool fullSphere) {
    if (fullSphere) {
        d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-6f);
        float2 uv = d.xz;
        if (d.y < 0) uv = (1 - abs(d.zx)) * float2(ImpostorSign(d.x), ImpostorSign(d.z));
        return uv;
    }
    // 下から見たときは地平線の方向で代用する。
    d.y = max(d.y, 0);
    d /= max(abs(d.x) + d.y + abs(d.z), 1e-6f);
    return float2(d.x + d.z, d.x - d.z);
}

// 方向 d から撮るときの画面の右と上。真上・真下の近くでは基準を替える（各マスの中で一貫していればよい）。
void ImpostorFrameBasis(float3 d, out float3 right, out float3 up) {
    const float3 reference = abs(d.y) > 0.999f ? float3(0, 0, -1) : float3(0, 1, 0);
    right = normalize(cross(reference, d));
    up = cross(d, right);
}

float3 ImpostorFrameDirection(uint2 frame, uint frames, bool fullSphere) {
    return ImpostorDirection(float2(frame) / float(frames - 1) * 2 - 1, fullSphere);
}

// 視線の向きに近い 3 マスと重み（格子の三角形の重心座標）。
struct ImpostorFrames {
    uint2 frame[3];
    float3 weight;
};
ImpostorFrames SelectImpostorFrames(float3 toCamera, uint frames, bool fullSphere) {
    const float last = float(frames - 1);
    const float2 grid = clamp((ImpostorGridUv(toCamera, fullSphere) * 0.5f + 0.5f) * last, 0, last);
    const float2 base = min(floor(grid), last - 1);
    const float2 f = grid - base;
    ImpostorFrames result;
    if (f.x + f.y <= 1) {
        result.frame[0] = uint2(base);
        result.frame[1] = uint2(base) + uint2(1, 0);
        result.frame[2] = uint2(base) + uint2(0, 1);
        result.weight = float3(1 - f.x - f.y, f.x, f.y);
    } else {
        result.frame[0] = uint2(base) + uint2(1, 1);
        result.frame[1] = uint2(base) + uint2(0, 1);
        result.frame[2] = uint2(base) + uint2(1, 0);
        result.weight = float3(f.x + f.y - 1, 1 - f.x, 1 - f.y);
    }
    return result;
}

// 法線の 8 面体符号化（全球の対応をそのまま使う）。[-1, 1]^2。
float2 EncodeImpostorNormal(float3 n) { return ImpostorGridUv(n, true); }
float3 DecodeImpostorNormal(float2 e) { return ImpostorDirection(e, true); }

#endif
