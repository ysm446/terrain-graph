#include "graph/NodeGraph.h"

#include <algorithm>
#include <array>

// ノードの定義テーブル（種類・保存名・表示名・ピン構成）。
// 評価や GPU に依存しないので、ファイルの読み書き（とそのテスト）からも単独で使える。
namespace tg::graph {
namespace {

// --- 定義テーブル ---------------------------------------------------------
// ノードの種類・保存名・表示名・ピン構成。CreateNode() がここからピンを作る。

// **ノードの名前とピンのラベルは英語で書く。**
// ノードグラフを持つツール（Substance / Houdini / Gaea など）はどれも英語表記で、
// 素材やノードの呼び名もその語彙で流通している。説明文だけ日本語にする。
// 合成レイヤーのピン。**Mask 入力は「どこに乗せるか」**を外から与えるもので、
// 繋がっていなければノード側のマスク設定がそのまま効く。
constexpr std::array<PinDefinition, 3> kLayerNodePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
}};

// Sea（Liquid）のピン。Result のほかに、水と陸を分ける Mask を出す（水の場から焼く）。
// 古いファイルは出力 1 本で保存されているので、Mask は末尾に足す（欠けたぶんは採番し直される）。
constexpr std::array<PinDefinition, 6> kLiquidPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Water"},
    {PinKind::Output, ValueType::Mask, "Depth"},
    {PinKind::Output, ValueType::Mask, "Shore"},
}};

// Surface のピン。**UV Path に Path を繋ぐと、パスに沿った帯の座標で素材を貼る**
// （進行方向が V、幅方向が U。帯の外には乗らない）。繋がなければ地形の UV で並べる。
// 古いファイルは入力 2 本で保存されているので、3 本目は末尾に足す（欠けたぶんは採番し直される）。
constexpr std::array<PinDefinition, 4> kSurfacePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Input, ValueType::Path, "UV Path"},
    {PinKind::Output, ValueType::Material, "Result"},
}};

// マスクを取らない加工（ブラー）のピン。
// ぼかしのピン。**Mask はどこをぼかすか**（明るい所ほどぼける。繋がなければ全体）。
constexpr std::array<PinDefinition, 3> kBlurPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
}};

// マスクのソースのピン。**入力を持たない。**
constexpr std::array<PinDefinition, 1> kMaskSourcePins = {{
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 高さから作るマスクのピン。**どこのハイトから作るか**を Base 入力で指す。
// 繋がなければ、そのマスクを使うレイヤーの直下のハイトを使う。
constexpr std::array<PinDefinition, 2> kMaskFromHeightPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 積雪のピン。ハイトの加工に加えて、**積もった量を Mask として出す**。
// 積もった所へ別のマテリアルを乗せられるようにするため。
// Mask 入力（省略可）は**雪を降らせる場所**（標高で雪線を切るなど）。
// 降った後の滑落はマスクの外へも出る（縁から下へ流れ出るのは自然な形）。
constexpr std::array<PinDefinition, 4> kDepositPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 堆積のピン。積雪と同じ形に加えて、**土砂を供給する場所**（Emission、省略可）を受ける。
// 繋がなければ全面へ一様に供給する。注ぎ口を絞ると「そこから流した液体」になる。
constexpr std::array<PinDefinition, 4> kSedimentPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Emission"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 崩落のピン。発生源のマスクを受け、地形に加えて
// **岩屑の厚み**と**岩片ごとの乱数**を出す。
constexpr std::array<PinDefinition, 6> kCrumblingPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Emission"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Unique"},
    {PinKind::Output, ValueType::Points, "Points"},
}};

// 河川のピン。川の出どころを絞る Seed（省略可）を受け、地形に加えて
// **水面の被覆**・**河原**・**水深**を出す。
constexpr std::array<PinDefinition, 6> kRiverPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Seed"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Water"},
    {PinKind::Output, ValueType::Mask, "Bank"},
    {PinKind::Output, ValueType::Mask, "Depth"},
}};

