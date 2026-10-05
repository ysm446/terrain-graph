#include "graph/GraphLayout.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace tg::graph {
namespace {

// 連鎖で箱を動かす案。箱の添字 → 新しい x。
using Moves = std::unordered_map<size_t, float>;

struct Context {
    const std::vector<LayoutBox>& boxes;
    const std::vector<std::vector<size_t>>& upstream;    // 箱 → その入力に繋がる箱
    const std::vector<std::vector<size_t>>& downstream;  // 箱 → その出力が繋がる箱
    float gap;     // 直すときに空ける幅
    float minGap;  // これより狭い接続を「直す対象」とみなす幅
};

float XOf(const Context& context, const Moves& moves, size_t index) {
    const auto found = moves.find(index);
    return found != moves.end() ? found->second : context.boxes[index].x;
}

// index の箱を target 以下の x へ寄せる。左に戻る接続ができる上流も、必要なだけ左へ寄せる。
void PlanPull(const Context& context, size_t index, float target, Moves& moves, int depth) {
    if (depth > 4096 || XOf(context, moves, index) <= target) return;
    moves[index] = target;
    for (const size_t source : context.upstream[index]) {
        const float width = context.boxes[source].width;
        if (XOf(context, moves, source) + width + context.minGap > target) {
            PlanPull(context, source, target - width - context.gap, moves, depth + 1);
        }
    }
}

// index の箱を target 以上の x へ押す。左に戻る接続ができる下流も、必要なだけ右へ押す。
void PlanPush(const Context& context, size_t index, float target, Moves& moves, int depth) {
    if (depth > 4096 || XOf(context, moves, index) >= target) return;
    moves[index] = target;
    const float right = target + context.boxes[index].width;
    for (const size_t sink : context.downstream[index]) {
        if (XOf(context, moves, sink) < right + context.minGap) {
            PlanPush(context, sink, right + context.gap, moves, depth + 1);
        }
    }
}

float Travel(const Context& context, const Moves& moves) {
    float total = 0.0f;
    for (const auto& [index, x] : moves) total += std::abs(x - context.boxes[index].x);
    return total;
}

bool Overlaps(const LayoutBox& a, const LayoutBox& b, float margin) {
    return a.x < b.x + b.width + margin && b.x < a.x + a.width + margin &&
           a.y < b.y + b.height + margin && b.y < a.y + a.height + margin;
}

}  // namespace

size_t ResolveOverlaps(std::vector<LayoutBox>& boxes, float margin, const std::vector<bool>& movable) {
    const auto canMove = [&](size_t index) { return movable.empty() || (index < movable.size() && movable[index]); };
    // 動かさない箱を先に置き、動かせる箱は上から（同じ高さなら左から）順に決める。
    std::vector<size_t> order;
    for (size_t i = 0; i < boxes.size(); ++i) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (canMove(a) != canMove(b)) return !canMove(a);
        if (boxes[a].y != boxes[b].y) return boxes[a].y < boxes[b].y;
        return boxes[a].x < boxes[b].x;
    });
    std::vector<size_t> placed;
    size_t moved = 0;
    for (const size_t index : order) {
        LayoutBox& box = boxes[index];
        if (canMove(index)) {
            const float original = box.y;
            // 重なる相手の下端のうち一番下まで下ろす。下ろした先でまた重なれば繰り返す
            // （1 回ごとに必ず下へ進むので、置いた箱の数だけ回れば終わる）。
            for (size_t pass = 0; pass <= placed.size(); ++pass) {
                float target = box.y;
                for (const size_t other : placed) {
                    if (Overlaps(box, boxes[other], margin)) {
                        target = std::max(target, boxes[other].y + boxes[other].height + margin);
                    }
                }
                if (target == box.y) break;
                box.y = target;
            }
            if (box.y != original) ++moved;
        }
        placed.push_back(index);
    }
    return moved;
}

size_t ArrangeLeftToRight(std::vector<LayoutBox>& boxes, const std::vector<LayoutEdge>& edges,
                          float gap, float minGap, float margin) {
    minGap = std::clamp(minGap, 0.0f, gap);
    const std::vector<LayoutBox> original = boxes;
    std::vector<std::vector<size_t>> upstream(boxes.size());
    std::vector<std::vector<size_t>> downstream(boxes.size());
    std::vector<LayoutEdge> valid;
    for (const LayoutEdge& edge : edges) {
        if (edge.from >= boxes.size() || edge.to >= boxes.size() || edge.from == edge.to) continue;
        upstream[edge.to].push_back(edge.from);
        downstream[edge.from].push_back(edge.to);
        valid.push_back(edge);
    }
    const Context context{boxes, upstream, downstream, gap, minGap};

    // --- 横: 左へ戻る接続を、戻りの大きいものから 1 本ずつ直す ----------------------
    // 1 本直すと別の接続が直る / 破れるので、毎回選び直す。接続の数の数倍で必ず終わるはずだが、
    // 循環があると終わらないので上限を置く。
    const size_t limit = valid.size() * 8 + 64;
    for (size_t iteration = 0; iteration < limit; ++iteration) {
        const LayoutEdge* worst = nullptr;
        float worstBack = 0.0f;
        for (const LayoutEdge& edge : valid) {
            // 戻りの量。間が minGap より狭いぶんも戻りに数える。
            const float back = boxes[edge.from].x + boxes[edge.from].width + minGap - boxes[edge.to].x;
            if (back > worstBack) {
                worstBack = back;
                worst = &edge;
            }
        }
        if (worst == nullptr) break;
        const LayoutBox& from = boxes[worst->from];
        const LayoutBox& to = boxes[worst->to];
        Moves pull;
        PlanPull(context, worst->from, to.x - from.width - gap, pull, 0);
        Moves push;
        PlanPush(context, worst->to, from.x + from.width + gap, push, 0);
        // 動く箱が少ないほう。同じなら動く量が小さいほう。
        const bool usePull = pull.size() < push.size() ||
                             (pull.size() == push.size() && Travel(context, pull) <= Travel(context, push));
        for (const auto& [index, x] : (usePull ? pull : push)) boxes[index].x = x;
    }

    // --- 縦: 動かした箱がほかと重なったら、元の高さに一番近い空きへずらす ----------------
    std::vector<size_t> moved;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].x != original[i].x) moved.push_back(i);
    }
    std::sort(moved.begin(), moved.end(), [&](size_t a, size_t b) {
        return boxes[a].x != boxes[b].x ? boxes[a].x < boxes[b].x : boxes[a].y < boxes[b].y;
    });
    const auto isFree = [&](size_t index) {
        for (size_t other = 0; other < boxes.size(); ++other) {
            if (other != index && Overlaps(boxes[index], boxes[other], margin)) return false;
        }
        return true;
    };
    constexpr float kStep = 20.0f;
    constexpr int kMaxSteps = 400;
    for (const size_t index : moved) {
        if (isFree(index)) continue;
        const float home = boxes[index].y;
        bool placed = false;
        for (int step = 1; step <= kMaxSteps && !placed; ++step) {
            for (const float sign : {1.0f, -1.0f}) {
                boxes[index].y = home + sign * kStep * static_cast<float>(step);
                if (isFree(index)) {
                    placed = true;
                    break;
                }
            }
        }
        if (!placed) boxes[index].y = home;
    }

    size_t count = 0;
    for (size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].x != original[i].x || boxes[i].y != original[i].y) ++count;
    }
    return count;
}

}  // namespace tg::graph
