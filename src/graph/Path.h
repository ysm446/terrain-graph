#pragma once

#include "compositor/MaskGraph.h"

#include <cstdint>
#include <vector>

// パス（Path ノードの中身）。**地形の上に引いた、向き付きの線。**
//
// 点は地形平面の正規化 UV（0〜1）で持ち、**高さは持たない**。高さは読む側
// （表示、Mask Path、将来の「パスに沿って掘る」ノード）が入力の Heightfield から
// 毎回導く。3D で高さを保存すると、上流の侵食を触った瞬間に点が地面に埋まったり
// 浮いたりする（設計の経緯は docs/design/node-graph.md の「パス」）。
//
// - 幅 / フェザー / 強さは点ごと。エッジ上では両端から補間する。
// - エッジは from → to の向きを持つ（川や氷河の流れの向き。道路では無視してよい）。
// - 分岐は許す（支流の合流、道路の交差）。1 つの点に何本のエッジが付いてもよい。
//
// UI / D3D12 には依存しない。編集操作は純粋な関数で、ビューポート側はこれを呼ぶだけ。
namespace tg::graph {

using PathElementId = int;

struct PathPoint {
    PathElementId id = 0;
    float u = 0.5f;  // 地形平面の正規化座標（0〜1）
    float v = 0.5f;
    float widthMeters = 24.0f;    // パスの全幅（m）
    float featherMeters = 12.0f;  // 幅の外側を 0 へ落とす幅（m）
    float intensity = 1.0f;       // マスクの強さ（0〜1）
    // 地形からの高さのずれ（m）。表示と、将来の高さを読むノードが使う。
    // Mask Path は見ない。
    float heightOffsetMeters = 0.0f;
};

struct PathEdge {
    PathElementId id = 0;
    PathElementId from = 0;
    PathElementId to = 0;
};

struct PathSettings {
    std::vector<PathPoint> points;
    std::vector<PathEdge> edges;
    // 新しく置く点の初期値。
    float defaultWidthMeters = 24.0f;
    float defaultFeatherMeters = 12.0f;
    float defaultIntensity = 1.0f;
    // 点とエッジの ID。パスの中で一意ならよい（グラフの ID 空間とは別）。
    PathElementId nextId = 1;

    const PathPoint* FindPoint(PathElementId id) const;
    PathPoint* FindPoint(PathElementId id);
    const PathEdge* FindEdge(PathElementId id) const;
    // a と b を繋ぐエッジ（向きは問わない）。無ければ nullptr。
    const PathEdge* FindEdgeBetween(PathElementId a, PathElementId b) const;
    // 点に付いているエッジの数。
    size_t EdgeCount(PathElementId pointId) const;
};

// --- 編集操作 -------------------------------------------------------------
// どれも成功したら真を返す。失敗しても中身は変えない。

// 点を置く。connectFrom が有効な点なら、そこから新しい点へエッジを張る。
PathElementId AddPathPoint(PathSettings& path, float u, float v, PathElementId connectFrom);
// 2 点をエッジで繋ぐ（from → to）。既に繋がっていれば何もしない（真を返す）。
bool ConnectPathPoints(PathSettings& path, PathElementId from, PathElementId to);
// エッジの途中（t: 0〜1）に点を挿入し、エッジを 2 本に割る。
// 幅 / フェザー / 強さは両端から補間する。返り値は新しい点（失敗なら 0）。
PathElementId InsertPathPointOnEdge(PathSettings& path, PathElementId edgeId, float t);
// 点を消す。付いているエッジも一緒に消える。
bool DeletePathPoint(PathSettings& path, PathElementId pointId);
// エッジを消す。点は残る。
bool DeletePathEdge(PathSettings& path, PathElementId edgeId);
// source を target へ合体させる。source のエッジは target に付け替わり、
// 位置と幅は target のものが残る（持っていった側が相手に合わせに行く）。
bool MergePathPoints(PathSettings& path, PathElementId source, PathElementId target);
// 点を分離する。エッジの本数ぶんに分け、各エッジの端に新しい点を作って
// そのエッジの向きに沿って少し戻した位置（offsetUv）へ置く。
// 出ていくエッジ（下流）は元の点に残す。新しく作った点を outCreated に返す。
// エッジが 1 本以下なら何もしない。
bool SplitPathPoint(PathSettings& path, PathElementId pointId, float offsetUv,
                    std::vector<PathElementId>* outCreated);
// エッジの片端を点から切り離し、新しい点として offsetUv だけ戻した位置に置く。
// pointId はそのエッジの from か to。返り値は新しい点（失敗なら 0）。
PathElementId DetachPathEdgeEnd(PathSettings& path, PathElementId edgeId, PathElementId pointId,
                                float offsetUv);
// エッジの向きを反転する。
bool ReversePathEdge(PathSettings& path, PathElementId edgeId);
// 点に付いているエッジを全部反転する。
bool ReversePathEdgesAt(PathSettings& path, PathElementId pointId);

// 評価用の線分列。座標は正規化 UV のまま（実寸への換算は評価器が一辺の長さで行う）。
// エッジの無い孤立した点は、長さ 0 の線分（円）として出す。
std::vector<compositor::PathSegment> BuildPathSegments(const PathSettings& path);

}  // namespace tg::graph
