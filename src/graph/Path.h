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

// 鎖（分岐から分岐までのエッジの並び）をどう描くか。エッジに持ち、鎖を選んだときに
// まとめて読み書きする（鎖は導出したものなので、性質の置き場はエッジ）。
//
// **曲線は点を通らない。** 置いた点は制御点で、折れ線（ガイド）の内側を回る。
// Catmull-Rom のように点を貫く形ではない。
enum class PathCurve : uint32_t {
    Line = 0,       // 折れ線そのもの
    Quadratic = 1,  // 2 次ベジェの連結。角ごとに丸め、両端の点だけ通る（C1）
    Cubic = 2,      // 3 次 B スプライン。さらに滑らか（C2）だが折れ線からより離れる
    // クロソイド → 円弧 → クロソイド。角ごとに曲率が 0 から連続的に立ち上がる緩和曲線
    // （道路 / 鉄道の線形）。2 次と同じく両端の点だけ通る。
    Clothoid = 3,
};

struct PathEdge {
    PathElementId id = 0;
    PathElementId from = 0;
    PathElementId to = 0;
    PathCurve curve = PathCurve::Line;
    // どれだけ角を取るか（0〜1）。0 で折れ線のまま、1 で最大（2 次なら線分の中点まで）。
    float rounding = 1.0f;
    // クロソイドのとき、交角のうちクロソイド 2 本が受け持つ割合（0〜1）。
    // 0 で純粋な円弧、1 で円弧なし（緩和曲線だけ）。
    float clothoidRatio = 0.5f;
};

// 鎖。エッジが 2 本だけ付いた点を通り、それ以外の点（端 / 分岐 / 交差）で止まる。
// 「A の途中に B が乗る」という形は無く、支流を繋いだ時点で本流の鎖はそこで割れる。
// 導出するものなので保存しない。
struct PathStrand {
    std::vector<PathElementId> points;  // 並んだ点（両端を含む）。閉じた輪なら末尾 = 先頭
    std::vector<PathElementId> edges;   // points[i] と points[i+1] を結ぶエッジ
    bool closed = false;                // 分岐の無い輪
};

// 曲線を割った標本 1 つ。座標は正規化 UV、値は制御点の値を道のりで補間したもの。
struct PathCurveSample {
    float u = 0.0f;
    float v = 0.0f;
    float widthMeters = 0.0f;
    float featherMeters = 0.0f;
    float intensity = 1.0f;
    float heightOffsetMeters = 0.0f;
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
// 点を消す。鎖の途中の点（エッジがちょうど 2 本）なら、両隣を 1 本のエッジで繋ぎ直して
// から消すので線は切れない（曲線の種類 / 丸め / 向きは残ったエッジのものが続く）。
// 端や分岐（エッジが 1 本以下、または 3 本以上）は、付いているエッジも一緒に消える。
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

// --- 鎖と曲線 -------------------------------------------------------------
// 鎖を導出する。すべてのエッジがちょうど 1 つの鎖に入る。
std::vector<PathStrand> BuildPathStrands(const PathSettings& path);
// エッジが属する鎖。無ければ nullptr。
const PathStrand* FindStrandOfEdge(const std::vector<PathStrand>& strands, PathElementId edgeId);
// 鎖の曲線の種類と丸め（先頭のエッジの値）。mixed はエッジごとに値が違うとき真。
PathCurve StrandCurve(const PathSettings& path, const PathStrand& strand, float* outRounding,
                      bool* outMixed);
// 鎖のクロソイド比（先頭のエッジの値）。
float StrandClothoidRatio(const PathSettings& path, const PathStrand& strand);
// 鎖を折れ線（曲線なら細かく割ったもの）にする。samplesPerSpan は制御点の区間ごとの標本数。
// 直線の鎖は制御点そのものを返す。
std::vector<PathCurveSample> SamplePathStrand(const PathSettings& path, const PathStrand& strand,
                                              int samplesPerSpan);
// 鎖のエッジを全部反転する。
bool ReversePathStrand(PathSettings& path, const PathStrand& strand);

// 評価用の線分列。座標は正規化 UV のまま（実寸への換算は評価器が一辺の長さで行う）。
// 曲線の鎖は細かい直線に割って出す。エッジの無い孤立した点は、長さ 0 の線分（円）として出す。
std::vector<compositor::PathSegment> BuildPathSegments(const PathSettings& path);

}  // namespace tg::graph
