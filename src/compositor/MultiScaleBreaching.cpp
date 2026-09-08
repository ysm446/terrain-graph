#include "compositor/MultiScaleBreaching.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace tg::compositor {
namespace {

// 外周から priority flood で作った木を逆順に辿り、窪地から出口までを下げる。
// 埋め立て面は経路の探索専用。結果へ盛り土を加えない。
std::vector<float> Breach(const std::vector<float>& heights, uint32_t n) {
    struct Cell {
        float spill;
        uint32_t index;
        bool operator>(const Cell& other) const {
            return spill != other.spill ? spill > other.spill : index > other.index;
        }
    };
    constexpr uint32_t Unvisited = std::numeric_limits<uint32_t>::max();
    std::vector<uint32_t> parent(heights.size(), Unvisited), order;
    order.reserve(heights.size());
    std::priority_queue<Cell, std::vector<Cell>, std::greater<Cell>> queue;
    const auto seed = [&](uint32_t i) {
        if (parent[i] != Unvisited) return;
        parent[i] = i;
        queue.push({heights[i], i});
    };
    for (uint32_t i = 0; i < n; ++i) {
        seed(i); seed((n - 1) * n + i); seed(i * n); seed(i * n + n - 1);
    }
    while (!queue.empty()) {
        const Cell cell = queue.top();
        queue.pop();
        order.push_back(cell.index);
        const int x = static_cast<int>(cell.index % n), y = static_cast<int>(cell.index / n);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int qx = x + dx, qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= static_cast<int>(n) || qy >= static_cast<int>(n)) continue;
            const uint32_t q = static_cast<uint32_t>(qy) * n + static_cast<uint32_t>(qx);
            if (parent[q] != Unvisited) continue;
            parent[q] = cell.index;
            queue.push({std::max(cell.spill, heights[q]), q});
        }
    }
    auto result = heights;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        result[parent[*it]] = std::min(result[parent[*it]], result[*it]);
    return result;
}

// 有限支持の分離箱カーネル。端では領域内の重みだけで正規化する。
void Smooth(std::vector<float>& values, uint32_t n, uint32_t radius) {
    std::vector<float> scratch(values.size());
    const int extent = static_cast<int>(radius) - 1;
    for (int axis = 0; axis < 2; ++axis) {
        for (uint32_t row = 0; row < n; ++row) {
            const auto index = [&](int p) -> size_t {
                return axis == 0 ? size_t(row) * n + p : size_t(p) * n + row;
            };
            double sum = 0;
            int count = 0;
            for (int p = 0; p <= extent && p < static_cast<int>(n); ++p) {
                sum += values[index(p)]; ++count;
            }
            for (int p = 0; p < static_cast<int>(n); ++p) {
                scratch[index(p)] = static_cast<float>(std::max(0.0, sum / count));
                if (p - extent >= 0) { sum -= values[index(p - extent)]; --count; }
                if (p + extent + 1 < static_cast<int>(n)) {
                    sum += values[index(p + extent + 1)]; ++count;
                }
            }
        }
        values.swap(scratch);
    }
}

}  // namespace

bool MultiScaleBreach(std::vector<float>& heights, uint32_t resolution, uint32_t radius) {
    if (resolution < 2 || heights.size() != size_t(resolution) * resolution ||
        !std::all_of(heights.begin(), heights.end(), [](float h) { return std::isfinite(h); }))
        return false;
    for (uint32_t r = std::clamp(radius, 1u, resolution);; r = std::max(1u, r / 2)) {
        auto breached = Breach(heights, resolution);
        if (r == 1) { heights.swap(breached); break; }
        for (size_t i = 0; i < heights.size(); ++i) breached[i] = heights[i] - breached[i];
        Smooth(breached, resolution, r);
        for (size_t i = 0; i < heights.size(); ++i) heights[i] -= breached[i];
    }
    return true;
}

}  // namespace tg::compositor
