#include "TestSupport.h"
#include "compositor/MultiScaleBreaching.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace {
// アルゴリズムの木を使わず、境界から非減少の高さだけを辿って排水可能域を検証する。
bool Drains(const std::vector<float>& h, int n) {
    std::vector<bool> visited(h.size());
    std::queue<int> queue;
    const auto add = [&](int i) { if (!visited[i]) { visited[i] = true; queue.push(i); } };
    for (int i = 0; i < n; ++i) { add(i); add((n-1)*n+i); add(i*n); add(i*n+n-1); }
    while (!queue.empty()) {
        const int i = queue.front(); queue.pop();
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const int x = i%n+dx, y = i/n+dy;
            if (x >= 0 && x < n && y >= 0 && y < n && h[y*n+x] >= h[i]) add(y*n+x);
        }
    }
    return std::all_of(visited.begin(), visited.end(), [](bool v) { return v; });
}
}

void RunMultiScaleBreachingTests() {
    using tg::tests::Check;
    using tg::compositor::MultiScaleBreach;
    tg::tests::Section("Multi-scale breaching");
    constexpr int N = 65;
    std::vector<float> original(N*N, 10.0f);
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
        const float dx = float(x-N/2), dy = float(y-N/2);
        original[y*N+x] = 2.0f + std::sqrt(dx*dx+dy*dy)*0.2f;
    }
    Check(!Drains(original, N), "Fixture contains an enclosed basin");
    auto h = original;
    Check(MultiScaleBreach(h, N, 16), "Multiscale correction succeeds");
    Check(Drains(h, N), "Every cell has a non-increasing route to the boundary");
    bool lowerOnly = true;
    for (size_t i=0; i<h.size(); ++i) lowerOnly &= std::isfinite(h[i]) && h[i] <= original[i];
    Check(lowerOnly, "Breaching only removes material and remains finite");
    const auto corrected = h;
    MultiScaleBreach(h, N, 16);
    Check(h == corrected, "An already drained result is unchanged");
    std::fill(h.begin(), h.end(), 4.0f);
    MultiScaleBreach(h, N, 16);
    Check(std::all_of(h.begin(), h.end(), [](float v) { return v == 4.0f; }), "Flat terrain remains exactly flat");
    for (int y=0; y<N; ++y) for (int x=0; x<N; ++x) h[y*N+x] = float(x+y);
    const auto slope = h;
    MultiScaleBreach(h, N, 16);
    Check(h == slope, "Drained planar slope remains unchanged");
    uint32_t seed = 42;
    for (auto& v : h) { seed = seed*1664525u+1013904223u; v = float(seed%10000u)*0.001f; }
    MultiScaleBreach(h, N, 7);
    Check(Drains(h, N), "Multiple random basins drain with an odd starting radius");
    h[0] = std::numeric_limits<float>::quiet_NaN();
    Check(!MultiScaleBreach(h, N, 16), "Non-finite input is rejected");
    Check(!MultiScaleBreach(h, N-1, 16), "Mismatched dimensions are rejected");
}
