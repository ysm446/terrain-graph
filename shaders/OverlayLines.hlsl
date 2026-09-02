// ビューポートに重ねる 3D のガイド線（ハイトの範囲の枠など）。
//
// ImGui のオーバーレイと違い、シーンの深度でテストするのでメッシュの
// 向こう側は隠れる。トーンマップ後の表示用テクスチャへ、露出を通さない
// 表示色のまま描く（ギズモは画面上で一定の明るさに見えるべきもの）。
//
// 頂点バッファは使わない。線分の端点を定数バッファの配列で渡し、
// SV_VertexID で引く。LINELIST で 2 頂点 = 1 本。

#include "Common.hlsli"

// 端点の最大数（線分 64 本ぶん）。C++ 側の kOverlayLineMaxVertices と一致させること。
#define TG_OVERLAY_MAX_VERTICES 128

struct OverlayLineConstants
{
    float4x4 viewProjection;
    // rgb: 線の色（表示色）。a は全体の不透明度。
    float4 color;
    // xyz: ワールド座標、w: 端点ごとの不透明度（color.a に掛かる）。
    float4 positions[TG_OVERLAY_MAX_VERTICES];
};

ConstantBuffer<OverlayLineConstants> g_lines : register(b1);

struct VsOutput
{
    float4 clipPosition : SV_Position;
    float alpha : ALPHA;
};

VsOutput VsMain(uint vertexId : SV_VertexID)
{
    const float4 vertex = g_lines.positions[vertexId];

    VsOutput output;
    output.clipPosition = mul(g_lines.viewProjection, float4(vertex.xyz, 1.0f));
    output.alpha = vertex.w;
    return output;
}

float4 PsMain(VsOutput input) : SV_Target0
{
    return float4(g_lines.color.rgb, g_lines.color.a * input.alpha);
}
