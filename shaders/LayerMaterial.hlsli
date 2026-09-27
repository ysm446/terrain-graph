#ifndef TG_LAYER_MATERIAL_HLSLI
#define TG_LAYER_MATERIAL_HLSLI
#include "CompositeCommon.hlsli"
struct LayerMaterialSlot {
    uint4 textures0; uint4 textures1;
    float4 color; float4 surface; float4 adjust;
    float4 mask; float4 breakup; float4 blend;
    // 轍・道路端（LayerMaterialGpu.h）。road0: 手入力の車線中央, タイヤ間隔, 帯の幅, ぼかし。
    // road1: 端の幅, 側（0 両側 / 1 左 / 2 右）, フラグ（bit0 車線に合わせる, bit1 対向にも）, 未使用。
    float4 road0; float4 road1;
};
struct LayerMaterialData {
    uint count; float blendRange; float displacementMeters; float pad;
    // 道路の文脈。幅（0 なら道路でない）, 車線数（進行方向）, 車線数（対向）, 左側通行なら 1。
    float4 road;
    LayerMaterialSlot slots[4];
};
// 中心 center、幅 width の帯。縁を feather でなだらかにする（road-material-editor の Band）。
float RoadBand(float x, float center, float width, float feather) {
    const float distance = abs(x - center) - width * 0.5f;
    if (feather <= 1e-5f) return distance <= 0 ? 1.0f : 0.0f;
    return saturate(1.0f - distance / feather);
}
// 轍（shape 0）と道路端（shape 1）。meters.x は左端からの横位置（m）。横位置は road-material-editor
// と同じく正が Left（左端が +幅/2、右端が -幅/2）で測る。車線は右端から並べ、走行側で
// 進行方向の車線がどちら側かが決まる（ComputeRoadLanes と同じ）。道路の文脈が無ければ 0。
float RoadShapeMask(LayerMaterialData data, LayerMaterialSlot s, float2 meters) {
    const float width = data.road.x;
    if (width <= 0) return 0;
    const float half = width * 0.5f;
    const float lateral = half - meters.x;
    if (s.mask.x == 1) {
        float fromEdge = half - abs(lateral);
        if (s.road1.y == 1) fromEdge = half - lateral;
        if (s.road1.y == 2) fromEdge = half + lateral;
        const float inner = fromEdge - s.road1.x;
        return s.road0.w <= 1e-5f ? (inner <= 0 ? 1.0f : 0.0f) : saturate(1.0f - inner / s.road0.w);
    }
    const uint flags = (uint)s.road1.z;
    const bool fromLanes = (flags & 1u) != 0, bothLanes = (flags & 2u) != 0;
    float value = 0;
    if (fromLanes) {
        const uint forward = max((uint)data.road.y, 1u), backward = (uint)data.road.z;
        const uint total = min(forward + backward, 16u);
        const bool leftHand = data.road.w != 0;
        const float laneWidth = width / float(total);
        const uint rightSideCount = leftHand ? backward : forward;
        for (uint i = 0; i < total; ++i) {
            const bool onRight = i < rightSideCount;
            const bool laneForward = leftHand ? !onRight : onRight;
            if (!bothLanes && !laneForward) continue;
            const float center = -half + laneWidth * (float(i) + 0.5f);
            value = max(value, RoadBand(lateral, center - s.road0.y * 0.5f, s.road0.z, s.road0.w));
            value = max(value, RoadBand(lateral, center + s.road0.y * 0.5f, s.road0.z, s.road0.w));
        }
    } else {
        [unroll] for (int lane = 0; lane < 2; ++lane) {
            if (lane == 1 && !bothLanes) break;
            const float center = lane == 0 ? s.road0.x : -s.road0.x;
            value = max(value, RoadBand(lateral, center - s.road0.y * 0.5f, s.road0.z, s.road0.w));
            value = max(value, RoadBand(lateral, center + s.road0.y * 0.5f, s.road0.z, s.road0.w));
        }
    }
    return value;
}
struct LayerMaterialSample { float3 color; float3 normal; float3 surface; float height; float4 coverage; };
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
                if (s.mask.x == 0 || s.mask.x == 1) {
                    mask = RoadShapeMask(data, s, meters);
                } else if (s.mask.x != 3) {
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
    result.coverage = coverage; result.coverage.x = 1;
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
