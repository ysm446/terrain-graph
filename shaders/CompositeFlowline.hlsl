// 粒子の流跡をマスクへ蓄積する。Height は読み取り専用。
#include "CompositeCommon.hlsli"

struct FlowlineConstants {
    uint4 textures; // Height SRV、Mask UAV、粒子 UAV、強さ UAV
    uint4 inputs;   // Source SRV、Outflow SRV、加算 UAV、flags
    uint4 grid;     // マスク解像度、粒子格子幅、粒子数、通過セル UAV
    float4 physics; // セル幅 m、Height m、速度保持率、回避率
    float4 flow;    // 強さ、分散、最小傾斜 rad、遷移幅 rad
};
ConstantBuffer<FlowlineConstants> g_flow : register(b1);
static const float kFlowlinePrecision = 65536.0f;

int2 FlowlineCell(int2 p) { return clamp(p, 0, int(g_flow.grid.x) - 1); }
float FlowlineHeight(int2 p) {
    Texture2D<float> height = ResourceDescriptorHeap[g_flow.textures.x];
    return height.Load(int3(FlowlineCell(p), 0)) * g_flow.physics.y;
}
float FlowlineInput(uint index, int2 p, float fallback) {
    if (index == kInvalidTextureIndex) return fallback;
    Texture2D<float> mask = ResourceDescriptorHeap[index];
    return mask.SampleLevel(g_samplerLinearClamp, (float2(p) + 0.5f) / g_flow.grid.x, 0);
}

[numthreads(8, 8, 1)]
void CsClear(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_flow.grid.x)) return;
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_flow.textures.y];
    RWTexture2D<uint> additions = ResourceDescriptorHeap[g_flow.inputs.z];
    mask[id.xy] = 0;
    additions[id.xy] = 0;
}

[numthreads(8, 8, 1)]
void CsSeed(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_flow.grid.y)) return;
    uint index = id.y * g_flow.grid.y + id.x;
    RWTexture2D<float4> particles = ResourceDescriptorHeap[g_flow.textures.z];
    RWTexture2D<float> strengths = ResourceDescriptorHeap[g_flow.textures.w];
    RWTexture2D<uint> deposits = ResourceDescriptorHeap[g_flow.grid.w];
    strengths[id.xy] = 0;
    deposits[id.xy] = 0xffffffffu;
    particles[id.xy] = 0;
    if (index >= g_flow.grid.z) return;
    float3 seed = float3(index % g_flow.grid.x, index / g_flow.grid.x, 0);
    float2 uv = frac(sin(float2(dot(seed, float3(127.141f, 311.742f, 251.523f)),
                               dot(seed, float3(269.513f, 183.357f, 524.013f)))) * 621.5153f);
    float2 p = uv * g_flow.grid.x;
    float source = max(0.0f, FlowlineInput(g_flow.inputs.x, FlowlineCell(int2(p)), 1.0f));
    if ((g_flow.inputs.w & 1u) != 0) source = saturate(source);
    strengths[id.xy] = isfinite(source) ? source * g_flow.flow.x : 0;
    particles[id.xy] = float4(p, 0, 0);
}

// 固定小数でステップごとに集計する。極端な入力でも uint を周回させない。
void FlowlineAdd(int2 p, float value) {
    RWTexture2D<uint> additions = ResourceDescriptorHeap[g_flow.inputs.z];
    uint amount = uint(min(max(value, 0.0f) * kFlowlinePrecision + 0.5f, 4294967040.0f));
    uint expected, observed;
    InterlockedAdd(additions[p], 0, expected);
    [allow_uav_condition] while (true) {
        uint desired = expected + min(amount, 0xffffffffu - expected);
        InterlockedCompareExchange(additions[p], expected, desired, observed);
        if (observed == expected) break;
        expected = observed;
    }
}

