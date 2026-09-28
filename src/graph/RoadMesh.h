#pragma once

#include "compositor/MaterialLayer.h"
#include "graph/RoadPath.h"
#include "renderer/MeshData.h"

#include <string>
#include <vector>

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
// 路面のメッシュの 1 行の頂点数（列 0 が左端、最後の列が右端）。
uint32_t RoadMeshStride(const RoadMeshSettings& settings);

// 路肩（Shoulder ノード）。路面（か内側の路肩）の端から外へ張り出す帯。
// road-material-editor の Shoulder（graph/Road.cpp の BuildShoulder）の移植。
enum class RoadShoulderSide : uint32_t {
    Both = 0,   // 左右両方（同じ設定）
    Left = 1,   // 進行方向に向かって左
    Right = 2,  // 進行方向に向かって右
};
// 路肩の形の決め方。
enum class RoadShoulderShape : uint32_t {
    Slope = 0,    // 幅・横断勾配・段差で決める
    Section = 1,  // 断面の点で決める（縁石・側溝など）
};
// 断面の点。内側の端からの外への距離と、内側の端からの高さ（m）。
struct RoadSectionPoint {
    float acrossMeters = 0.0f;
    float heightMeters = 0.0f;
};
// 路肩の区間の切り替え（SurfaceLayout の区間の移植 ②）。道のり atMeters から先を別の材質・境界にする。
// 移行距離は切替位置を中心にした幅で、その中を smoothstep でなめらかにつなぐ。
struct RoadShoulderSwitch {
    float atMeters = 10.0f;          // 切替位置（始点からの道のり、m）
    float transitionMeters = 2.0f;   // 移行距離（m）。0 なら切り替えの位置でそのまま替わる
    compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
    float uvRepeatMeters = 2.0f;
    std::string boundaryPath;        // 内側の境界（空なら無い）
    std::string boundaryUid;
};
struct RoadShoulderSettings {
    RoadShoulderSide side = RoadShoulderSide::Both;
    RoadShoulderShape shape = RoadShoulderShape::Slope;
    // 断面の点（shape が Section のとき）。内側の端 (0, 0) は含めず、外へ向かう順に並べる。
    // 外への距離は減らさない（同じなら縦の面。縁石の立ち上がりなど）。
    std::vector<RoadSectionPoint> section;
    // 以下の幅・横断勾配・段差は shape が Slope のとき。
    float widthMeters = 1.5f;
    // 横断勾配（%）。正なら外側へ向かって下がる。
    float crossSlopePercent = 4.0f;
    // 内側の端の段差（m）。0 より大きいと、端のすぐ外に stepWidthMeters の面取りの列を挟み、そこで段差ぶん下げる。
    float stepHeightMeters = 0.0f;
    float stepWidthMeters = 0.05f;
    // 材質（Material か Layered Material）。無ければ路肩の灰色で塗る。
    compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
    // 通常の Material のとき、模様が 1 周する長さ（m）。
    float uvRepeatMeters = 2.0f;
    // 内側の境界（境界マテリアル、.tgboundary）。ルートからの相対パスと固定 ID。空なら無い。
    // 内側の端から境界の幅の中で、マスクで内側の帯の材質へ切り替え、ハイトの凹凸を足す。
    std::string boundaryPath;
    std::string boundaryUid;
    // 区間の切り替え（道のりの順でなくてもよい。使うときに並べる）。上の材質・境界は最初の区間。
    std::vector<RoadShoulderSwitch> switches;
};
inline constexpr float kShoulderMinWidthMeters = 0.1f;
// 切り替えの数と、1 本の路肩で使える材質・境界マテリアルの種類の上限（シェーダへ渡す枠の数）。
inline constexpr size_t kShoulderMaxSwitches = 7;
inline constexpr size_t kShoulderMaxSpanMaterials = 4;
inline constexpr size_t kShoulderMaxSpanBoundaries = 4;
inline constexpr float kShoulderMaxTransitionMeters = 50.0f;

// 路肩の区間（切り替えを道のりの順に並べ、最初の区間を先頭に足したもの）。
struct RoadShoulderSpan {
    float startMeters = 0.0f;
    // この区間の始まりでの移行距離。前後の区間の長さを超えない（隣の移行と重ならない）。
    float transitionMeters = 0.0f;
    compositor::MaterialAssetId material = compositor::kNoMaterialAsset;
    float uvRepeatMeters = 2.0f;
    std::string boundaryPath;
    std::string boundaryUid;
};
// 区間の列を作る。切替位置は 0〜道路の長さに収め、同じ位置なら後のものを使う。
std::vector<RoadShoulderSpan> ShoulderSpans(const RoadShoulderSettings& settings, float lengthMeters);
inline constexpr float kShoulderMaxWidthMeters = 20.0f;
// 断面の点の数と高さの上限。
inline constexpr size_t kShoulderMaxSectionPoints = 16;
inline constexpr float kShoulderMaxSectionHeight = 5.0f;

// 断面の点を、内側の端 (0, 0) から外側の端まで返す。勾配の形なら幅・横断勾配・段差から作る。
std::vector<RoadSectionPoint> ShoulderSectionPoints(const RoadShoulderSettings& settings);
// 断面の点を検査する（(0, 0) を含まない、RoadShoulderSettings::section の形）。
bool ValidateShoulderSection(const std::vector<RoadSectionPoint>& section, std::string* error);
// 断面に沿った長さ（m）。路肩の UV の x の範囲（内側の端 0 から外側の端まで）。
float ShoulderSectionLength(const RoadShoulderSettings& settings);
// 断面のひな形（UI の「ひな形」）。
enum class RoadSectionTemplate : uint32_t { Sidewalk = 0, Gutter = 1, SoilShoulder = 2 };
std::vector<RoadSectionPoint> ShoulderSectionTemplate(RoadSectionTemplate kind);

// 路肩の帯を作る。source は内側の帯（路面か内側の路肩）、sourceStride はその 1 行の頂点数、
// edgeColumn は張り出す端の列、innerColumn はその隣の列（外向きを決める）。
// 列 0 は端の頂点をそのまま写す（内側の帯と隙間なくつながる）。UV は実寸: x = 内側の端から断面に
// 沿った長さ、y = 道のり。断面の角（向きが大きく変わる点）は列を 2 つ重ねて稜線を立てる。
// outStride に 1 行の頂点数を返す（最後の列が外側の端）。
bool BuildRoadShoulder(const renderer::MeshData& source, uint32_t sourceStride, uint32_t edgeColumn,
                       uint32_t innerColumn, const RoadShoulderSettings& settings, renderer::MeshData& out,
                       uint32_t& outStride, std::string* error);

}  // namespace tg::graph
