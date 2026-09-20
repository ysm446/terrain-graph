#include "compositor/MaterialStack.h"

namespace tg::compositor {

MaterialLayer MaterialStack::MakeBaseLayer() {
    MaterialLayer layer;
    layer.name = "Base";
    // **ノイズを載せない。** 既定の ValueSource::Noise のままだと、
    // 変位量が 0 でも法線に模様が出て「まっさらな球」に見えない。
    layer.heightSource = ValueSource::Constant;
    layer.heightBase = kHeightPivot;
    // 下地なのでマスクは効かないが、値も既定の「全面」で揃えておく。
    layer.mask.source = MaskSource::Constant;
    layer.mask.constant = 1.0f;
    return layer;
}

MaterialStack::MaterialStack() {
    // 起動直後と「新規」は同じ状態から始める。下地 1 枚だけ。
    m_layers.push_back(MakeBaseLayer());
}

void MaterialStack::SetTerrainScale(float sizeMeters, float heightMeters) {
    if (m_sizeMeters == sizeMeters && m_heightMeters == heightMeters) {
        return;
    }
    m_sizeMeters = sizeMeters;
    m_heightMeters = heightMeters;
    // 法線が実寸に依るので、実寸が動いたら合成し直す。
    MarkDirty();
}

size_t MaterialStack::FirstEnabledIndex() const {
    for (size_t i = 0; i < m_layers.size(); ++i) {
        // 加工（ブラー / 堆積）は下地になれない（下に何も無ければ相手がいない）。
        // 一番下に来たときは、その上の合成レイヤーを下地として扱う。
        if (m_layers[i].enabled && !IsHeightOperationKind(m_layers[i].kind)) {
            return i;
        }
    }
    return static_cast<size_t>(-1);
}

}  // namespace tg::compositor