[numthreads(8, 8, 1)]
void CsStep(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_flow.grid.y) || id.y * g_flow.grid.y + id.x >= g_flow.grid.z) return;
    RWTexture2D<float4> particles = ResourceDescriptorHeap[g_flow.textures.z];
    RWTexture2D<float> strengths = ResourceDescriptorHeap[g_flow.textures.w];
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_flow.textures.y];
    RWTexture2D<uint> deposits = ResourceDescriptorHeap[g_flow.grid.w];
    deposits[id.xy] = 0xffffffffu;
    float strength = strengths[id.xy];
    if (strength <= 0 || !isfinite(strength)) return;
    float4 particle = particles[id.xy];
    int2 p = FlowlineCell(int2(particle.xy));
    float height = FlowlineHeight(p);
    float level = height + g_flow.flow.y * mask[p];
    float minimum = height + 1.0f;
    float2 gradient = 0;
    float slope = 0;
    [unroll] for (int axis = 0; axis < 2; ++axis) {
        [unroll] for (int sign = -1; sign <= 1; sign += 2) {
            int2 q = FlowlineCell(p + (axis == 0 ? int2(sign, 0) : int2(0, sign)));
            float neighbor = FlowlineHeight(q);
            float surface = neighbor + g_flow.flow.y * mask[q];
            gradient[axis] += sign * surface;
            slope = max(slope, abs(surface - level));
            minimum = min(minimum, neighbor);
        }
    }
    if (minimum > height && (g_flow.inputs.w & 2u) == 0) { strengths[id.xy] = 0; return; }
    float gradientLength = length(gradient);
    float2 normal = gradientLength > 1e-12f ? -gradient / gradientLength : float2(0, 0);
    float2 velocity = particle.zw * g_flow.physics.z - gradient;
    velocity -= normal * max(0.0f, dot(velocity, normal)) * 0.9999f * g_flow.physics.w;
    float speed = length(velocity);
    if (speed <= 1e-12f || !isfinite(speed)) { strengths[id.xy] = 0; return; }
    float2 position = particle.xy + velocity / speed;
    if (any(position < 0) || any(position > float(g_flow.grid.x - 1))) { strengths[id.xy] = 0; return; }
    float angle = atan(slope / g_flow.physics.x);
    float weight = angle >= g_flow.flow.z ? 1.0f :
        (g_flow.flow.w > 0 ? saturate(1 - (g_flow.flow.z - angle) / g_flow.flow.w) : 0.0f);
    FlowlineAdd(p, strength * weight);
    if (weight > 0) deposits[id.xy] = uint(p.y) * g_flow.grid.x + uint(p.x);
    float outflow = FlowlineInput(g_flow.inputs.y, p, 0.0f);
    strengths[id.xy] = strength * saturate(1 - outflow);
    particles[id.xy] = float4(position, velocity);
}

[numthreads(8, 8, 1)]
void CsCommit(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_flow.grid.y) || id.y * g_flow.grid.y + id.x >= g_flow.grid.z) return;
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_flow.textures.y];
    RWTexture2D<uint> additions = ResourceDescriptorHeap[g_flow.inputs.z];
    RWTexture2D<uint> deposits = ResourceDescriptorHeap[g_flow.grid.w];
    uint cell = deposits[id.xy];
    if (cell == 0xffffffffu) return;
    uint2 p = uint2(cell % g_flow.grid.x, cell / g_flow.grid.x);
    // 加算は前パスで完了済み。同じセルでは最初の粒子だけが非ゼロ値を受け取る。
    uint amount;
    InterlockedExchange(additions[p], 0, amount);
    if (amount > 0) mask[p] += float(amount) / kFlowlinePrecision;
}

[numthreads(8, 8, 1)]
void CsFinish(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= g_flow.grid.x)) return;
    RWTexture2D<float> mask = ResourceDescriptorHeap[g_flow.textures.y];
    float value = mask[id.xy];
    if ((g_flow.inputs.w & 4u) != 0) value /= 1 + value;
    if ((g_flow.inputs.w & 8u) != 0) value += FlowlineInput(g_flow.inputs.y, int2(id.xy), 0.0f);
    mask[id.xy] = value;
}
