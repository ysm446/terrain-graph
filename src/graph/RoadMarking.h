#pragma once

#include "compositor/MaterialLayer.h"
#include "graph/RoadMesh.h"
#include "renderer/MeshData.h"

#include <array>
#include <string>
#include <vector>

// Lane Marking（路面の区画線）。Road Mesh の路面の格子の上に、中央線・外側線・車線境界線の
// 細い帯のメッシュを作る。road-material-editor の Lane Marking（graph/Road.cpp の BuildRoadMarkings）の移植。
// 停止線と矢印はまだ移していない（停止線の仕様はユーザーが後で決める）。
//
// 横位置は路面の UV と同じく左端からの距離（m）。車線の並びは轍のマスク（LayerMaterial.hlsli の
// RoadShapeMask）と同じ: 車線は右端から並べ、走行側で進行方向の車線がどちら側かが決まる。
//
// UI / D3D12 には依存しない。
namespace tg::graph {

// 線の種類。材質・幅・破線は種類ごとに持つ。
enum class RoadMarkingKind : uint32_t {
    Center = 0,  // 中央線（進行方向と対向の境。対向の車線が無ければ出ない）
    Edge = 1,    // 外側線（左右の端から edgeInsetMeters の位置）
    Lane = 2,    // 車線境界線（同じ向きの車線の間）
};
inline constexpr size_t kRoadMarkingKindCount = 3;

struct RoadMarkingLine {
    bool enabled = true;
    bool dashed = false;
    float widthMeters = 0.15f;
    // 線の材質（Material か Layered Material）。無ければ白の定数で塗る。
    compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
};

struct RoadMarkingSettings {
    // 中央線・外側線・車線境界線（RoadMarkingKind の順）。車線境界線は既定で破線。
    std::array<RoadMarkingLine, kRoadMarkingKindCount> lines = {
        RoadMarkingLine{true, false, 0.15f}, RoadMarkingLine{true, false, 0.15f}, RoadMarkingLine{true, true, 0.15f}};
    // 外側線の中心の、路面の端からの距離（m）。
    float edgeInsetMeters = 0.5f;
    // 破線の線の長さと間隔（m）。
    float dashLengthMeters = 5.0f;
    float dashGapMeters = 5.0f;
    // 路面から法線の向きへ浮かせる量（m）。描画でも深度を手前へずらすので、ごく小さくてよい。
    float liftMeters = 0.005f;
    // 材質の貼り方。横は線の幅いっぱいで 1 周（横長の白線の画像がそのまま収まる）、
    // 長さの向きは uvRepeatMeters ごとに 1 周。uvAlongU なら長さの向きを U にする。
    float uvRepeatMeters = 1.0f;
    bool uvAlongU = false;
};

// 車線の並び（左端からの横位置、m）。
struct RoadLaneLayout {
    float laneWidthMeters = 0.0f;
    bool hasCenter = false;
    float centerMeters = 0.0f;
    std::vector<float> dividerMeters;  // 同じ向きの車線の境
};
RoadLaneLayout ComputeRoadLaneLayout(const RoadMeshSettings& mesh);

// 線の帯を種類ごとに作る。road は Road Mesh の路面（BuildRoadMesh の結果）、stride はその 1 行の頂点数。
// 無効にした種類と、道路に出ない種類（対向の無い道路の中央線など）は空のまま。
// 設定が不正（線が重なる、道路の外に出る等）なら偽で、error に理由を入れる。
bool BuildRoadMarkings(const renderer::MeshData& road, uint32_t stride, const RoadMeshSettings& mesh,
                       const RoadMarkingSettings& settings,
                       std::array<renderer::MeshData, kRoadMarkingKindCount>& out, std::string* error);

}  // namespace tg::graph
