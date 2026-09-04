#pragma once

#include "graph/Path.h"

#include <cstdint>
#include <vector>

// パスの経路探索。地形（CPU 側のハイト）の上で、エッジの両端の間の経路を探し、
// エッジの内部点（PathEdge::waypoints）として書き込む。
//
// 格子（ハイトの解像度そのまま）の上の A*。近傍は 36 方向（3 セル以内の既約な向き）で、
// 8 方向だと 45° 刻みの階段になって斜面を斜めに登る線形が作れない。曲がりにもペナルティを
// 掛ける（無いと 1 歩ごとに左右へ振る細かいジグザグで勾配を守ってしまい、間引くと直登に戻る）。
// コストは**ペナルティ方式**（制約ではない）。制約にすると「経路なし」が起きるが、
// ペナルティなら必ず何かは返る。
//   道路: 長さ × (1 + k × 超過^2)、超過 = max(0, |勾配| − 許容) / 許容
//   流れ: 長さ × (1 + k1 × 上り勾配^2 + k2 × 相対高さ)。上りを嫌い、低い所（谷底）を好む
// 結果の折れ線は Douglas–Peucker で間引いて内部点にする。
//
// UI / D3D12 には依存しない。地形は生の配列で受ける（compositor::CpuHeightfield を
// そのまま受けると D3D12 側のヘッダを引き込むため）。
namespace tg::graph {

// 経路探索が読む地形。行優先の正規化ハイト（0〜1）と実寸。
struct PathRouteTerrain {
    uint32_t resolution = 0;
    const float* heights = nullptr;  // resolution^2。行優先、0〜1
    float sizeMeters = 1024.0f;      // 地形の一辺（m）
    float heightMeters = 200.0f;     // ハイト 0〜1 の全幅（m）

    bool IsValid() const { return resolution >= 2 && heights != nullptr; }
};

// 問い合わせ 1 つ（エッジ 1 本ぶん）。座標は正規化 UV。
struct PathRouteQuery {
    float fromU = 0.0f;
    float fromV = 0.0f;
    float toU = 0.0f;
    float toV = 0.0f;
    PathRoute mode = PathRoute::Road;
    float maxGradePercent = 10.0f;  // 道路のときだけ使う
    // 結果の折れ線を間引く許容差（UV）。0 ならセル 1.5 個ぶん。
    float simplifyToleranceUv = 0.0f;
};

// 経路を探す。成功したら内部点（両端を除く）を outWaypoints に入れて真を返す。
// 両端が同じセルなら内部点は空（真）。
bool FindPathRoute(const PathRouteTerrain& terrain, const PathRouteQuery& query,
                   std::vector<PathRouteWaypoint>& outWaypoints);

// エッジ 1 本の経路を計算して書き込む（内部点と、計算したときの両端の位置）。
// route が None のエッジ、端点の無いエッジは偽。
bool RoutePathEdge(PathSettings& path, PathElementId edgeId, const PathRouteTerrain& terrain);

// 経路探索が有効なエッジのうち、経路が古いものを計算し直す。force なら古くなくても全部。
// only を渡すと、その ID のエッジだけを対象にする。計算したエッジの数を返す。
// route が None のエッジに内部点が残っていれば捨てる。
size_t RoutePathEdges(PathSettings& path, const PathRouteTerrain& terrain, bool force,
                      const std::vector<PathElementId>* only = nullptr);

}  // namespace tg::graph
