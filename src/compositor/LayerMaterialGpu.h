#pragma once
#include <cstdint>
namespace tg::compositor {
// HLSL LayerMaterialData と同じ配置。4層は合成材質内だけの上限。
struct LayerMaterialSlotGpu {
    uint32_t textures0[4]{};
    uint32_t textures1[4]{};
    float color[4]{};
    float surface[4]{};
    float adjust[4]{};
    float mask[4]{};
    float breakup[4]{};
    float blend[4]{};
};
struct LayerMaterialGpu {
    uint32_t count = 0;
    float blendRange = 0.2f;
    float displacementMeters = 0;
    float pad = 0;
    LayerMaterialSlotGpu slots[4]{};
};
static_assert(sizeof(LayerMaterialSlotGpu) == 128);
static_assert(sizeof(LayerMaterialGpu) == 528);
}
