#include "compositor/MaterialStack.h"

#include <utility>

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

MaterialLayer& MaterialStack::Add(const MaterialLayer& layer) {
    m_layers.push_back(layer);
    MarkDirty();
    return m_layers.back();
}

void MaterialStack::Remove(size_t index) {
    if (index >= m_layers.size()) {
        return;
    }
    m_layers.erase(m_layers.begin() + static_cast<ptrdiff_t>(index));
    MarkDirty();
}

void MaterialStack::Move(size_t index, int delta) {
    if (index >= m_layers.size() || delta == 0) {
        return;
    }
    const auto target = static_cast<ptrdiff_t>(index) + delta;
    if (target < 0 || target >= static_cast<ptrdiff_t>(m_layers.size())) {
        return;
    }
    std::swap(m_layers[index], m_layers[static_cast<size_t>(target)]);
    MarkDirty();
}

void MaterialStack::MoveTo(size_t from, size_t to) {
    if (from >= m_layers.size() || to >= m_layers.size() || from == to) {
        return;
    }
    // 入れ替えではなく「抜いて差し込む」。間のレイヤーの順序を保つ。
    MaterialLayer moved = std::move(m_layers[from]);
    m_layers.erase(m_layers.begin() + static_cast<ptrdiff_t>(from));
    m_layers.insert(m_layers.begin() + static_cast<ptrdiff_t>(to), std::move(moved));
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
