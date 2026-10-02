#pragma once

// 密な点列の鎖を、クロソイド曲線の制御点（少ない点）へ置き換える。
//
// GeoJSON などから取り込んだ道路は 10 m おきの点が数百並び、曲線の種類をクロソイドにしても
// 角ごとの接線長が短すぎて折れ線のままになる。ここでは、その点列を「直線の交点（IP）」の
// 並びに置き換え、クロソイド（丸め 1.0）で引いたときに元の折れ線から許容誤差の範囲に収まる
// ようにする。
//
// 進め方（IP を探す閾値は持たず、誤差だけで決める）:
//   1. Douglas–Peucker で元の点列を間引き、残った点を制御点の初期値にする（元の線の上に乗る）。
//   2. 制御点でクロソイドを引き、角ごとに曲線の頂（制御点に一番近い標本）が元の線から
//      ずれている分だけ制御点を外へ押し出す（接線の交点へ近づく）。これを数回繰り返す。
//   3. 元の線と曲線の双方向のずれを測り、許容誤差を超える所の元の点を制御点に加えて 2 へ戻る。
//      反向曲線や複合曲線のように直線を挟まない所は、ここで点が増えて近似される。
//   両端の点は動かさない。

#include "graph/Path.h"

#include <array>
#include <vector>

namespace tg::graph {

struct PathClothoidFitOptions {
    float toleranceMeters = 2.0f;  // 元の折れ線からの最大のずれ（元 → 曲線、曲線 → 元の両方）
    float clothoidRatio = 0.5f;    // 置き換えた鎖のクロソイド比
};

struct PathClothoidFitResult {
    size_t pointsBefore = 0;
    size_t pointsAfter = 0;
    float maxErrorMeters = 0.0f;
    // 置き換えた鎖のエッジと内側の点（選択を引き継ぐため）。
    std::vector<PathElementId> edges;
    std::vector<PathElementId> interiorPoints;
};

// 制御点 1 つ。位置は実寸（m）。source は元の折れ線のどの点から来たか（属性の引き継ぎに使う）。
struct FittedControlPoint {
    float x = 0.0f;
    float y = 0.0f;
    size_t source = 0;
};

// 位置だけの版。polyline は実寸（m）の折れ線（3 点以上）。返り値の両端は polyline の両端。
// outMaxError に最後の双方向の最大のずれ（m）を返す。
std::vector<FittedControlPoint> FitClothoidControlPoints(const std::vector<std::array<float, 2>>& polylineMeters,
                                                         const PathClothoidFitOptions& options,
                                                         float* outMaxError = nullptr);

// 鎖を置き換える。開いた鎖で点が 3 つ以上のときだけ（閉じた輪と 2 点の鎖は偽）。
// 両端の点は残す（他の鎖との接続を保つ）。内側の点と鎖のエッジは作り直し、幅 / フェザー / 強さ /
// 高さのずれは元の最寄りの制御点から引き継ぐ。エッジの設定は先頭のエッジのものを引き継ぎ、
// 曲線をクロソイド・丸め 1.0・経路探索なしにする。sizeMeters は地形の一辺（UV → m の換算）。
bool FitStrandToClothoid(PathSettings& path, const PathStrand& strand, float sizeMeters,
                         const PathClothoidFitOptions& options, PathClothoidFitResult* outResult = nullptr);

}  // namespace tg::graph
