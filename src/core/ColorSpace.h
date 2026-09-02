#pragma once

#include <algorithm>
#include <cmath>

namespace tg {

// sRGB とリニアの相互変換（IEC 61966-2-1）。
//
// **式は `shaders/Common.hlsli` の同名関数と必ず合わせること。**
// CPU 側とシェーダ側で違う近似（2.2 乗など）を使うと、
// UI で選んだ色と描画結果がわずかにずれる。
//
// 描画に使う色はすべてリニアで持ち、sRGB は「人が見る / 選ぶとき」の
// 表現としてだけ使う。詳細は docs/design/design-guide.md の「色空間」を参照。

inline float SrgbToLinear(float c) {
    const float v = std::max(c, 0.0f);
    return (v <= 0.04045f) ? (v / 12.92f) : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

inline float LinearToSrgb(float c) {
    const float v = std::max(c, 0.0f);
    return (v <= 0.0031308f) ? (v * 12.92f) : (1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f);
}

}  // namespace tg
