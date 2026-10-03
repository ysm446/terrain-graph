// ノードグラフの並べ直し（graph/GraphLayout）。
//
// どの接続も左から右へ向くこと、問題のない配置は動かさないこと、動かした箱が重ならないことを確かめる。

#include "graph/GraphLayout.h"

#include "TestSupport.h"

#include <vector>

namespace {

using namespace tg::graph;
using tg::tests::Check;
using tg::tests::Section;

// どの接続も「to の左端 >= from の右端」か。
bool AllForward(const std::vector<LayoutBox>& boxes, const std::vector<LayoutEdge>& edges) {
    for (const LayoutEdge& edge : edges) {
        if (boxes[edge.to].x < boxes[edge.from].x + boxes[edge.from].width) return false;
    }
    return true;
}

bool AnyOverlap(const std::vector<LayoutBox>& boxes) {
    for (size_t a = 0; a < boxes.size(); ++a) {
        for (size_t b = a + 1; b < boxes.size(); ++b) {
            if (boxes[a].x < boxes[b].x + boxes[b].width && boxes[b].x < boxes[a].x + boxes[a].width &&
                boxes[a].y < boxes[b].y + boxes[b].height && boxes[b].y < boxes[a].y + boxes[a].height) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

void RunGraphLayoutTests() {
    Section("GraphLayout: 左から右へ並べ直す");
    constexpr float kGap = 40.0f;
    constexpr float kMinGap = 20.0f;
    constexpr float kMargin = 10.0f;
    {
        // すでに左から右へ並んでいる鎖は動かさない。
        std::vector<LayoutBox> boxes = {{0, 0, 200, 100}, {300, 0, 200, 100}, {600, 50, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 1}, {1, 2}};
        const std::vector<LayoutBox> before = boxes;
        const size_t moved = ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        bool same = true;
        for (size_t i = 0; i < boxes.size(); ++i) same &= boxes[i].x == before[i].x && boxes[i].y == before[i].y;
        Check(moved == 0 && same, "問題のない配置は動かさない");
    }
    {
        // 上流が下流より右にある。直した後は左から右へ。
        std::vector<LayoutBox> boxes = {{900, 0, 200, 100}, {300, 0, 200, 100}, {600, 0, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 1}, {1, 2}};
        const size_t moved = ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        Check(AllForward(boxes, edges), "左へ戻る接続を直す");
        // 上流 1 個を左へ寄せるほうが、下流 2 個を右へ押すより少ない。
        Check(moved == 1 && boxes[1].x == 300.0f && boxes[2].x == 600.0f, "動く箱が少ないほうを選ぶ");
        Check(boxes[0].x == 300.0f - 200.0f - kGap, "直した接続は間に gap を空ける");
    }
    {
        // 真下に並んだ接続（同じ x）も、線が戻るので直す。
        std::vector<LayoutBox> boxes = {{0, 0, 200, 100}, {0, 200, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 1}};
        ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        Check(AllForward(boxes, edges), "同じ列に縦に並んだ接続も直す");
    }
    {
        // 繋がっていない箱どうしは、同じ列に並んでいてもそのまま。
        std::vector<LayoutBox> boxes = {{0, 0, 200, 100}, {0, 200, 200, 100}, {300, 100, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 2}, {1, 2}};
        Check(ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin) == 0, "繋がっていない箱の並びは変えない");
    }
    {
        // 動かした先に別の箱がある。縦にずらして重ならないようにする。
        std::vector<LayoutBox> boxes = {{900, 0, 200, 100}, {300, 0, 200, 100}, {60, 0, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 1}};
        ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        Check(AllForward(boxes, edges) && !AnyOverlap(boxes), "動かした箱はほかと重ならない");
    }
    {
        // 枝分かれと合流のある配置。全部の接続が左から右へ。
        std::vector<LayoutBox> boxes = {{500, 0, 220, 120}, {100, 0, 220, 120},  {100, 300, 220, 120},
                                        {0, 150, 220, 120}, {800, 600, 220, 120}, {300, 600, 220, 120}};
        const std::vector<LayoutEdge> edges = {{0, 1}, {0, 2}, {1, 3}, {2, 3}, {4, 5}, {5, 3}};
        ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        Check(AllForward(boxes, edges), "枝分かれと合流があっても全部の接続を直す");
        Check(!AnyOverlap(boxes), "枝分かれと合流があっても重ならない");
    }
    {
        // 右向きでも、間が minGap より狭い接続は広げる。
        std::vector<LayoutBox> boxes = {{0, 0, 200, 100}, {205, 0, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 1}};
        ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        Check(boxes[1].x - (boxes[0].x + boxes[0].width) == kGap, "間が狭すぎる接続は gap まで広げる");
    }
    {
        // 循環していても止まる（直しきれないだけ）。
        std::vector<LayoutBox> boxes = {{0, 0, 200, 100}, {300, 0, 200, 100}};
        const std::vector<LayoutEdge> edges = {{0, 1}, {1, 0}};
        ArrangeLeftToRight(boxes, edges, kGap, kMinGap, kMargin);
        Check(true, "接続が循環していても終わる");
    }
}