// 水滴侵食のピン。効かせる範囲を絞る Mask（省略可）を受け、地形に加えて
// **流量**（水の通った量）と**堆積量**を出す。Mask は削り / 積みの差分に
// 掛けるだけで、水滴の落とし方は変えない。
constexpr std::array<PinDefinition, 5> kDropletPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Flow"},
    {PinKind::Output, ValueType::Mask, "Deposit"},
}};

// 散布のピン。散布範囲を絞る Mask（省略可）を受け、地形に加えて
// **分布**と**個体ごとの乱数**を出す。崩落と同じ形。
// Variation（省略可）は配置の点へ書く色むらの値。Model Scatter の株が、
// マテリアルの「色むら」の設定でこの値に応じて色を寄せる（未接続は中立の 0.5）。
constexpr std::array<PinDefinition, 7> kScatterPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Input, ValueType::Mask, "Variation"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Unique"},
    {PinKind::Output, ValueType::Points, "Points"},
}};

// マスクを 1 枚受けて 1 枚返す加工のピン。
constexpr std::array<PinDefinition, 2> kMaskFilterPins = {{
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// マスク 2 枚を合成するピン。**ここでグラフが合流する。**
constexpr std::array<PinDefinition, 3> kMaskBlendPins = {{
    {PinKind::Input, ValueType::Mask, "Foreground"},
    {PinKind::Input, ValueType::Mask, "Background"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// パスのピン。Base は「どの時点の地形に沿うか」（表示とプレビューと経路探索に使う。
// パスの座標は 2D なので評価には効かない）。Avoid は登山道の経路探索が避ける所
// （岩場・崖・水流など。明るいほど避ける）。出力はパスそのもの。
constexpr std::array<PinDefinition, 3> kPathPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Avoid"},
    {PinKind::Output, ValueType::Path, "Path"},
}};

// 道路の線形。Base（沿う地形）と Avoid（経路探索で避ける所）は Path と同じ。
// 出力は Path とは別の型（Road Mesh が読む。Mask Path などへは繋がない）。
constexpr std::array<PinDefinition, 3> kRoadPathPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Avoid"},
    {PinKind::Output, ValueType::RoadPath, "Road Path"},
}};

// 道路の路面のメッシュ。Road Path から作り、Mesh を出す。
constexpr std::array<PinDefinition, 2> kRoadMeshPins = {{
    {PinKind::Input, ValueType::RoadPath, "Road Path"},
    {PinKind::Output, ValueType::Mesh, "Mesh"},
}};

// 路肩。道路（路面と内側の路肩）を受け、自分の帯を足して出す。繋いで外側へ重ねられる。
constexpr std::array<PinDefinition, 2> kShoulderPins = {{
    {PinKind::Input, ValueType::Mesh, "Road"},
    {PinKind::Output, ValueType::Mesh, "Mesh"},
}};

// 区画線。道路を受け、路面の上に線の帯を足して出す。
constexpr std::array<PinDefinition, 2> kLaneMarkingPins = {{
    {PinKind::Input, ValueType::Mesh, "Road"},
    {PinKind::Output, ValueType::Mesh, "Mesh"},
}};

// ユニークなメッシュをビューポートへ出す終端。
constexpr std::array<PinDefinition, 1> kMeshOutputPins = {{
    {PinKind::Input, ValueType::Mesh, "Mesh"},
}};

// パスの足跡をマスクにするピン。
constexpr std::array<PinDefinition, 2> kMaskPathPins = {{
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// メッシュ（道路）の足跡をマスクにするピン。Road Mesh か Shoulder の出力を繋ぐ。
constexpr std::array<PinDefinition, 2> kMaskMeshPins = {{
    {PinKind::Input, ValueType::Mesh, "Mesh"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

// 道路の均しのピン。Base は地形、Mesh は Road Mesh か Shoulder の出力。
// Mask は 路面の下（平らな幅まで）/ 切土の法面 / 盛土の法面。
constexpr std::array<PinDefinition, 6> kRoadGradingPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mesh, "Mesh"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Road"},
    {PinKind::Output, ValueType::Mask, "Cut"},
    {PinKind::Output, ValueType::Mask, "Fill"},
}};

constexpr std::array<PinDefinition, 1> kOutputNodePins = {{
    {PinKind::Input, ValueType::Material, "Material"},
}};

// 風の場。Base の地形に一様な風をぶつけて流れを作り、地表の風速と粉雪の発生量を Mask で出す。
// Wind 出力は 3D の速度場（今は繋ぐ先が無い。Volume Sim が境界条件に読む予定）。
constexpr std::array<PinDefinition, 4> kWindFieldPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Output, ValueType::Mask, "Speed"},
    {PinKind::Output, ValueType::Mask, "Spindrift"},
    {PinKind::Output, ValueType::Wind, "Wind"},
}};

// 雲グラフの Terrain。地形グラフの結果を Material として出す（入力なし）。
constexpr std::array<PinDefinition, 1> kTerrainNodePins = {{
    {PinKind::Output, ValueType::Material, "Result"},
}};

// ソースノードのピン。**入力を持たない。**
constexpr std::array<PinDefinition, 1> kSourceNodePins = {{
    {PinKind::Output, ValueType::Material, "Result"},
}};

constexpr std::array<PinDefinition, 4> kFlowlinePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Source"},
    {PinKind::Input, ValueType::Mask, "Outflow"},
    {PinKind::Output, ValueType::Mask, "Mask"},
}};

