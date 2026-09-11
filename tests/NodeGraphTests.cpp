// ノードグラフから評価用レイヤー列へのコンパイルを確かめる。
// GPU 評価の前段だけを対象にし、入力を外したときに古い結果を残さない規則を固定する。

#include "graph/NodeGraph.h"
#include "renderer/CloudMotion.h"

#include "TestSupport.h"

#include <array>
#include <variant>
#include <cmath>

namespace {

using tg::graph::NodeGraph;
using tg::graph::NodeKind;
using tg::tests::Check;
using tg::tests::Section;

bool IsNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.size() != 1) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

bool StartsWithNeutralPlane(const tg::graph::CompiledGraph& compiled) {
    if (compiled.layers.empty()) {
        return false;
    }
    const tg::compositor::MaterialLayer& layer = compiled.layers.front();
    return layer.enabled && !tg::compositor::IsHeightOperationKind(layer.kind) &&
           layer.heightSource == tg::compositor::ValueSource::Constant &&
           layer.heightBase == tg::compositor::kHeightPivot;
}

}  // namespace

void RunNodeGraphTests() {
    {
        Section("手続き雲の形状構築");
        auto graph = NodeGraph::CreateDefault();
        const auto line=graph.CreateNode(NodeKind::CloudLine);
        const auto spheres=graph.CreateNode(NodeKind::CloudSpheres);
        const auto base=graph.CreateNode(NodeKind::CloudEllipsoid);
        const auto merge=graph.CreateNode(NodeKind::CloudMerge);
        const auto noise=graph.CreateNode(NodeKind::CloudNoise);
        const auto output=graph.CreateNode(NodeKind::CloudOutput);
        const auto link=[&](int from,int to,size_t index=0) {
            return graph.CreateLink(graph.FindNode(from)->outputs[0].id,graph.FindNode(to)->inputs[index].id);
        };
        Check(link(noise,output),"実験用の密度フィールドを既存雲出力へ接続できる");
        Check(!graph.CompileCloud().connected,"形状なしは雲を表示しない");
        Check(!link(line,noise),"ラインを直接密度フィールドへ接続しない");
        Check(link(line,spheres) && link(spheres,merge) && link(base,merge,1) && link(merge,noise),"形状構築チェーンを接続できる");
        const auto compiled=graph.CompileCloud();
        Check(compiled.connected && compiled.primitives.size()==9,"8球と楕円体を同じフィールドにまとめる");
        Check(!compiled.layer && compiled.cloud.thickness>1400,"地形と独立した立体の包囲箱を作る");
        const auto repeated=graph.CompileCloud();
        Check(repeated.primitives[0].centerX==compiled.primitives[0].centerX,"同じシードで同じ配置を再現する");
        auto& sphereSettings=std::get<tg::graph::CloudSpheresSettings>(graph.FindMutableNode(spheres)->settings);
        sphereSettings.count=64;
        Check(graph.CompileCloud().connected && graph.CompileCloud().primitives.size()==65,
            "旧上限を超えた64球と楕円体をマージできる");
        sphereSettings.count=static_cast<int>(tg::graph::MaxCloudPrimitives)-1;
        Check(graph.CompileCloud().connected && graph.CompileCloud().primitives.size()==tg::graph::MaxCloudPrimitives,
            "マージした合計256個を欠落なく保持する");
        sphereSettings.count=static_cast<int>(tg::graph::MaxCloudPrimitives);
        Check(graph.CompileCloud().shapeOverflow && !graph.CompileCloud().connected,"上限を超えた形状を黙って切り捨てず表示を停止する");
        Check(link(spheres,merge,1),"同じ形状を両入力に繋げる");
        Check(graph.CompileCloud().primitives.size()==tg::graph::MaxCloudPrimitives && !graph.CompileCloud().shapeOverflow,"共有された形状は一度だけ取り込む");
        Check(!link(merge,merge),"形状マージの循環を拒否する");
        sphereSettings.count=1; sphereSettings.jitter=0; sphereSettings.radiusVariation=0;
        const auto single=graph.CompileCloud();
        Check(single.primitives.size()==1 && single.primitives[0].centerY==200 && single.primitives[0].radiusX==350,
            "球が1つの場合は始点と始点半径を使う");
        graph.DeleteNode(line);
        Check(!graph.CompileCloud().connected,"ライン削除後に古い形状を残さない");
    }

    {
        Section("手続き雲の包囲範囲と選択ガイド");
        NodeGraph graph;
        const auto shape=graph.CreateNode(NodeKind::CloudEllipsoid);
        const auto noise=graph.CreateNode(NodeKind::CloudNoise);
        const auto output=graph.CreateNode(NodeKind::CloudOutput);
        auto& ellipse=std::get<tg::graph::CloudEllipsoidSettings>(graph.FindMutableNode(shape)->settings);
        ellipse.radiusX=1200; ellipse.radiusY=180; ellipse.radiusZ=700;
        Check(graph.CompileCloudShapes(shape).primitives.size()==1 && !graph.CompileCloud().connected,
            "未接続の形状もガイド用に評価できる");
        graph.CreateLink(graph.FindNode(shape)->outputs[0].id,graph.FindNode(noise)->inputs[0].id);
        graph.CreateLink(graph.FindNode(noise)->outputs[0].id,graph.FindNode(output)->inputs[0].id);
        auto& settings=std::get<tg::graph::CloudNoiseSettings>(graph.FindMutableNode(noise)->settings);
        Check(graph.CompileCloud().cloud.noiseType==2,"従来の手続き雲は低周波Perlinを維持する");
        settings.noiseType=1;
        Check(graph.CompileCloud().cloud.noiseType==0,"手続き雲のfBMを共通描画設定へ渡す");
        settings.noiseType=2;
        Check(graph.CompileCloud().cloud.noiseType==1,"手続き雲のPerlin-Worleyを共通描画設定へ渡す");
        settings.noiseType=0;
        settings.displacement=120;
        const auto bounds=graph.CompileCloud().cloud;
        Check(std::abs(bounds.thickness-600)<0.01f,
            "薄い楕円体の高さに長軸用の余白を加えない");
        settings.detail=500; settings.feather=500;
        const auto inward=graph.CompileCloud().cloud;
        Check(inward.width==bounds.width && inward.thickness==bounds.thickness && inward.depth==bounds.depth,
            "内向きの削りと境界幅で包囲範囲を広げない");
        settings.displacement=0;
        const auto exact=graph.CompileCloud().cloud;
        Check(exact.width==2400 && exact.thickness==360 && exact.depth==1400,
            "変位なしの単体は元形状の包囲範囲に一致する");
        graph.DeleteNode(shape);
        Check(graph.CompileCloudShapes(shape).primitives.empty(),"削除した形状のガイドを残さない");
    }

    Section("雲の時間更新");
    {
        tg::renderer::CloudMotion motion;
        motion.Advance(2.0, true, 10.0f, 0.0f);
        Check(std::abs(motion.z-20.0)<1e-5 && motion.x==0, "風速と経過時間に応じて +Z へ進む");
        motion.Advance(5.0, false, 10.0f, 0.0f);
        Check(std::abs(motion.z-20.0)<1e-5, "一時停止中は位置を維持する");
        motion.Advance(1.0, true, 10.0f, 1.570796327f);
        Check(std::abs(motion.x-10.0)<1e-5, "再開と風向変更は現在の位置から続く");
        using tg::renderer::CloudMotion;
        Check(CloudMotion::LocalNoiseOffset(100.0, 100.0, 2) == -100.0f,
              "移動 400m に対し模様は 300m 進み、範囲との相対位置が変わる");
        Check(CloudMotion::LocalNoiseOffset(100.0, 100.0, 0) == 0.0f,
              "従来の全体移動では模様を固定する");
        Check(CloudMotion::LocalNoiseOffset(-100.0, 100.0, 2) == 100.0f,
              "逆風ではノイズの相対移動も反転する");
        Check(CloudMotion::LocalNoiseOffset(10100.0, 100.0, 2) == -100.0f,
              "長時間の移流は両ノイズに共通の周期で折り返す");
        CloudMotion ratioMotion;
        ratioMotion.Advance(2.0, true, 10.0f, 0.0f, 0.75f);
        Check(ratioMotion.driftZ == 5.0, "既定比率では相対移動が風の 25% になる");
        ratioMotion.Advance(1.0, true, 10.0f, 0.0f, 1.0f);
        Check(ratioMotion.driftZ == 5.0, "比率を 1 にしても現在の模様は飛ばず維持する");
        ratioMotion.Advance(1.0, true, 10.0f, 0.0f, 0.0f);
        Check(ratioMotion.driftZ == 15.0, "比率 0 は模様を空間に固定する相対速度になる");
        ratioMotion.Advance(2.0, false, 10.0f, 0.0f, 0.0f);
        Check(ratioMotion.driftZ == 15.0, "停止中は相対移動も止まる");
        ratioMotion.Reset();
        Check(ratioMotion.driftZ == 0.0, "リセットは模様の位相も戻す");
        motion.Reset();
        Check(motion.x==0 && motion.z==0, "開始位置への復帰は移動量を消す");
        NodeGraph graph;
        const auto terrainRevision = graph.TerrainRevision();
        const auto revision = graph.Revision();
        graph.MarkCloudDirty();
        Check(graph.Revision()!=revision && graph.TerrainRevision()==terrainRevision,
              "雲だけの編集は地形の再コンパイルを要求しない");
        graph.MarkDirty();
        Check(graph.TerrainRevision()!=terrainRevision, "通常のグラフ編集は地形を更新する");
    }

    Section("雲層の分布入力");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        const auto layer = graph.CreateNode(NodeKind::CloudLayer);
        const auto output = graph.CreateNode(NodeKind::CloudOutput);
        const auto mask = graph.CreateNode(NodeKind::MaskNoise);
        const auto layerPin = graph.FindNode(layer)->outputs.front().id;
        const auto inputPin = graph.FindNode(layer)->inputs.front().id;
        const auto maskPin = graph.FindNode(mask)->outputs.front().id;
        Check(graph.CreateLink(layerPin, graph.FindNode(output)->inputs.front().id), "雲層は雲出力へ接続できる");
        Check(graph.CompileCloud().layer && graph.CompileCloud().maskPin == 0, "未接続の雲層は全面分布");
        Check(graph.CreateLink(maskPin, inputPin), "マスクを分布へ接続できる");
        const auto cloud = graph.CompileCloud();
        Check(cloud.maskPin == maskPin && cloud.maskNode == mask, "分布元のピンを保持する");
        Check(cloud.cloud.width == 12000.0f && cloud.cloud.motionMode == 1, "雲層は広い固定範囲が既定");
        Check(cloud.cloud.noiseType == 0, "雲層の既定ノイズは従来の Perlin fBM");
        Check(cloud.cloud.cellCount == 10, "既存の雲層は10セル周期を維持する");
        auto& layerSettings = std::get<tg::graph::CloudNodeSettings>(graph.FindMutableNode(layer)->settings);
        layerSettings.noiseType = 1;
        layerSettings.cellCount = 17;
        graph.MarkCloudDirty();
        Check(graph.CompileCloud().cloud.cellCount == 17, "雲層の繰り返しセル数を描画へ渡す");
        Check(graph.CompileCloud().cloud.noiseType == 1 && graph.CompileCloud().maskPin == maskPin,
            "Perlin-Worley の選択と分布マスクを同時に描画へ渡す");
        const auto compiled = graph.CompileLayersTo(cloud.maskNode, cloud.maskPin);
        Check(!compiled.maskOps.empty(), "分布マスクを既存の評価プログラムへ変換できる");
        Check(!graph.CanCreateLink(layerPin, inputPin), "Volume を分布へ接続しない");
        graph.DeleteNode(mask);
        Check(graph.CompileCloud().maskPin == 0, "分布元の削除で未接続へ戻る");
        Check(graph.CompileLayers().layers.size() == 1, "雲層は地形出力を変えない");
    }

    Section("ノードグラフ — 雲の独立した出力");
    {
        NodeGraph graph = NodeGraph::CreateDefault();
        Check(!graph.CompileCloud().hasOutput, "既存グラフは従来の空設定を使う");
        const auto cloudId = graph.CreateNode(NodeKind::Cloud);
        const auto outputId = graph.CreateNode(NodeKind::CloudOutput);
        const auto* cloud = graph.FindNode(cloudId);
        const auto* output = graph.FindNode(outputId);
        const auto cloudPin = cloud->outputs.front().id;
        const auto inputPin = output->inputs.front().id;
        Check(graph.CompileCloud().hasOutput && !graph.CompileCloud().connected,
              "未接続の雲出力は雲を表示しない");
        Check(!graph.CanCreateLink(graph.Nodes().front().outputs.front().id, inputPin),
              "Material と Volume は接続できない");
        Check(graph.CreateLink(cloudPin,inputPin) && graph.CompileCloud().connected,
              "雲塊を雲出力へ接続できる");
        auto& settings = std::get<tg::graph::CloudNodeSettings>(graph.FindMutableNode(cloudId)->settings);
        settings.centerY = -150.0f;
        settings.width = 900.0f;
        settings.noiseType = 1;
        settings.enabled = false;
        const auto compiled = graph.CompileCloud();
        Check(compiled.cloud.centerY == -150.0f && compiled.cloud.width == 900.0f && !compiled.cloud.enabled,
              "位置・寸法・有効状態が描画用設定へ伝わる");
        Check(compiled.cloud.noiseType == 1, "雲塊にも Perlin-Worley の選択が伝わる");
        Check(graph.CreateNode(NodeKind::CloudOutput) == 0, "雲出力は重複して作れない");
        Check(graph.CompileLayers().layers.size() == 1, "雲の接続は地形の出力へ混入しない");
        graph.DeleteNode(cloudId);
        Check(graph.CompileCloud().hasOutput && !graph.CompileCloud().connected,
              "雲塊を削除すると古い雲が残らない");
        graph.DeleteNode(outputId);
        Check(!graph.CompileCloud().hasOutput, "雲出力を削除すると従来の空設定へ戻る");
    }

    Section("ノードグラフ — 入力のないハイト加工");

    constexpr std::array kOperationKinds = {
        NodeKind::Blur,      NodeKind::Sediment, NodeKind::Crumbling,
        NodeKind::MeanderingRivers, NodeKind::Lake, NodeKind::SnowCover, NodeKind::Snow, NodeKind::River,    NodeKind::Droplet,
        NodeKind::MultiScaleErosion,
        NodeKind::FluvialErosion,
        NodeKind::FlattenBorders,
    };
    for (const NodeKind kind : kOperationKinds) {
        NodeGraph graph;
        const tg::graph::GraphId operationId = graph.CreateNode(kind);
        Check(IsNeutralPlane(graph.CompileLayersTo(operationId)),
              "Base 未接続の加工ノードは変位 0 の平面になる");
    }

    {
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId blurId = graph.CreateNode(NodeKind::Blur);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* blur = graph.FindNode(blurId);
        const bool connected = base != nullptr && blur != nullptr && !base->outputs.empty() &&
                               !blur->inputs.empty() &&
                               graph.CreateLink(base->outputs.front().id, blur->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(blurId);
        Check(connected && compiled.layers.size() == 2 &&
                  compiled.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  compiled.layers.back().kind == tg::compositor::LayerKind::Blur,
              "Base 接続中の加工ノードは入力と加工を保つ");
    }

    Section("ノードグラフ — Fluvial Erosion の硬度と補助出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto erosionId = graph.CreateNode(NodeKind::FluvialErosion);
        const auto noiseId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* erosion = graph.FindNode(erosionId);
        const auto* noise = graph.FindNode(noiseId);
        Check(erosion->inputs.size() == 3 && erosion->outputs.size() == 4,
              "侵食範囲と硬度を受け、地形・侵食量・堆積量・Age を返す");
        Check(graph.CreateLink(base->outputs[0].id, erosion->inputs[0].id), "地形入力を接続");
        Check(graph.CreateLink(noise->outputs[0].id, erosion->inputs[2].id), "硬度入力を接続");
        const auto compiled = graph.CompileLayersTo(erosionId);
        Check(compiled.layers.size() == 2 && compiled.layers.back().hardnessMaskOp >= 0 &&
              compiled.layers.back().mask.maskOp == -1,
              "侵食範囲が未接続でも硬度を独立した op として評価する");
        for (size_t i = 1; i < 4; ++i) {
            const auto preview = graph.CompileLayersTo(erosionId, erosion->outputs[i].id);
            bool found = false;
            for (const auto& op : preview.maskOps)
                found |= op.kind == tg::compositor::MaskOpKind::FluvialErosion && op.dropletMask.channel == i-1;
            Check(found, "補助出力のプレビューが対応する成分を参照する");
        }
    }

    Section("ノードグラフ — Meandering Rivers のパスと分岐");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto riverId = graph.CreateNode(NodeKind::MeanderingRivers);
        const auto pathId = graph.CreateNode(NodeKind::Path);
        const auto surfaceId = graph.CreateNode(NodeKind::Surface);
        const auto* base = graph.FindNode(baseId);
        const auto* river = graph.FindNode(riverId);
        const auto* pathNode = graph.FindNode(pathId);
        const auto* surface = graph.FindNode(surfaceId);
        Check(river->inputs.size() == 2 && river->inputs[1].valueType == tg::graph::ValueType::Path &&
            river->outputs.size() == 2, "地形と Path を受け、Result と River を出す");
        Check(graph.CreateLink(base->outputs[0].id, river->inputs[0].id) &&
            graph.CreateLink(pathNode->outputs[0].id, river->inputs[1].id), "地形と Path を接続できる");
        auto& path = std::get<tg::graph::PathNodeSettings>(graph.FindMutableNode(pathId)->settings).path;
        const auto a = tg::graph::AddPathPoint(path, 0.1f, 0.5f, 0);
        const auto b = tg::graph::AddPathPoint(path, 0.9f, 0.5f, a);
        path.FindPoint(a)->heightOffsetMeters = 2.0f;
        const auto compiled = graph.CompileLayersTo(riverId);
        const auto& samples = compiled.layers.back().meanderPoints;
        Check(!samples.empty() && samples.front().u == 0.1f && samples.back().u == 0.9f &&
            samples.front().along == 0.0f && samples.back().along == 1.0f &&
            samples.front().heightOffset == 2.0f, "向き・端点・高さオフセットを引き継ぐ");
        const auto maskPreview = graph.CompileLayersTo(riverId, river->outputs[1].id);
        bool found = false;
        for (const auto& op : maskPreview.maskOps) found |= op.kind == tg::compositor::MaskOpKind::MeanderingRivers;
        Check(found, "River マスクのプレビューを生成できる");
        Check(graph.CreateLink(base->outputs[0].id, surface->inputs[0].id) &&
            graph.CreateLink(river->outputs[1].id, surface->inputs[1].id), "River だけを別枝で使える");
        const auto branch = graph.CompileLayersTo(surfaceId);
        Check(branch.layers.size() == 3 && branch.layers[1].maskOnly &&
            !branch.layers[1].meanderPoints.empty(), "マスクだけの枝にもパスを渡し、地形を変更しない");
        tg::graph::ReversePathEdge(path, path.edges.front().id);
        const auto reversed = tg::graph::BuildMeanderPoints(path, 1024, 10);
        Check(!reversed.empty() && reversed.front().u == 0.9f && reversed.back().u == 0.1f,
            "ID 順に依存せずパスの矢印方向を使う");
        path.edges.clear();
        Check(graph.CompileLayersTo(riverId).layers.back().meanderPoints.empty(), "Path を空にすると古い川筋を残さない");
        tg::graph::ConnectPathPoints(path, a, b);
        const auto c = tg::graph::AddPathPoint(path, 0.5f, 0.8f, b);
        tg::graph::ConnectPathPoints(path, c, a);
        Check(tg::graph::BuildMeanderPoints(path, 1024, 10).empty(), "閉じた Path を川として評価しない");
    }

    Section("ノードグラフ — Lake の独立した出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto lakeId = graph.CreateNode(NodeKind::Lake);
        const auto maskId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* lake = graph.FindNode(lakeId);
        const auto* mask = graph.FindNode(maskId);
        Check(lake->inputs.size() == 2 && lake->outputs.size() == 4,
              "Base / Mask と Result / Lake / Depth / Water Level を持つ");
        Check(graph.CreateLink(base->outputs[0].id, lake->inputs[0].id) &&
              graph.CreateLink(mask->outputs[0].id, lake->inputs[1].id), "給水範囲を接続できる");
        auto* settings = std::get_if<tg::graph::LayerNodeSettings>(&graph.FindMutableNode(lakeId)->settings);
        settings->layer.lake.allowOutflow = true;
        settings->layer.lake.waterAmount = 3.5f;
        graph.MarkDirty();
        const auto result = graph.CompileLayersTo(lakeId);
        Check(result.layers.size() == 2 && result.layers.back().kind == tg::compositor::LayerKind::Lake &&
              result.layers.back().lake.allowOutflow && result.layers.back().lake.waterAmount == 3.5f &&
              result.layers.back().mask.maskOp >= 0, "専用設定と給水マスクを保持する");
        for (size_t i = 1; i < 4; ++i) {
            const auto preview = graph.CompileLayersTo(lakeId, lake->outputs[i].id);
            bool found = false;
            for (const auto& op : preview.maskOps)
                found |= op.kind == tg::compositor::MaskOpKind::Lake && op.dropletMask.channel == i - 1;
            Check(found, "湖・水深・水位を取り違えずプレビューする");
        }
    }

    Section("ノードグラフ — Snow Cover の独立した出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto snowId = graph.CreateNode(NodeKind::SnowCover);
        const auto maskId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* snow = graph.FindNode(snowId);
        const auto* mask = graph.FindNode(maskId);
        Check(snow->inputs.size() == 2 && snow->outputs.size() == 4,
              "Base / Mask と Result / Cover / Depth / Flows を持つ");
        Check(graph.CreateLink(base->outputs[0].id, snow->inputs[0].id) &&
              graph.CreateLink(mask->outputs[0].id, snow->inputs[1].id), "降雪範囲を接続できる");
        auto* settings = std::get_if<tg::graph::LayerNodeSettings>(&graph.FindMutableNode(snowId)->settings);
        settings->layer.snowCover.dusting = true;
        settings->layer.snowCover.snowfallDepth = 3.5f;
        graph.MarkDirty();
        const auto result = graph.CompileLayersTo(snowId);
        Check(result.layers.size() == 2 && result.layers.back().kind == tg::compositor::LayerKind::SnowCover &&
              result.layers.back().snowCover.dusting && result.layers.back().snowCover.snowfallDepth == 3.5f &&
              result.layers.back().mask.maskOp >= 0, "専用設定と降雪マスクを保持する");
        for (size_t i = 1; i < 4; ++i) {
            const auto preview = graph.CompileLayersTo(snowId, snow->outputs[i].id);
            bool found = false;
            for (const auto& op : preview.maskOps)
                found |= op.kind == tg::compositor::MaskOpKind::SnowCover && op.dropletMask.channel == i - 1;
            Check(found, "被覆・雪深・流動量を取り違えずプレビューする");
        }
    }

    Section("ノードグラフ — Sediment の Emission 入力");
    {
        // Emission に繋いだマスクは、堆積レイヤーの Mask 入力（供給元）として op へ落ちる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId noiseId = graph.CreateNode(NodeKind::MaskNoise);
        const tg::graph::GraphId sedimentId = graph.CreateNode(NodeKind::Sediment);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* noise = graph.FindNode(noiseId);
        const tg::graph::Node* sediment = graph.FindNode(sedimentId);
        const bool hasPins = base != nullptr && noise != nullptr && sediment != nullptr &&
                             !base->outputs.empty() && !noise->outputs.empty() &&
                             sediment->inputs.size() == 2 &&
                             sediment->inputs[1].valueType == tg::graph::ValueType::Mask;
        const bool connected =
            hasPins && graph.CreateLink(base->outputs.front().id, sediment->inputs[0].id) &&
            graph.CreateLink(noise->outputs.front().id, sediment->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(sedimentId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().kind == tg::compositor::LayerKind::Sediment &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size();
        Check(connected && wired, "Sediment の Emission 入力は供給元のマスク op になる");

        // 繋がなければ供給元は無し（全面へ一様）。
        NodeGraph plain;
        const tg::graph::GraphId plainBaseId = plain.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId plainSedimentId = plain.CreateNode(NodeKind::Sediment);
        const tg::graph::Node* plainBase = plain.FindNode(plainBaseId);
        const tg::graph::Node* plainSediment = plain.FindNode(plainSedimentId);
        const bool plainConnected =
            plainBase != nullptr && plainSediment != nullptr && !plainBase->outputs.empty() &&
            !plainSediment->inputs.empty() &&
            plain.CreateLink(plainBase->outputs.front().id, plainSediment->inputs[0].id);
        const tg::graph::CompiledGraph plainCompiled = plain.CompileLayersTo(plainSedimentId);
        Check(plainConnected && plainCompiled.layers.size() == 2 &&
                  plainCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "Emission 未接続の Sediment は供給元を持たない");
    }

    Section("ノードグラフ — Snow の Mask 入力");
    {
        // Mask に繋いだマスクは、積雪レイヤーの Mask 入力（降らせる場所）として op へ落ちる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId heightId = graph.CreateNode(NodeKind::MaskHeight);
        const tg::graph::GraphId snowId = graph.CreateNode(NodeKind::Snow);
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* height = graph.FindNode(heightId);
        const tg::graph::Node* snow = graph.FindNode(snowId);
        const bool hasPins = base != nullptr && height != nullptr && snow != nullptr &&
                             !base->outputs.empty() && !height->outputs.empty() &&
                             snow->inputs.size() == 2 &&
                             snow->inputs[1].valueType == tg::graph::ValueType::Mask;
        const bool connected =
            hasPins && graph.CreateLink(base->outputs.front().id, snow->inputs[0].id) &&
            graph.CreateLink(height->outputs.front().id, snow->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(snowId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().kind == tg::compositor::LayerKind::Snow &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size();
        Check(connected && wired, "Snow の Mask 入力は降らせる場所のマスク op になる");

        // 繋がなければ全面へ一様。
        NodeGraph plain;
        const tg::graph::GraphId plainBaseId = plain.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId plainSnowId = plain.CreateNode(NodeKind::Snow);
        const tg::graph::Node* plainBase = plain.FindNode(plainBaseId);
        const tg::graph::Node* plainSnow = plain.FindNode(plainSnowId);
        const bool plainConnected =
            plainBase != nullptr && plainSnow != nullptr && !plainBase->outputs.empty() &&
            !plainSnow->inputs.empty() &&
            plain.CreateLink(plainBase->outputs.front().id, plainSnow->inputs[0].id);
        const tg::graph::CompiledGraph plainCompiled = plain.CompileLayersTo(plainSnowId);
        Check(plainConnected && plainCompiled.layers.size() == 2 &&
                  plainCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "Mask 未接続の Snow は降らせる場所を持たない");
    }

    Section("パス — まとめて動かす / コピーと貼り付け");
    {
        using tg::graph::PathClip;
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        PathSettings path;
        const PathElementId a = tg::graph::AddPathPoint(path, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(path, 0.4f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(path, 0.4f, 0.4f, b);
        const PathElementId lone = tg::graph::AddPathPoint(path, 0.9f, 0.9f, 0);
        const tg::graph::PathEdge* ab = path.FindEdgeBetween(a, b);
        const tg::graph::PathEdge* bc = path.FindEdgeBetween(b, c);
        Check(ab != nullptr && bc != nullptr && lone != 0, "3 点の鎖と孤立点を作れる");

        // まとめて動かす。0〜1 へ丸める。
        const bool moved = tg::graph::MovePathPoints(path, {a, b, c}, 0.1f, -0.3f);
        const tg::graph::PathPoint* pa = path.FindPoint(a);
        const tg::graph::PathPoint* pc = path.FindPoint(c);
        Check(moved && pa != nullptr && pc != nullptr && std::abs(pa->u - 0.3f) < 1e-5f &&
                  pa->v == 0.0f && std::abs(pc->u - 0.5f) < 1e-5f && std::abs(pc->v - 0.1f) < 1e-5f,
              "MovePathPoints は指定した点だけを動かし、0〜1 へ丸める");
        float cu = 0.0f;
        float cv = 0.0f;
        Check(tg::graph::PathPointsCentroid(path, {a, b, c}, cu, cv) &&
                  std::abs(cu - (0.3f + 0.5f + 0.5f) / 3.0f) < 1e-5f,
              "PathPointsCentroid は重心を返す");

        // 鎖を切り出す。エッジは両端の点を連れていき、内部点は持ち越さない。
        if (ab != nullptr && bc != nullptr) {
            tg::graph::PathEdge* mutableAb = const_cast<tg::graph::PathEdge*>(ab);
            mutableAb->routed = true;
            mutableAb->waypoints.push_back({0.35f, 0.1f});
            mutableAb->curve = tg::graph::PathCurve::Cubic;
        }
        PathClip clip;
        const bool extracted = tg::graph::ExtractPathClip(path, {}, {ab->id, bc->id}, clip);
        Check(extracted && clip.points.size() == 3 && clip.edges.size() == 2 &&
                  !clip.edges.front().routed && clip.edges.front().waypoints.empty() &&
                  clip.edges.front().curve == tg::graph::PathCurve::Cubic,
              "ExtractPathClip は鎖の点とエッジを切り出し、内部点は捨てて曲線の性質は残す");

        // 点の集合から切り出すと、その間のエッジだけが付いてくる。
        PathClip pointClip;
        Check(tg::graph::ExtractPathClip(path, {a, b, lone}, {}, pointClip) &&
                  pointClip.points.size() == 3 && pointClip.edges.size() == 1,
              "点の集合の ExtractPathClip は点どうしを結ぶエッジだけを拾う");

        // 貼り付け。ID は振り直され、ずらした位置に同じ形で入る。
        const size_t pointsBefore = path.points.size();
        const size_t edgesBefore = path.edges.size();
        std::vector<PathElementId> pastedPoints;
        std::vector<PathElementId> pastedEdges;
        const bool pasted =
            tg::graph::PastePathClip(path, clip, 0.2f, 0.5f, &pastedPoints, &pastedEdges);
        bool idsFresh = true;
        for (const PathElementId id : pastedPoints) {
            idsFresh &= (id != a && id != b && id != c && id != lone);
        }
        const tg::graph::PathPoint* firstPasted =
            pastedPoints.empty() ? nullptr : path.FindPoint(pastedPoints.front());
        Check(pasted && path.points.size() == pointsBefore + 3 &&
                  path.edges.size() == edgesBefore + 2 && pastedEdges.size() == 2 && idsFresh &&
                  firstPasted != nullptr && std::abs(firstPasted->u - 0.5f) < 1e-5f &&
                  std::abs(firstPasted->v - 0.5f) < 1e-5f &&
                  path.FindEdgeBetween(pastedPoints[0], pastedPoints[1]) != nullptr,
              "PastePathClip は新しい ID で同じ形を、ずらした位置に貼る");
        Check(tg::graph::BuildPathStrands(path).size() == 2,
              "貼った鎖は元の鎖と別の鎖になる");
    }

    Section("パス — 面の線分列と Mask Area");
    {
        using tg::graph::PathElementId;
        using tg::graph::PathSettings;
        // 開いた鎖だけなら面の線分は無い。
        PathSettings open;
        const PathElementId o1 = tg::graph::AddPathPoint(open, 0.2f, 0.2f, 0);
        const PathElementId o2 = tg::graph::AddPathPoint(open, 0.8f, 0.2f, o1);
        tg::graph::AddPathPoint(open, 0.8f, 0.8f, o2);
        Check(tg::graph::BuildPathAreaSegments(open).empty(),
              "開いた鎖だけの BuildPathAreaSegments は空");

        // 三角形の輪。線分は輪を一周して先頭へ戻る。
        PathSettings loop;
        const PathElementId a = tg::graph::AddPathPoint(loop, 0.2f, 0.2f, 0);
        const PathElementId b = tg::graph::AddPathPoint(loop, 0.8f, 0.2f, a);
        const PathElementId c = tg::graph::AddPathPoint(loop, 0.5f, 0.8f, b);
        tg::graph::ConnectPathPoints(loop, c, a);
        const auto segments = tg::graph::BuildPathAreaSegments(loop);
        bool chained = !segments.empty();
        for (size_t i = 0; i + 1 < segments.size(); ++i) {
            chained &= (segments[i].bx == segments[i + 1].ax && segments[i].by == segments[i + 1].ay);
        }
        const bool closed = !segments.empty() && segments.back().bx == segments.front().ax &&
                            segments.back().by == segments.front().ay;
        Check(segments.size() == 3 && chained && closed,
              "閉じた鎖の BuildPathAreaSegments は輪を一周して先頭へ戻る");

        // グラフ: Path → Mask Area → Surface の Mask。閉じた鎖があれば Area の op になる。
        NodeGraph graph;
        const tg::graph::GraphId baseId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::GraphId areaId = graph.CreateNode(NodeKind::MaskArea);
        const tg::graph::GraphId surfaceId = graph.CreateNode(NodeKind::Surface);
        tg::graph::Node* pathNode = graph.FindMutableNode(pathId);
        if (auto* settings = std::get_if<tg::graph::PathNodeSettings>(&pathNode->settings)) {
            settings->path = loop;
        }
        const tg::graph::Node* base = graph.FindNode(baseId);
        const tg::graph::Node* area = graph.FindNode(areaId);
        const tg::graph::Node* surface = graph.FindNode(surfaceId);
        const bool linked =
            graph.CreateLink(pathNode->outputs.front().id, area->inputs.front().id) &&
            graph.CreateLink(base->outputs.front().id, surface->inputs[0].id) &&
            graph.CreateLink(area->outputs.front().id, surface->inputs[1].id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(surfaceId);
        const bool wired = compiled.layers.size() == 2 &&
                           compiled.layers.back().mask.source == tg::compositor::MaskSource::Node &&
                           compiled.layers.back().mask.maskOp >= 0 &&
                           static_cast<size_t>(compiled.layers.back().mask.maskOp) <
                               compiled.maskOps.size() &&
                           compiled.maskOps[static_cast<size_t>(compiled.layers.back().mask.maskOp)]
                                   .kind == tg::compositor::MaskOpKind::Area &&
                           compiled.maskOps.front().pathSegments.size() == 3;
        Check(linked && wired, "Mask Area は閉じた鎖から Area の op になる");

        // 開いた鎖しか無ければ op は作られない（マスクは定数へ落ちる）。
        if (auto* settings = std::get_if<tg::graph::PathNodeSettings>(&pathNode->settings)) {
            settings->path = open;
        }
        graph.MarkDirty();
        const tg::graph::CompiledGraph openCompiled = graph.CompileLayersTo(surfaceId);
        Check(openCompiled.layers.size() == 2 &&
                  openCompiled.layers.back().mask.source != tg::compositor::MaskSource::Node,
              "閉じた鎖が無い Mask Area は op を作らない");
    }

    Section("ノードグラフ — Path の Base");
    {
        NodeGraph graph;
        const tg::graph::GraphId heightmapId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId outputId = graph.CreateNode(NodeKind::Output);
        const tg::graph::GraphId pathId = graph.CreateNode(NodeKind::Path);
        const tg::graph::Node* heightmap = graph.FindNode(heightmapId);
        const tg::graph::Node* output = graph.FindNode(outputId);
        const tg::graph::Node* path = graph.FindNode(pathId);

        const bool outputConnected =
            heightmap != nullptr && output != nullptr && !heightmap->outputs.empty() &&
            !output->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, output->inputs.front().id);
        Check(outputConnected && IsNeutralPlane(graph.CompileLayersTo(pathId)),
              "Base 未接続の Path は Output 側の地形ではなく変位 0 の平面になる");

        const bool pathConnected =
            heightmap != nullptr && path != nullptr && !heightmap->outputs.empty() &&
            !path->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, path->inputs.front().id);
        const tg::graph::CompiledGraph compiled = graph.CompileLayersTo(pathId);
        Check(pathConnected && compiled.layers.size() == 1 &&
                  compiled.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  compiled.layers.front().heightSource != tg::compositor::ValueSource::Constant,
              "Base 接続中の Path は自身の入力地形を表示する");
    }

    Section("ノードグラフ — Mask Flowline の入力とマスク出力");
    {
        NodeGraph graph;
        const auto baseId = graph.CreateNode(NodeKind::Heightmap);
        const auto flowId = graph.CreateNode(NodeKind::MaskFlowline);
        const auto sourceId = graph.CreateNode(NodeKind::MaskNoise);
        const auto outflowId = graph.CreateNode(NodeKind::MaskNoise);
        const auto* base = graph.FindNode(baseId);
        const auto* flow = graph.FindNode(flowId);
        const auto* source = graph.FindNode(sourceId);
        const auto* outflow = graph.FindNode(outflowId);
        Check(flow->inputs.size() == 3 && flow->outputs.size() == 1 &&
              flow->outputs[0].valueType == tg::graph::ValueType::Mask,
              "Base / Source / Outflow を受け、Mask だけを出す");
        Check(graph.CreateLink(base->outputs[0].id, flow->inputs[0].id) &&
              graph.CreateLink(source->outputs[0].id, flow->inputs[1].id) &&
              graph.CreateLink(outflow->outputs[0].id, flow->inputs[2].id), "地形・発生範囲・流出域を接続できる");
        auto& settings = std::get<tg::graph::MaskNodeSettings>(graph.FindMutableNode(flowId)->settings);
        settings.flowline.numberOfFlows = 321;
        settings.flowline.lengthMeters = 45.0f;
        graph.MarkDirty();
        const auto compiled = graph.CompileLayersTo(flowId);
        Check(compiled.maskOps.size() == 3, "依存マスクを先にコンパイルする");
        const auto& op = compiled.maskOps.back();
        Check(op.kind == tg::compositor::MaskOpKind::Flowline && op.inputA == 0 && op.inputB == 1 &&
              op.heightSourceLayer == 0 && op.flowline.numberOfFlows == 321 && op.flowline.lengthMeters == 45.0f,
              "入力の順序・地形の参照位置・専用設定を保持する");
        bool heightUnchanged = true;
        for (size_t i = 1; i < compiled.layers.size(); ++i)
            heightUnchanged &= (compiled.layers[i].channelMask & tg::compositor::ChannelBit(tg::compositor::Channel::Height)) == 0;
        Check(heightUnchanged, "マスクのプレビューは地形の高さを書き換えない");
        Check(!graph.CreateLink(flow->outputs[0].id, flow->inputs[1].id), "自分の出力を発生範囲に戻す循環を拒否する");
        graph.DeleteNode(sourceId);
        const auto disconnected = graph.CompileLayersTo(flowId);
        Check(disconnected.maskOps.back().inputA == -1 && disconnected.maskOps.back().inputB == 0,
              "発生範囲を外しても流出域の接続は保持する");
    }

    Section("ノードグラフ — ハイト由来マスクの Base");
    constexpr std::array kHeightMaskKinds = {
        NodeKind::MaskFlowline,
        NodeKind::MaskFluvial,
        NodeKind::MaskHeight,
        NodeKind::MaskSlope,
        NodeKind::MaskCurvature,
    };
    for (const NodeKind kind : kHeightMaskKinds) {
        NodeGraph graph;
        const tg::graph::GraphId heightmapId = graph.CreateNode(NodeKind::Heightmap);
        const tg::graph::GraphId outputId = graph.CreateNode(NodeKind::Output);
        const tg::graph::GraphId maskId = graph.CreateNode(kind);
        const tg::graph::Node* heightmap = graph.FindNode(heightmapId);
        const tg::graph::Node* output = graph.FindNode(outputId);
        const tg::graph::Node* mask = graph.FindNode(maskId);

        const bool outputConnected =
            heightmap != nullptr && output != nullptr && !heightmap->outputs.empty() &&
            !output->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, output->inputs.front().id);
        const tg::graph::CompiledGraph disconnected = graph.CompileLayersTo(maskId);
        Check(outputConnected && StartsWithNeutralPlane(disconnected),
              "Base 未接続のハイト由来マスクは変位 0 の平面上で表示する");

        const bool maskConnected =
            heightmap != nullptr && mask != nullptr && !heightmap->outputs.empty() &&
            !mask->inputs.empty() &&
            graph.CreateLink(heightmap->outputs.front().id, mask->inputs.front().id);
        const tg::graph::CompiledGraph connected = graph.CompileLayersTo(maskId);
        Check(maskConnected && !connected.layers.empty() &&
                  connected.layers.front().kind == tg::compositor::LayerKind::Shape &&
                  connected.layers.front().heightSource != tg::compositor::ValueSource::Constant,
              "Base 接続中のハイト由来マスクは自身の入力地形上で表示する");
    }
}
