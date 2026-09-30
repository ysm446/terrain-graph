#include "graph/NodeGraph.h"

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
    {NodeKind::Heightmap, "heightmap", "Heightmap", kSourceNodePins},
    {NodeKind::Surface, "surface", "Surface", kSurfacePins},
    {NodeKind::Shape, "shape", "Shape", kLayerNodePins},
    {NodeKind::Liquid, "liquid", "Liquid", kLayerNodePins},
    {NodeKind::Blur, "heightmapBlur", "Heightmap Blur", kBlurPins},
    {NodeKind::Sediment, "sediment", "Sediment", kSedimentPins},
    {NodeKind::Crumbling, "crumbling", "Crumbling", kCrumblingPins},
    {NodeKind::SnowCover, "snowCover", "Snow Cover", kSnowCoverPins},
    {NodeKind::Snow, "snow", "Snow", kDepositPins},
    {NodeKind::River, "river", "River", kRiverPins},
    {NodeKind::Lake, "lake", "Lake", kLakePins},
    {NodeKind::MeanderingRivers, "meanderingRivers", "Meandering Rivers", kMeanderingRiversPins},
    {NodeKind::Droplet, "droplet", "Droplet Erosion", kDropletPins},
    {NodeKind::FluvialErosion, "fluvialErosion", "Fluvial Erosion", kFluvialErosionPins},
    {NodeKind::FlattenBorders, "flattenBorders", "Flatten Borders", kBlurPins},
    {NodeKind::HeightLevels, "heightLevels", "Height Levels", kBlurPins},
    {NodeKind::MultiScaleErosion, "multiScaleErosion", "Multi-Scale Erosion", kBlurPins},
    {NodeKind::Scatter, "scatter", "Scatter", kScatterPins},
    {NodeKind::MaskImage, "maskImage", "Mask Image", kMaskSourcePins},
    {NodeKind::MaskNoise, "maskNoise", "Mask Noise", kMaskSourcePins},
    {NodeKind::MaskFluvial, "maskFluvial", "Mask Fluvial", kMaskFromHeightPins},
    {NodeKind::MaskFlowline, "maskFlowline", "Mask Flowline", kFlowlinePins},
    {NodeKind::MaskHeight, "maskHeight", "Mask Height", kMaskFromHeightPins},
    {NodeKind::MaskSlope, "maskSlope", "Mask Slope", kMaskFromHeightPins},
    {NodeKind::MaskCurvature, "maskCurvature", "Mask Curvature", kMaskFromHeightPins},
    {NodeKind::MaskLevels, "maskLevels", "Mask Levels", kMaskFilterPins},
    {NodeKind::MaskBlur, "maskBlur", "Mask Blur", kMaskFilterPins},
    {NodeKind::MaskBlend, "maskBlend", "Mask Blend", kMaskBlendPins},
    {NodeKind::Path, "path", "Path", kPathPins},
    {NodeKind::MaskPath, "maskPath", "Mask Path", kMaskPathPins},
    {NodeKind::MaskArea, "maskArea", "Mask Area", kMaskPathPins},
    {NodeKind::RoadPath, "roadPath", "Road Path", kRoadPathPins},
    {NodeKind::RoadMesh, "roadMesh", "Road Mesh", kRoadMeshPins},
    {NodeKind::MeshOutput, "meshOutput", "Mesh Output", kMeshOutputPins},
    {NodeKind::Shoulder, "shoulder", "Shoulder", kShoulderPins},
    {NodeKind::LaneMarking, "laneMarking", "Lane Marking", kLaneMarkingPins},
    {NodeKind::Cloud, "cloud", "Cloud (Legacy)", kCloudPins},
    {NodeKind::CloudLayer, "cloudLayer", "Cloud Layer (Legacy)", kCloudLayerPins},
    {NodeKind::CloudWeatherLayer, "cloudWeatherLayer", "Cloud Weather Layer", kCloudWeatherPins},
    {NodeKind::CloudMerge, "cloudMerge", "Cloud Merge (Experimental)", kCloudMergePins},
    {NodeKind::CloudAnimation, "cloudAnimation", "Cloud Animation", kCloudAnimationPins},
    {NodeKind::CloudNoise, "cloudNoise", "Cloud Noise (Experimental)", kCloudNoisePins},
    {NodeKind::CloudShapeGenerate, "cloudShapeGenerate", "Cloud Shape Generate (Experimental)", kCloudShapePins},
    {NodeKind::CloudMapGenerate, "cloudMapGenerate", "Cloud Map Generate (Experimental)", kCloudShapePins},
    {NodeKind::CloudTransform, "cloudTransform", "Cloud Transform (Experimental)", kCloudTransformPins},
    {NodeKind::CloudOutput, "cloudOutput", "Cloud Output", kCloudOutputPins},
    {NodeKind::ModelScatter, "modelScatter", "Model Scatter", kModelScatterPins},
    {NodeKind::ModelOutput, "modelOutput", "Model Output", kModelOutputPins},
    {NodeKind::ModelMerge, "modelMerge", "Model Merge", kModelMergePins},
    {NodeKind::Output, "output", "Output", kOutputNodePins},
    {NodeKind::Terrain, "terrain", "Terrain", kTerrainNodePins},
    {NodeKind::WindField, "windField", "Wind Field", kWindFieldPins},
    {NodeKind::SnowPlume, "snowPlume", "Snow Plume", kSnowPlumePins},
    // 追加メニューには出さない。読み込みで定義が見つからなかったノードの受け皿。
    {NodeKind::Missing, "missing", "Missing", {}},
}};

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
