#pragma once

#include "compositor/MaterialLayer.h"
#include "graph/RoadPath.h"
#include "renderer/MeshData.h"

#include <string>

// Road Mesh（道路の路面のメッシュ）。Road Path の中心線（縦断を反映、1 m ごと）から、
// 幅方向を約 1 m ごとに割った格子を作る。バンク角で断面を傾ける。
//
// road-material-editor の Road（graph/Road.cpp の BuildRoad）の移植。中心線は Road Path が
// 地形から作るので（graph/RoadPath.h）、ここは断面を張るだけ。
//
// UV は実寸（m）: x = 左端からの横位置（0〜幅）、y = 始点からの道のり。材質は
// この座標で貼る（シェーダが繰り返し長で割る）。
//
// UI / D3D12 には依存しない。
namespace tg::graph {

struct RoadMeshSettings {
    float widthMeters = 7.0f;
    // 車線数（進行方向 / 対向）。車線の幅は幅を車線数で割る。轍のマスクが使う。
    int lanesForward = 1;
    int lanesBackward = 1;
    // 左側通行か（日本は左）。進行方向の車線が道路のどちら側に並ぶかが決まる（轍のマスク）。
    bool leftHandTraffic = true;
    // 路面を中心線からどれだけ持ち上げるか（m）。地形の均し（切土・盛土）が入るまでの
    // 間、地形と重なって路面が欠けるのを抑える。
    float surfaceOffsetMeters = 0.05f;
    // 路面の材質（Material か Layered Material）。無ければ灰色の定数で塗る。
    compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
    // 通常の Material のとき、模様が 1 周する長さ（m）。Layered Material は層ごとの値を使う。
    float uvRepeatMeters = 4.0f;
};

// 幅の上下限（m）。
inline constexpr float kRoadMinWidthMeters = 0.5f;
inline constexpr float kRoadMaxWidthMeters = 60.0f;

// 路面のメッシュを作る。centerline は縦断を反映した中心線（BuildRoadCenterline の結果）。
// 失敗（点が足りない、角が急すぎて断面が裏返る等）なら偽で、error に理由を入れる。
bool BuildRoadMesh(const RoadPathSettings& road, const RoadProfileCurve& centerline,
                   const RoadMeshSettings& settings, renderer::MeshData& out, std::string* error);

}  // namespace tg::graph