constexpr std::array<PinDefinition, 6> kLakePins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Water Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Lake"},
    {PinKind::Output, ValueType::Mask, "Depth"},
    {PinKind::Output, ValueType::Mask, "Water Level"},
}};

constexpr std::array<PinDefinition, 6> kSnowCoverPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Cover"},
    {PinKind::Output, ValueType::Mask, "Depth"},
    {PinKind::Output, ValueType::Mask, "Flows"},
}};

constexpr std::array<PinDefinition, 7> kFluvialErosionPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Mask, "Mask"},
    {PinKind::Input, ValueType::Mask, "Hardness"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "Wear"},
    {PinKind::Output, ValueType::Mask, "Deposit"},
    {PinKind::Output, ValueType::Mask, "Age"},
}};

constexpr std::array<PinDefinition, 1> kCloudPins = {{
    {PinKind::Output, ValueType::Volume, "Volume"},
}};
constexpr std::array<PinDefinition, 1> kCloudOutputPins = {{
    {PinKind::Input, ValueType::Volume, "Volume"},
}};

constexpr std::array<PinDefinition, 3> kCloudWeatherPins = {{
    {PinKind::Input, ValueType::Mask, "Coverage"},
    {PinKind::Input, ValueType::Mask, "Type"},
    {PinKind::Output, ValueType::Volume, "Volume"},
}};
constexpr std::array<PinDefinition, 2> kCloudLayerPins = {{
    {PinKind::Input, ValueType::Mask, "Distribution"},
    {PinKind::Output, ValueType::Volume, "Volume"},
}};

constexpr std::array<PinDefinition, 4> kMeanderingRiversPins = {{
    {PinKind::Input, ValueType::Material, "Base"},
    {PinKind::Input, ValueType::Path, "Path"},
    {PinKind::Output, ValueType::Material, "Result"},
    {PinKind::Output, ValueType::Mask, "River"},
}};

constexpr std::array<PinDefinition, 1> kCloudShapePins = {{{PinKind::Output, ValueType::CloudShape, "Shape"}}};
constexpr std::array<PinDefinition, 2> kCloudMergePins = {{
    {PinKind::Input, ValueType::CloudShape, "Shape 1"},
    {PinKind::Output, ValueType::CloudShape, "Shape"}}};
constexpr std::array<PinDefinition, 2> kCloudNoisePins = {{
    {PinKind::Input, ValueType::CloudShape, "Shape"}, {PinKind::Output, ValueType::Volume, "Volume"}}};

constexpr std::array<PinDefinition, 2> kCloudTransformPins = {{
    {PinKind::Input, ValueType::CloudShape, "Shape"}, {PinKind::Output, ValueType::CloudShape, "Shape"}}};

