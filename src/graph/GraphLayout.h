#pragma once

#include <cstddef>
#include <vector>

// ノードグラフの並べ直し。UI に依存しない（位置と大きさ、接続だけを受ける）。
namespace tg::graph {

// ノード 1 個の箱（キャンバス座標。x / y は左上）。
struct LayoutBox {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// 接続 1 本（箱の添字。from の出力から to の入力へ）。
struct LayoutEdge {
    size_t from = 0;
    size_t to = 0;
};

// どの接続も左から右へ向くように、箱を動かす。動かした箱の数を返す。
//
// 線は出力（箱の右端）から入力（箱の左端）へ引くので、「左へ戻らない」は
// `to.x >= from.x + from.width` のこと。これを破っている接続だけを直し、直すときは
// 間に gap を空ける。**なるべく元の配置を保つ**: 破っている接続ごとに「上流を左へ寄せる」か
// 「下流を右へ押す」かのうち、連鎖して動く箱が少ないほう（同じなら動く量が小さいほう）を選ぶ。
// 繋がっていない箱どうしが同じ列に並ぶのはそのまま。
//
// 動かした箱がほかの箱と重なったら、元の高さに一番近い空きへ縦にずらす（間に margin を空ける）。
// 接続が循環していると直しきれないので、回数の上限で打ち切る（その分は直らない）。
size_t ArrangeLeftToRight(std::vector<LayoutBox>& boxes, const std::vector<LayoutEdge>& edges,
                          float gap, float margin);

}  // namespace tg::graph
