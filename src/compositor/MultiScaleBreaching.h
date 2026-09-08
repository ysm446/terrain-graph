#pragma once

#include <cstdint>
#include <vector>

namespace tg::compositor {

// Section 5.2: 各段で排水路を再計算し、掘削差分だけを広い幅から狭い幅へならす。
// 高さは m。外周を出口とし、水平な流路を許す。入力サイズ不正なら false。
bool MultiScaleBreach(std::vector<float>& heights, uint32_t resolution, uint32_t radius);

}  // namespace tg::compositor