constexpr std::array<PinDefinition, 2> kCloudAnimationPins = {{{PinKind::Input, ValueType::Volume, "Volume"}, {PinKind::Output, ValueType::Volume, "Volume"}}};

constexpr std::array<PinDefinition, 2> kModelScatterPins = {{
    {PinKind::Input, ValueType::Points, "Points"},
    {PinKind::Output, ValueType::Instances, "Instances"},
}};
constexpr std::array<PinDefinition, 2> kModelMergePins = {{
    {PinKind::Input, ValueType::Instances, "Instances 1"},
    {PinKind::Output, ValueType::Instances, "Instances"},
}};
constexpr std::array<PinDefinition, 1> kModelOutputPins = {{
    {PinKind::Input, ValueType::Instances, "Instances"},
}};
// 雪煙。Source のマスク（Wind Field の Spindrift を想定）から風下へ帯を伸ばして描く。出力は持たない。
constexpr std::array<PinDefinition, 1> kSnowPlumePins = {{
    {PinKind::Input, ValueType::Mask, "Source"},
}};
constexpr std::array<NodeDefinition, 57> kNodeDefinitions = {{
    {NodeKind::Mountain, "mountain", "Mountain", kSourceNodePins,
     "画像なしで山並みと尾根を生成する"},
    {NodeKind::Heightmap, "heightmap", "Heightmap", kSourceNodePins,
     "画像を地形として読み込む"},
    {NodeKind::Surface, "surface", "Surface", kSurfacePins,
     "素材を高さで張り合わせる"},
    {NodeKind::Shape, "shape", "Shape", kLayerNodePins,
     "高さへ起伏を加算する"},
    // 表示名は Sea（ユーザー指定 2026-10-04）。保存名（"liquid"）と識別子（Liquid）は変えない
    // （既存のファイルを開けるように。元は Mixer の Liquid に倣った汎用の名前だった）。
    {NodeKind::Liquid, "liquid", "Sea", kLiquidPins,
     "水位より低い所に海（水）を張る"},
    {NodeKind::Blur, "heightmapBlur", "Heightmap Blur", kBlurPins,
     "ハイトをぼかしてならす"},
    {NodeKind::Sediment, "sediment", "Sediment", kSedimentPins,
     "土砂を重力で再分配して谷に積もらせる"},
    {NodeKind::Crumbling, "crumbling", "Crumbling", kCrumblingPins,
     "崩れた岩屑を斜面下へ流して積む"},
    {NodeKind::SnowCover, "snowCover", "Snow Cover", kSnowCoverPins,
     "積雪と薄雪を生成し、被覆・雪深・流動量を出す"},
    {NodeKind::Snow, "snow", "Snow", kDepositPins,
     "雪を降らせ、急な雪面から落として積もらせる"},
    {NodeKind::River, "river", "River", kRiverPins,
     "川筋から河床を掘り、下流へ下がる水面を張る"},
    {NodeKind::Lake, "lake", "Lake", kLakePins,
     "窪みに水を溜め、湖の範囲・水深・水位を出す"},
    {NodeKind::MeanderingRivers, "meanderingRivers", "Meandering Rivers", kMeanderingRiversPins,
     "Path を蛇行させ、河床を掘る"},
    {NodeKind::Droplet, "droplet", "Droplet Erosion", kDropletPins,
     "水滴を流して谷を刻み、土砂を運んで積む"},
    {NodeKind::FluvialErosion, "fluvialErosion", "Fluvial Erosion", kFluvialErosionPins,
     "流れに沿って谷を刻み、細部を戻しながら侵食する"},
    {NodeKind::FlattenBorders, "flattenBorders", "Flatten Borders", kBlurPins,
     "地形の外周を指定した標高へならす"},
    {NodeKind::HeightLevels, "heightLevels", "Height Levels", kBlurPins,
     "高さの範囲を引き伸ばす（侵食で縮んだ範囲を元の全幅へ戻す）"},
    {NodeKind::MultiScaleErosion, "multiScaleErosion", "Multi-Scale Erosion", kBlurPins,
     "大きな谷から細かな溝まで段階的に侵食する"},
    {NodeKind::Scatter, "scatter", "Scatter", kScatterPins,
     "単純な形をばら撒き、分布のマスクを出す"},
    {NodeKind::MaskImage, "maskImage", "Mask Image", kMaskSourcePins,
     "画像をマスクにする（白い所だけ乗る）"},
    {NodeKind::MaskNoise, "maskNoise", "Mask Noise", kMaskSourcePins,
     "ノイズをマスクにする（下地に依らない）"},
    {NodeKind::MaskFluvial, "maskFluvial", "Mask Fluvial", kMaskFromHeightPins,
     "下地の川筋をマスクにする"},
    {NodeKind::MaskFlowline, "maskFlowline", "Mask Flowline", kFlowlinePins,
     "地形に沿う流跡をマスクにする"},
    {NodeKind::MaskHeight, "maskHeight", "Mask Height", kMaskFromHeightPins,
     "下地の標高帯（m）をマスクにする"},
    {NodeKind::MaskSlope, "maskSlope", "Mask Slope", kMaskFromHeightPins,
     "下地の傾斜（角度）をマスクにする"},
    {NodeKind::MaskCurvature, "maskCurvature", "Mask Curvature", kMaskFromHeightPins,
     "下地の凹凸（尾根 / 谷）をマスクにする"},
    {NodeKind::MaskLevels, "maskLevels", "Mask Levels", kMaskFilterPins,
     "マスクの黒点 / 白点 / ガンマを調整する"},
    {NodeKind::MaskBlur, "maskBlur", "Mask Blur", kMaskFilterPins,
     "マスクをぼかして境界をなだらかにする"},
    {NodeKind::MaskBlend, "maskBlend", "Mask Blend", kMaskBlendPins,
     "マスク 2 枚を合成する"},
    {NodeKind::Path, "path", "Path", kPathPins,
     "地形の上に線を引く（道路 / 川 / 氷河のガイド）"},
    {NodeKind::MaskPath, "maskPath", "Mask Path", kMaskPathPins,
     "パスの足跡をマスクにする"},
    {NodeKind::MaskArea, "maskArea", "Mask Area", kMaskPathPins,
     "パスの閉じた鎖の内側をマスクにする（エリア選択）"},
    {NodeKind::RoadPath, "roadPath", "Road Path", kRoadPathPins,
     "道路の線形（縦断曲線・バンク角）を引く"},
    {NodeKind::RoadMesh, "roadMesh", "Road Mesh", kRoadMeshPins,
     "Road Path から路面のメッシュを作る"},
    {NodeKind::MeshOutput, "meshOutput", "Mesh Output", kMeshOutputPins,
     "道路などのメッシュをビューポートへ出す"},
    {NodeKind::Shoulder, "shoulder", "Shoulder", kShoulderPins,
     "路面の端から外へ路肩の帯を張り出す"},
    {NodeKind::LaneMarking, "laneMarking", "Lane Marking", kLaneMarkingPins,
     "路面に中央線・外側線・車線境界線を引く"},
    {NodeKind::RoadGrading, "roadGrading", "Road Grading", kRoadGradingPins,
     "道路メッシュに合わせて地形を切土・盛土で均す"},
    {NodeKind::MaskMesh, "maskMesh", "Mask Mesh", kMaskMeshPins,
     "道路などのメッシュの足跡をマスクにする（植生の除外など）"},
    {NodeKind::Cloud, "cloud", "Cloud (Legacy)", kCloudPins,
     "旧形式の雲。保存済みのシーンを表示するために残している（追加メニューには出さない）"},
    {NodeKind::CloudLayer, "cloudLayer", "Cloud Layer (Legacy)", kCloudLayerPins,
     "旧形式の雲層。保存済みのシーンを表示するために残している（追加メニューには出さない）"},
    {NodeKind::CloudWeatherLayer, "cloudWeatherLayer", "Cloud Weather Layer", kCloudWeatherPins,
     "雲量と雲種のマップで広域の雲層を作る"},
    {NodeKind::CloudMerge, "cloudMerge", "Cloud Merge (Experimental)", kCloudMergePins,
     "基本形状を統合する"},
    {NodeKind::CloudAnimation, "cloudAnimation", "Cloud Animation", kCloudAnimationPins,
     "指定範囲で雲を繰り返し移動する"},
    {NodeKind::CloudNoise, "cloudNoise", "Cloud Noise (Experimental)", kCloudNoisePins,
     "輪郭と密度を作る"},
    {NodeKind::CloudShapeGenerate, "cloudShapeGenerate", "Cloud Shape Generate (Experimental)", kCloudShapePins,
     "単独の積雲を生成する"},
    {NodeKind::CloudMapGenerate, "cloudMapGenerate", "Cloud Map Generate (Experimental)", kCloudShapePins,
     "ポイントの分布から雲形状を生成する"},
    {NodeKind::CloudTransform, "cloudTransform", "Cloud Transform (Experimental)", kCloudTransformPins,
     "雲形状を移動する"},
    {NodeKind::CloudOutput, "cloudOutput", "Cloud Output", kCloudOutputPins,
     "Volume を繋いで雲を表示する"},
    {NodeKind::ModelScatter, "modelScatter", "Model Scatter", kModelScatterPins,
     "Points にモデルをランダム配置する"},
    {NodeKind::ModelOutput, "modelOutput", "Model Output", kModelOutputPins,
     "モデル配置をビューポートへ出す"},
    {NodeKind::ModelMerge, "modelMerge", "Model Merge", kModelMergePins,
     "複数のモデル配置をまとめる"},
    {NodeKind::Output, "output", "Output", kOutputNodePins,
     "ここに繋いだ結果をプレビューする"},
    {NodeKind::Terrain, "terrain", "Terrain", kTerrainNodePins,
     "地形グラフの結果を取り出す（マスクの Base に繋ぐ）"},
    {NodeKind::WindField, "windField", "Wind Field", kWindFieldPins,
     "地形全体の風の場。地表の風速と粉雪の発生量をマスクにする"},
    {NodeKind::SnowPlume, "snowPlume", "Snow Plume", kSnowPlumePins,
     "稜線から風下へ雪煙をなびかせる（Spindrift を Source に繋ぐ）"},
    // 追加メニューには出さない。読み込みで定義が見つからなかったノードの受け皿。
    {NodeKind::Missing, "missing", "Missing", {},
     "読み込んだファイルにあるが、この版では扱えない種類。ピンを持たず、エラー表示だけする"},
}};
// 宣言の要素数が実際より多いと、名前の空の定義（種類は Surface）が混ざり、
// kind が空のノードを Surface として読んでしまう。
static_assert(std::ranges::all_of(kNodeDefinitions, [](const NodeDefinition& definition) {
                  return definition.name[0] != '\0';
              }),
              "kNodeDefinitions の要素数と定義の数を揃える");

}  // namespace

std::span<const NodeDefinition> NodeDefinitions() {
    return kNodeDefinitions;
}

const NodeDefinition* FindNodeDefinition(NodeKind kind) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.kind == kind) {
            return &definition;
        }
    }
    return nullptr;
}

const NodeDefinition* FindNodeDefinitionByName(std::string_view name) {
    for (const NodeDefinition& definition : kNodeDefinitions) {
        if (definition.name == name) {
            return &definition;
        }
    }
    return nullptr;
}

std::optional<size_t> FindPinDefinitionIndex(const NodeDefinition& definition, PinKind kind,
                                             std::string_view name) {
    const auto normalize = [](std::string_view text) {
        std::string result;
        for (const char c : text) {
            if (c == ' ' || c == '_' || c == '-') {
                continue;
            }
            result.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
        }
        return result;
    };
    const std::string wanted = normalize(name);
    if (wanted.empty()) {
        return std::nullopt;
    }
    size_t index = 0;
    for (const PinDefinition& pin : definition.pins) {
        if (pin.kind != kind) {
            continue;
        }
        if (normalize(pin.label) == wanted) {
            return index;
        }
        ++index;
    }
    return std::nullopt;
}

}  // namespace tg::graph
