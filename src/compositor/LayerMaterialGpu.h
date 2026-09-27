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
    // 道路の座標が要るマスク（轍・道路端）。road0: 車線中央からの距離（手入力のとき）,
    // タイヤ間隔, 帯の幅, 縁のぼかし。road1: 端の幅, 側（0 両側 / 1 左 / 2 右）,
    // フラグ（bit0 車線に合わせる、bit1 対向にも置く）, 未使用。
    float road0[4]{};
    float road1[4]{};
};
struct LayerMaterialGpu {
    uint32_t count = 0;
    float blendRange = 0.2f;
    float displacementMeters = 0;
    float pad = 0;
    // 道路の文脈（Road Mesh の路面のときだけ）。幅（m、0 なら道路でない）, 車線数（進行方向）,
    // 車線数（対向）, 左側通行なら 1。轍・道路端のマスクは幅が 0 だと覆わない。
    float road[4]{};
    LayerMaterialSlotGpu slots[4]{};
};
static_assert(sizeof(LayerMaterialSlotGpu) == 160);
static_assert(sizeof(LayerMaterialGpu) == 672);
}
