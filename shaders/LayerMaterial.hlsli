#ifndef TG_LAYER_MATERIAL_HLSLI
#define TG_LAYER_MATERIAL_HLSLI
#include "CompositeCommon.hlsli"
struct LayerMaterialSlot {
    uint4 textures0; uint4 textures1;
    float4 color; float4 surface; float4 adjust;
    float4 mask; float4 breakup; float4 blend;
};
struct LayerMaterialData {
    uint count; float blendRange; float displacementMeters; float pad;
    LayerMaterialSlot slots[4];
};
struct LayerMaterialSample { float3 color; float3 normal; float3 surface; float height; };
float4 SampleLayerMaterialMap(uint index, float2 uv, float footprint) {
    Texture2D<float4> map = ResourceDescriptorHeap[index];
    uint w, h; map.GetDimensions(w, h);
    const float lod = max(log2(max(footprint * max(w, h), 1.0f)), 0.0f);
    return map.SampleLevel(g_samplerLinearWrap, uv, lod);
}
// 材質内の合成。道路の形状・地形の下地の高さには依存しない。
LayerMaterialSample EvaluateLayerMaterialBase(LayerMaterialData data, float2 meters, float2 worldMeters, float2 footprintMeters, float2 xAxis, float2 yAxis) {
    LayerMaterialSample samples[4];
    float4 heights = 0.5f, coverage = 0, modes = 0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        LayerMaterialSlot s = data.slots[i];
        LayerMaterialSample v;
        v.color = s.color.rgb; v.surface = s.surface.xyz; v.normal = float3(0,0,1); v.height = 0.5f;
        if (i < data.count) {
            const float2 p = s.surface.w != 0 ? worldMeters : meters;
            const float2 uv = p / max(s.color.w, 0.01f);
            const float footprint = (s.surface.w != 0 ? footprintMeters.y : footprintMeters.x) / max(s.color.w, 0.01f);
            if (s.textures0.x != kInvalidTextureIndex) v.color *= SampleLayerMaterialMap(s.textures0.x, uv, footprint).rgb;
            v.color = AdjustBaseColor(v.color, s.adjust.x, s.adjust.y, s.adjust.z);
            if (s.textures0.y != kInvalidTextureIndex) {
                v.normal = SampleLayerMaterialMap(s.textures0.y, uv, footprint).rgb * 2 - 1;
                if (s.adjust.w != 0) v.normal.y = -v.normal.y;
                v.normal = normalize(v.normal);
                if (s.surface.w != 0) v.normal.xy = float2(dot(v.normal.xy, xAxis), dot(v.normal.xy, yAxis));
            }
            if (s.textures0.z != kInvalidTextureIndex) v.surface.x = SelectChannel(SampleLayerMaterialMap(s.textures0.z, uv, footprint), UnpackChannel(s.textures1.z, 0));
            if (s.textures0.w != kInvalidTextureIndex) v.surface.y = SelectChannel(SampleLayerMaterialMap(s.textures0.w, uv, footprint), UnpackChannel(s.textures1.z, 1));
            if (s.textures1.x != kInvalidTextureIndex) v.surface.z = SelectChannel(SampleLayerMaterialMap(s.textures1.x, uv, footprint), UnpackChannel(s.textures1.z, 2));
            if (s.textures1.y != kInvalidTextureIndex) v.height = SelectChannel(SampleLayerMaterialMap(s.textures1.y, uv, footprint), UnpackChannel(s.textures1.z, 3));
            float mask = 0;
            if (s.mask.x >= 0 && (s.textures1.w & 1u) != 0) {
                mask = 1;
                if (s.mask.x != 3) {
                    const float2 coord = s.mask.x == 2 ? float2(0, meters.y) : worldMeters;
                    const float noise = Fbm(coord / max(s.mask.y, 0.05f) + s.breakup.w, 5, 4096);
                    mask = smoothstep(s.mask.z - s.mask.w, s.mask.z + s.mask.w, noise);
                }
                const float breakup = Fbm(meters / max(s.breakup.z, 0.05f) + s.breakup.w + 17, 3, 4096);
                mask *= lerp(1.0f, breakup, s.breakup.y);
                if ((s.textures1.w & 2u) != 0) mask = 1 - mask;
                mask *= s.breakup.x;
                if (s.blend.y != 0) {
                    const float gate = smoothstep(s.blend.z - s.blend.w, s.blend.z + s.blend.w, heights.x);
                    mask *= s.blend.y == 1 ? gate : 1 - gate;
                }
            }
            coverage[i] = mask;
        }
        heights[i] = v.height; modes[i] = s.blend.x; samples[i] = v;
    }
    // road-material-editor の LayerHeightBlend と同じ重み。
    float4 maskWeights = 0, heightCoverage = coverage;
    [unroll] for (uint slot = 1; slot < 4; ++slot) if (modes[slot] == 0) {
        maskWeights[slot] = saturate(coverage[slot] + (heights[slot] - heights.x) * data.blendRange) * step(1e-6f, coverage[slot]);
        heightCoverage[slot] = 0;
    }
    float maskTotal = dot(maskWeights, 1.0f);
    if (maskTotal > 1) { maskWeights /= maskTotal; maskTotal = 1; }
    heightCoverage.x = saturate(1 - heightCoverage.y - heightCoverage.z - heightCoverage.w);
    const float4 score = heights + heightCoverage;
    const float peak = max(max(score.x, score.y), max(score.z, score.w));
    float4 weights = max(score - peak + max(data.blendRange, 1e-3f), 0) * step(1e-6f, heightCoverage);
    const float total = dot(weights, 1.0f);
    weights = maskWeights + (total > 1e-5f ? weights / total : float4(1,0,0,0)) * (1 - maskTotal);
    LayerMaterialSample result;
    result.color = 0; result.surface = 0; result.height = 0; result.normal = float3(0,0,1);
    [unroll] for (uint j = 0; j < 4; ++j) {
        result.color += samples[j].color * weights[j]; result.surface += samples[j].surface * weights[j]; result.height += heights[j] * weights[j];
        result.normal = ReorientNormal(result.normal, FlattenNormal(samples[j].normal, weights[j]));
    }
    return result;
}
// ハイト由来の実寸勾配と素材の法線をRNMで合わせる。
LayerMaterialSample EvaluateLayerMaterial(LayerMaterialData data, float2 meters, float2 worldMeters, float2 footprintMeters,
                                         float2 localPerMeter, float2 xAxis, float2 yAxis) {
    LayerMaterialSample result = EvaluateLayerMaterialBase(data, meters, worldMeters, footprintMeters, xAxis, yAxis);
    if (data.displacementMeters > 0) {
        const float stepMeters = max(footprintMeters.y, 0.001f);
        const float2 dx = float2(localPerMeter.x * stepMeters, 0), dy = float2(0, localPerMeter.y * stepMeters);
        const float hx0 = EvaluateLayerMaterialBase(data, meters - dx, worldMeters - xAxis * stepMeters, footprintMeters, xAxis, yAxis).height;
        const float hx1 = EvaluateLayerMaterialBase(data, meters + dx, worldMeters + xAxis * stepMeters, footprintMeters, xAxis, yAxis).height;
        const float hy0 = EvaluateLayerMaterialBase(data, meters - dy, worldMeters - yAxis * stepMeters, footprintMeters, xAxis, yAxis).height;
        const float hy1 = EvaluateLayerMaterialBase(data, meters + dy, worldMeters + yAxis * stepMeters, footprintMeters, xAxis, yAxis).height;
        const float2 gradient = float2(hx1 - hx0, hy1 - hy0) * data.displacementMeters / (2 * stepMeters);
        result.normal = ReorientNormal(normalize(float3(-gradient, 1)), result.normal);
    }
    return result;
}
#endif
