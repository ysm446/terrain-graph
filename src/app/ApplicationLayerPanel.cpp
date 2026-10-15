// レイヤー（グラフノードの設定）のプロパティ行とペイントマスクの節。
// レイヤーパネルは廃止済みで、グラフパネルの下段から使われる。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/ColorSpace.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

// レイヤー 1 枚ぶんのプロパティ行。グラフパネルの下段から使う。
// 変更の記録（アンドゥ / グラフの再コンパイル）は呼び出し側で行う。
namespace {

// Lake / River の「水」の節。水の色と水チャンネルを書くかどうかと、色の行。
// 深い所の色はレイヤーのベースカラーを使う（Liquid と同じ持ち方）。withWaves は波の強さの行を出すか
// （River は流れの節に自分の波を持つので出さない）。
bool DrawWaterAppearance(compositor::MaterialLayer& layer, const compositor::MaterialLayer& defaults,
                         bool withWaves, const char* tableId) {
    bool changed = false;
    ui::SectionHeader(ui::Tr("水", "Water"));
    if (ui::BeginPropertyTable(tableId)) {
        changed |= ui::PropertyBool(ui::Tr("水を描く", "Paint Water"), &layer.liquid.paintWater, defaults.liquid.paintWater,
                                    ui::Tr("このノードが水の色を付け、水チャンネル（波・波打ち際・流れの範囲）を書く。"
                                           "切ると形だけを作る（色は後ろの Surface で付ける今までの使い方）",
                                           "This node colors the water and writes the water channels (waves, shoreline, flow area). "
                                           "When off, only the shape is made (color it with a later Surface, as before)"));
        if (layer.liquid.paintWater) {
            changed |= ui::PropertyColorLinear(ui::Tr("深い所の色", "Deep Color"), &layer.baseColor.x, &defaults.baseColor.x,
                                               ui::Tr("水の深い所の色", "Color of deep water"));
            changed |= ui::PropertyColorLinear(ui::Tr("浅瀬の色", "Shallow Color"), &layer.liquid.shallowColor.x,
                                               &defaults.liquid.shallowColor.x,
                                               ui::Tr("水の浅い所の色。深くなるほど深い所の色へ寄る",
                                                      "Color of shallow water. Shifts toward the deep color as depth increases"));
            changed |= ui::PropertyFloat(ui::Tr("色の変わる深さ", "Color Change Depth"), &layer.liquid.colorDepthMeters, 0.0f, 100.0f,
                                         defaults.liquid.colorDepthMeters,
                                         ui::Tr("浅瀬の色から深い所の色へ変わる水深（m）。0 で深い所の色だけ",
                                                "Water depth (m) over which the shallow color turns into the deep color. 0 uses only the deep color"), "%.1f m");
            changed |= ui::PropertyFloat(ui::Tr("透ける深さ", "Clarity Depth"), &layer.liquid.clarityMeters, 0.0f, 50.0f,
                                         defaults.liquid.clarityMeters,
                                         ui::Tr("水越しに底の色が見える水深（m）。大きいほど澄んだ水。0 で透けない",
                                                "Water depth (m) through which the bottom is visible. Larger is clearer water. 0 is opaque"),
                                         "%.1f m");
            if (withWaves) {
                changed |= ui::PropertyFloat(ui::Tr("波の強さ", "Wave Strength"), &layer.liquid.waveStrength, 0.0f, 1.0f,
                                             defaults.liquid.waveStrength,
                                             ui::Tr("この水面の波の傾きの強さ。0 で平らな水面",
                                                    "Slope strength of the waves on this water surface. 0 is a flat surface"), "%.2f");
            }
        }
        ui::EndPropertyTable();
    }
    return changed;
}

}  // namespace

bool Application::DrawLayerSettings(compositor::MaterialLayer& layer, bool isBase, bool isSource,
                                   bool maskFromNode, bool maskResolves, bool pathUv) {
    // **困っていることは一番上に出す。** 設定の行が多いレイヤーだと、
    // マスクの節はスクロールの外へ行ってしまう。
    if (!maskResolves) {
        ui::HintText("%s", ui::Tr("Mask 入力に繋いだノードがこのチェーンの中にいないので、"
                                  "マスクが効いていない。そのノードの Result を下地として通すこと",
                                  "The node connected to the Mask input is not in this chain, so the mask has no effect. "
                                  "Pass that node's Result through as the base"));
    }
    // 既定値マーカーは種類ごとの既定値を参照する（追加時の初期値と揃える）。
    const compositor::MaterialLayer& defaults = DefaultLayerFor(layer.kind);
    const bool isShape = (layer.kind == compositor::LayerKind::Shape);
    const bool isLiquid = (layer.kind == compositor::LayerKind::Liquid);
    bool changed = false;
    const auto* assignedMaterial = m_materialLibrary.Find(layer.material);
    const bool layeredMaterial = assignedMaterial && assignedMaterial->layerMaterial;
    if (layeredMaterial && !assignedMaterial->layerError.empty()) ui::HintText(assignedMaterial->layerError.c_str());
    if (layeredMaterial && assignedMaterial->layerMaterial &&
        std::any_of(assignedMaterial->layerMaterial->materials.begin(), assignedMaterial->layerMaterial->materials.end(),
                    [](const graph::PresetMaterial& m) {
                        return m.mask && (m.mask->shape == graph::RoadMaskShape::WheelTracks ||
                                          m.mask->shape == graph::RoadMaskShape::EdgeFalloff);
                    }))
        ui::HintText("%s", ui::Tr("轍・道路端のマスクは Road Mesh の路面でだけ効く（Surface では覆わない）",
                                  "Wheel track and road edge masks only work on the Road Mesh surface (Surface does not cover them)"));

    // 堆積は合成レイヤーではなく「下地のハイトを土砂で作り替える加工」。
    if (layer.kind == compositor::LayerKind::Sediment) {
        const compositor::MaterialLayer::SedimentSettings sedimentDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("sedimentBasicRows")) {
            char sedimentName[128] = {};
            std::snprintf(sedimentName, sizeof(sedimentName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), sedimentName, sizeof(sedimentName))) {
                layer.name = sedimentName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("堆積", "Deposition"));
        if (ui::BeginPropertyTable("sedimentRows")) {
            changed |= ui::PropertyBool(
                ui::Tr("地形を土砂にする", "Convert Terrain"), &layer.sediment.convertTerrain,
                sedimentDefaults.convertTerrain,
                ui::Tr("入の間は入力の地形そのものが崩れて谷を埋める。"
                       "切ると入力は動かない基盤になり、供給量で足したぶんだけが流れる",
                       "When on, the input terrain itself collapses and fills the valleys. "
                       "When off, the input is fixed bedrock and only the added Emission flows"));
            changed |= ui::PropertyFloat(ui::Tr("供給量", "Emission"), &layer.sediment.emissionMeters, 0.0f, 20.0f,
                                         sedimentDefaults.emissionMeters,
                                         ui::Tr("上乗せする土砂の厚み（m）。Emission 入力を繋ぐと"
                                                "その明るさに比例して積む（繋がなければ全面へ一様）",
                                                "Thickness (m) of sediment to add. With the Emission input connected, "
                                                "it is deposited in proportion to its brightness (uniform everywhere otherwise)"),
                                         "%.2f m");
            changed |= ui::PropertyFloat(
                ui::Tr("供給時間", "Emission Time"), &layer.sediment.emissionTime, 0.0f, 1.0f,
                sedimentDefaults.emissionTime,
                ui::Tr("0 で最初に全量を積む。上げるほど反復に分けて積むので、"
                       "前の反復が彫った河道に流れ込んで筋がはっきりする",
                       "0 deposits everything at the start. Higher spreads it over the iterations, "
                       "so it flows into channels carved earlier and the streaks get sharper"),
                "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("最大ディテール", "Max Detail"), &layer.sediment.detailMeters, 1.0f, 512.0f,
                sedimentDefaults.detailMeters,
                ui::Tr("1 反復でどこまで土砂を運ぶか（m）。大きいほど広い盆地が早く落ち着くが重い",
                       "How far (m) sediment travels per iteration. Larger settles wide basins faster but is heavier"),
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("粘性", "Viscosity"), &layer.sediment.viscosity, 0.0f, 1.0f, sedimentDefaults.viscosity,
                ui::Tr("安息角。0 で水平に均され、上げるほど急な斜面のまま留まる（1 で 80 度）",
                       "Angle of repose. 0 levels flat; higher keeps steeper slopes (80 degrees at 1)"),
                "%.2f");
            changed |= ui::PropertyInt(ui::Tr("反復", "Iterations"), &layer.sediment.iterations, 1, 200,
                                       sedimentDefaults.iterations,
                                       ui::Tr("崩し直す回数。多いほど落ち着くが重い",
                                              "Number of collapse passes. More settles further but is heavier"));
            changed |= ui::PropertyInt(ui::Tr("安定化", "Stabilization"), &layer.sediment.stabilization, 1, 8,
                                       sedimentDefaults.stabilization,
                                       ui::Tr("1 反復のなかで何回滑らせるか。"
                                              "粘性が低いときは上げると暴れにくい",
                                              "Slide passes per iteration. "
                                              "Raise it with low viscosity to keep things stable"));
            int resolutionIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.sediment.resolution) {
                    resolutionIndex = i;
                }
            }
            if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &resolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 1,
                                  ui::Tr("土砂を動かすグリッド。合成解像度とは別。"
                                         "上げるほど細かい筋が出るが、反復のコストも比例して増える",
                                         "Grid that moves the sediment, separate from the composite resolution. "
                                         "Higher shows finer streaks, but each iteration costs more in proportion"))) {
                layer.sediment.resolution = kSedimentResolutionValues[resolutionIndex];
                changed = true;
            }
            changed |= ui::PropertyFloat(
                ui::Tr("基準の厚み", "Ref. Thickness"), &layer.sediment.maskThicknessMeters, 0.0f, 20.0f,
                sedimentDefaults.maskThicknessMeters,
                ui::Tr("Mask 出力がこの厚みで 1 になる。0 にすると一番厚い所が 1 に"
                       "なるが、少数の分厚い点が基準になって残りが 0 付近へ潰れる",
                       "The Mask output reaches 1 at this thickness. At 0 the thickest spot becomes 1, "
                       "but a few very thick points set the scale and the rest gets crushed near 0"),
                "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("マスクの締まり", "Mask Contrast"), &layer.sediment.maskContrast, 0.0f, 1.0f,
                sedimentDefaults.maskContrast,
                ui::Tr("Mask 出力（積もった厚み）のコントラスト。0 で線形、"
                       "上げるほど「積もった / 積もっていない」がはっきり分かれる",
                       "Contrast of the Mask output (deposited thickness). 0 is linear; "
                       "higher separates deposited and bare areas more sharply"),
                "%.2f");
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("重力で土砂を再分配する。谷底に厚く積もり、尾根は痩せる。"
                                  "Emission 入力で供給する場所を絞れる（「地形を土砂にする」を切り、"
                                  "粘性を下げると、注ぎ口から流した液体になる）。"
                                  "Mask 出力は積もった厚みなので、堆積した所へ別のマテリアルを乗せられる",
                                  "Redistributes sediment by gravity. It piles up in valley floors and thins the ridges. "
                                  "The Emission input limits where it is supplied (turn off Convert Terrain "
                                  "and lower Viscosity to get liquid poured from a spout). "
                                  "The Mask output is the deposited thickness, so you can put another material on the deposits"));
        return changed;
    }

    if (layer.kind == compositor::LayerKind::MeanderingRivers) {
        auto& p = layer.meanderingRivers;
        const compositor::MaterialLayer::MeanderingRiversSettings d;
        if (ui::BeginPropertyTable("meanderingRivers", ui::Tr("地形の影響", "Terrain Influence"))) {
            changed |= ui::PropertyInt(ui::Tr("反復回数", "Iterations"), &p.iterations, 0, 512, d.iterations, ui::Tr("蛇行を進める回数。0 では入力した川筋を使う", "Number of meander steps. 0 uses the input river course as is"));
            changed |= ui::PropertyFloat(ui::Tr("川幅", "River Width"), &p.riverWidth, 0.1f, 1000.0f, d.riverWidth, ui::Tr("河床を掘る幅", "Width of the carved riverbed"), "%.2f m");
            changed |= ui::PropertyFloat(ui::Tr("蛇行スケール", "Meander Scale"), &p.meanderScale, 0.1f, 10.0f, d.meanderScale, ui::Tr("川幅に対する標本間隔と移動距離の倍率", "Sample spacing and step distance relative to the river width"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("蛇行の強さ", "Meander Strength"), &p.intensity, 0.0f, 2.0f, d.intensity, ui::Tr("川筋を進める強さ。両端は固定する", "How strongly the course moves. Both ends stay fixed"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("地形の影響", "Terrain Influence"), &p.heightInfluence, 0.0f, 10.0f, d.heightInfluence, ui::Tr("低い地形へ向かう強さ", "How strongly the river heads toward lower ground"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("平滑化", "Smoothing"), &p.smoothing, 0.0f, 1.0f, d.smoothing, ui::Tr("反復ごとに川筋を滑らかにする強さ", "How much the course is smoothed each iteration"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("深さの比率", "Depth Ratio"), &p.riverDepth, 0.0f, 2.0f, d.riverDepth, ui::Tr("川幅に対する掘り込みの深さ。0 では河床を掘らない", "Carving depth relative to the river width. 0 does not carve a riverbed"), "%.3f");
            changed |= ui::PropertyFloat(ui::Tr("岸の揺らぎ", "Bank Noise"), &p.bankNoise, 0.0f, 0.5f, d.bankNoise, ui::Tr("道のりに沿って川幅を変化させる割合", "How much the width varies along the river"), "%.2f");
            changed |= ui::PropertyInt(ui::Tr("シード", "Seed"), &p.seed, 0, 1000000, d.seed, ui::Tr("初期の揺らぎと岸の模様を変える", "Changes the initial wobble and the bank pattern"));
            changed |= ui::PropertyBool(ui::Tr("上り勾配を除去", "Remove Uphill"), &p.flattenUphill, d.flattenUphill, ui::Tr("Path の矢印方向へ川の基準高が上がらないようにする", "Keeps the river's base height from rising in the Path's arrow direction"));
            changed |= ui::PropertyBool(ui::Tr("流域を整形", "Shape Basin"), &p.basinEnabled, d.basinEnabled, ui::Tr("入力した川筋の周囲を川の基準高へ馴染ませる。盛り上がる場所もある", "Blends the area around the input course into the river's base height. Some places may rise"));
            changed |= ui::PropertyFloat(ui::Tr("流域の半幅", "Basin Half Width"), &p.basinWidth, 0.0f, 5000.0f, d.basinWidth, ui::Tr("入力した川筋から流域の外縁までの距離", "Distance from the input course to the basin's outer edge"), "%.1f m");
            changed |= ui::PropertyFloat(ui::Tr("流域の深さ", "Basin Depth"), &p.basinDepth, 0.0f, 1000.0f, d.basinDepth, ui::Tr("川の基準高から流域を下げる距離", "How far the basin is lowered from the river's base height"), "%.2f m");
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("Base に地形、Path に上流から下流へ向く開いた線を接続する。"
            "分岐・閉じた線・途中で向きが反転する線には対応しない。"
            "Result は地形、River は河床の範囲。水面は生成しない。",
            "Connect the terrain to Base and an open line running from upstream to downstream to Path. "
            "Branches, closed lines, and lines that reverse direction are not supported. "
            "Result is the terrain and River is the riverbed area. No water surface is generated."));
        return changed;
    }
    // 湖は給水マスクを受けて水を移動し、水面まで地形を持ち上げる。
    if (layer.kind == compositor::LayerKind::Lake) {
        auto& p = layer.lake;
        const compositor::MaterialLayer::LakeSettings d;
        ui::SectionHeader(ui::Tr("湖", "Lake"));
        if (ui::BeginPropertyTable("lakeRows")) {
            char name[128]{};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) { layer.name = name; changed = true; }
            changed |= ui::PropertyBool(ui::Tr("範囲を湖面にする", "Fill Area as Lake"), &p.areaMode, d.areaMode,
                ui::Tr("水を溜めず、Water Mask の範囲をそのまま湖にする。湖面が平らなハイトマップ（窪みが無い）用。"
                "水位は範囲の中の地面の平均の高さ",
                "Turns the Water Mask area directly into a lake without pooling water. For heightmaps where the lake is flat (no basin). "
                "The water level is the average ground height inside the area"));
            if (p.areaMode) {
                changed |= ui::PropertyFloat(ui::Tr("最大水深", "Max Depth"), &p.areaDepthMeters, 0.0f, 500.0f, d.areaDepthMeters,
                    ui::Tr("Water Mask が 1 の所の水深。湖底は水位から「マスク × 最大水深」だけ掘る"
                    "（岸で 0、沖で 1 のマスクにすると岸から深くなる）",
                    "Water depth where the Water Mask is 1. The lake bed is carved mask x Max Depth below the water level "
                    "(a mask of 0 at the shore and 1 offshore deepens away from the shore)"), "%.1f m");
            } else {
                changed |= ui::PropertyFloat(ui::Tr("水量", "Water Amount"), &p.waterAmount, 0.0f, 100.0f, d.waterAmount,
                    ui::Tr("全面に供給する水の厚さ。Water Mask 入力で供給場所を絞る",
                           "Thickness of water supplied everywhere. The Water Mask input limits where it is supplied"), "%.3f m");
                changed |= ui::PropertyInt(ui::Tr("最適化段数", "Optimization Steps"), &p.optimizationSteps, 0, 10, d.optimizationSteps,
                    ui::Tr("大きいほど粗い格子で水を移動する。小さな窪みの結果も変化する",
                           "Higher moves water on coarser grids. Results in small basins change too"));
                changed |= ui::PropertyBool(ui::Tr("境界から流出", "Outflow at Edges"), &p.allowOutflow, d.allowOutflow,
                    ui::Tr("地形の端から外へ水を流す", "Lets water flow out over the terrain edges"));
                changed |= ui::PropertyFloat(ui::Tr("基準スケール", "Reference Scale"), &p.referenceDetailScale, 0.01f, 100.0f, d.referenceDetailScale,
                    ui::Tr("水の移動を計算する格子の基準となる距離", "Reference distance for the grid that computes water movement"), "%.3f m");
            }
            ui::EndPropertyTable();
        }
        changed |= DrawWaterAppearance(layer, kDefaultLakeLayer, true, "lakeWaterRows");
        ui::HintText("%s", ui::Tr("窪みに水を溜めて水面の高さへ変形する。Lake は湖の範囲、Depth は水深（m）、"
                                  "Water Level は周囲へ延長した水位（Height と同じ 0〜1 基準）。"
                                  "「水を描く」が入なら、水の色・波・波打ち際はこのノードが付ける。"
                                  "波の大きさや波打ち際の細かい設定は Sea のものを使う（Sea が無ければ既定）",
                                  "Pools water in basins and raises the terrain to the water surface. Lake is the lake area, Depth is the water depth (m), "
                                  "Water Level is the water level extended outward (same 0-1 scale as Height). "
                                  "With Paint Water on, this node adds the water color, waves, and shoreline. "
                                  "Wave size and detailed shoreline settings come from Sea (defaults if there is no Sea)"));
        return changed;
    }
    // 積雪も合成レイヤーではなく「下地のハイトへ雪を積む加工」。
    // 降る量は一様なので、マスクの節は出さない（どこに積もるかは雪面が決める）。
    if (layer.kind == compositor::LayerKind::SnowCover) {
        auto& p = layer.snowCover;
        const compositor::MaterialLayer::SnowCoverSettings d;
        if (ui::BeginPropertyTable("snowCoverBasic")) {
            char name[128]{};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) { layer.name=name; changed=true; }
            ui::EndPropertyTable();
        }
        if (ImGui::CollapsingHeader(ui::Tr("積雪###snowCover0", "Snow###snowCover0"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ui::BeginPropertyTable("snowCover0")) {
                changed |= ui::PropertyBool(ui::Tr("厚い積雪", "Deep Snow"), &p.deepSnow, d.deepSnow, ui::Tr("雪面を変形する積雪を有効にする", "Enables snow that reshapes the snow surface"));
                changed |= ui::PropertyFloat(ui::Tr("特徴サイズ", "Feature Size"), &p.featureSize, 0.01f, 128.0f, d.featureSize, ui::Tr("積雪の大きな形を計算するスケール", "Scale used to compute the large forms of the snow"), "%.3f");
                changed |= ui::PropertyInt(ui::Tr("安定化反復", "Settle Iterations"), &p.settleIterations, 0, 2000, d.settleIterations, ui::Tr("各解像度で雪面を落ち着かせる反復回数", "Iterations that settle the snow surface at each resolution"));
                changed |= ui::PropertyFloat(ui::Tr("降雪深", "Snowfall Depth"), &p.snowfallDepth, 0.0f, 100.0f, d.snowfallDepth, ui::Tr("全面に降らせる雪の厚さ。Mask 入力で分布を指定する", "Thickness of snow that falls everywhere. The Mask input sets the distribution"), "%.2f m");
                changed |= ui::PropertyFloat(ui::Tr("流量補正", "Flow Correction"), &p.flowVolume, -10.0f, 10.0f, d.flowVolume, ui::Tr("流動量の差を雪面の移動に反映する", "Applies differences in flow volume to the movement of the snow surface"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("最大傾斜", "Max Slope"), &p.maxSlope, 0.0f, 89.0f, d.maxSlope, ui::Tr("雪が付着できる最大傾斜。安定化を開始する角度ではない", "Steepest slope snow can stick to. Not the angle where settling starts"), ui::Tr("%.1f 度", "%.1f°"));
                changed |= ui::PropertyFloat(ui::Tr("融雪", "Melt"), &p.melt, 0.0f, 1.0f, d.melt, ui::Tr("降雪深の 2 倍にこの値を掛けた厚さを最後に取り除く", "Removes this value times twice the Snowfall Depth at the end"), "%.3f");
                changed |= ui::PropertyBool(ui::Tr("多段解像度", "Multigrid"), &p.multigrid, d.multigrid, ui::Tr("粗い格子から元の解像度まで段階的に積雪を計算する", "Computes snow in stages from a coarse grid up to the full resolution"));
                changed |= ui::PropertyFloat(ui::Tr("基準スケール", "Reference Scale"), &p.referenceDetailScale, 0.01f, 100.0f, d.referenceDetailScale, ui::Tr("特徴サイズに掛ける基準の距離", "Reference distance multiplied by Feature Size"), "%.2f m");
                ui::EndPropertyTable();
            }
        }
        if (ImGui::CollapsingHeader(ui::Tr("降雪分布###snowCover1", "Snowfall Distribution###snowCover1"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ui::BeginPropertyTable("snowCover1")) {
                changed |= ui::PropertyBool(ui::Tr("降雪線", "Snow Line"), &p.snowLine, d.snowLine, ui::Tr("低い標高で降雪を抑える", "Suppresses snowfall at low elevations"));
                changed |= ui::PropertyFloat(ui::Tr("抑制の強さ", "Suppression"), &p.snowLineStrength, 0.0f, 1.0f, d.snowLineStrength, ui::Tr("降雪線より低い場所で取り除く降雪量の割合", "Fraction of snowfall removed below the snow line"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("標高", "Elevation"), &p.snowLineHeight, -10000.0f, 10000.0f, d.snowLineHeight, ui::Tr("降雪線の下端。Height 0.5 の基準面を標高 0 m とする", "Bottom of the snow line. The Height 0.5 reference plane is elevation 0 m"), "%.2f m");
                changed |= ui::PropertyFloat(ui::Tr("遷移幅", "Falloff"), &p.snowLineFalloff, 0.0f, 1000.0f, d.snowLineFalloff, ui::Tr("降雪線の下端から通常の降雪量に戻るまでの高さ", "Height from the bottom of the snow line to full snowfall"), "%.2f m");
                ui::EndPropertyTable();
            }
        }
        if (ImGui::CollapsingHeader(ui::Tr("風###snowCover2", "Wind###snowCover2"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ui::BeginPropertyTable("snowCover2")) {
                changed |= ui::PropertyBool(ui::Tr("風", "Wind"), &p.wind, d.wind, ui::Tr("斜面の向きから降雪量を補正する", "Adjusts snowfall by slope direction"));
                changed |= ui::PropertyFloat(ui::Tr("風向 X", "Wind Direction X"), &p.windX, -1.0f, 1.0f, d.windX, ui::Tr("風が進む方向の X 成分", "X component of the direction the wind blows"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("風向 Y", "Wind Direction Y"), &p.windY, -1.0f, 1.0f, d.windY, ui::Tr("風が進む方向の鉛直成分", "Vertical component of the direction the wind blows"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("風向 Z", "Wind Direction Z"), &p.windZ, -1.0f, 1.0f, d.windZ, ui::Tr("風が進む方向の Z 成分", "Z component of the direction the wind blows"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("風の強さ", "Wind Strength"), &p.windStrength, 0.0f, 1.0f, d.windStrength, ui::Tr("風向による降雪量の補正強度", "How strongly wind direction adjusts snowfall"), "%.3f");
                changed |= ui::PropertyBool(ui::Tr("風判定をぼかす", "Blur Wind Test"), &p.blurWind, d.blurWind, ui::Tr("風向の判定に使う地形だけをぼかす", "Blurs only the terrain used to test wind direction"));
                changed |= ui::PropertyFloat(ui::Tr("ぼかし半径", "Blur Radius"), &p.windBlurRadius, 0.0f, 512.0f, d.windBlurRadius, ui::Tr("風向の判定用の地形をならす半径", "Radius for smoothing the terrain used to test wind direction"), "%.2f m");
                ui::EndPropertyTable();
            }
        }
        if (ImGui::CollapsingHeader(ui::Tr("薄雪###snowCover3", "Dusting###snowCover3"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ui::BeginPropertyTable("snowCover3")) {
                changed |= ui::PropertyBool(ui::Tr("薄雪", "Dusting"), &p.dusting, d.dusting, ui::Tr("地形を変えずに薄雪の被覆を追加する", "Adds a dusting of snow without changing the terrain"));
                changed |= ui::PropertyFloat(ui::Tr("薄雪の強さ", "Dusting Strength"), &p.dustingIntensity, 0.0f, 1.0f, d.dustingIntensity, ui::Tr("薄雪の被覆マスクに掛ける強さ", "Strength applied to the dusting coverage mask"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("滑落角", "Slip-Off Angle"), &p.slipoffAngle, 0.0f, 89.0f, d.slipoffAngle, ui::Tr("この角度以上では薄雪を付着させない", "No dusting sticks at or above this angle"), ui::Tr("%.1f 度", "%.1f°"));
                changed |= ui::PropertyFloat(ui::Tr("角度の遷移幅", "Angle Falloff"), &p.slipoffFalloff, 0.0f, 89.0f, d.slipoffFalloff, ui::Tr("滑落角に近づくにつれて被覆を弱める範囲", "Range over which coverage weakens toward the slip-off angle"), ui::Tr("%.1f 度", "%.1f°"));
                changed |= ui::PropertyFloat(ui::Tr("曲率の影響", "Curvature Effect"), &p.curvatureInfluence, 0.0f, 1.0f, d.curvatureInfluence, ui::Tr("凹部で薄雪を増やし、凸部で減らす", "Adds dusting in hollows and removes it on bumps"), "%.3f");
                ui::EndPropertyTable();
            }
        }
        if (ImGui::CollapsingHeader(ui::Tr("薄雪の移流###snowCover4", "Dusting Advection###snowCover4"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ui::BeginPropertyTable("snowCover4")) {
                changed |= ui::PropertyBool(ui::Tr("薄雪を流す", "Advect Dusting"), &p.erodeDusting, d.erodeDusting, ui::Tr("降雪マスクを地形に沿って移流してから付着を判定する", "Advects the snowfall mask along the terrain before testing where it sticks"));
                changed |= ui::PropertyFloat(ui::Tr("流路の長さ", "Flow Length"), &p.advectionLength, 0.0f, 1000.0f, d.advectionLength, ui::Tr("薄雪マスクを運ぶ粒子の移動距離", "Travel distance of the particles that carry the dusting mask"), "%.2f m");
                changed |= ui::PropertyFloat(ui::Tr("流路の分散", "Flow Spread"), &p.advectionVolume, 0.0f, 10.0f, d.advectionVolume, ui::Tr("流れの集中した場所から粒子を逸らす補正", "Steers particles away from where flow concentrates"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("移流の強さ", "Advection Strength"), &p.advectionStrength, 0.0f, 1.0f, d.advectionStrength, ui::Tr("粒子が通った場所へ運んだ値を混ぜる割合", "How much carried values blend into the places particles pass"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("値の保持率", "Value Retention"), &p.valuePreservation, 0.0f, 1.0f, d.valuePreservation, ui::Tr("粒子が出発地点のマスク値を保持する割合", "How much of the starting mask value a particle keeps"), "%.3f");
                ui::EndPropertyTable();
            }
        }
        if (ImGui::CollapsingHeader(ui::Tr("薄雪のノイズ###snowCover5", "Dusting Noise###snowCover5"), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ui::BeginPropertyTable("snowCover5")) {
                changed |= ui::PropertyBool(ui::Tr("ノイズ", "Noise"), &p.noise, d.noise, ui::Tr("薄雪の付着判定をノイズで崩す", "Breaks up the dusting test with noise"));
                changed |= ui::PropertyFloat(ui::Tr("ノイズの強さ", "Noise Strength"), &p.noiseStrength, 0.0f, 4.0f, d.noiseStrength, ui::Tr("傾斜に掛けるノイズの振幅", "Amplitude of the noise applied to the slope"), "%.3f");
                changed |= ui::PropertyFloat(ui::Tr("ノイズの大きさ", "Noise Scale"), &p.noiseScale, 0.01f, 1000.0f, d.noiseScale, ui::Tr("薄雪の模様の基準サイズ", "Base size of the dusting pattern"), "%.2f m");
                changed |= ui::PropertyFloat(ui::Tr("ノイズの粗さ", "Noise Roughness"), &p.noiseRoughness, 0.0f, 1.0f, d.noiseRoughness, ui::Tr("細かいオクターブの振幅比", "Amplitude ratio of the finer octaves"), "%.3f");
                changed |= ui::PropertyInt(ui::Tr("オクターブ", "Octaves"), &p.noiseOctaves, 0, 16, d.noiseOctaves, ui::Tr("基準ノイズに重ねる細かいノイズの段数", "Number of finer noise layers added to the base noise"));
                ui::EndPropertyTable();
            }
        }
        if (p.snowLine && ImGui::CollapsingHeader(ui::Tr("降雪線のカーブ###snowLineCurve", "Snow Line Curve###snowLineCurve"))) {
            if (ui::BeginPropertyTable("snowCoverRampCount")) {
                changed |= ui::PropertyInt(ui::Tr("制御点数", "Control Points"), &p.rampCount, 2, 8, d.rampCount, ui::Tr("位置と値で降雪量の遷移を設定する", "Sets the snowfall transition by position and value"));
                ui::EndPropertyTable();
            }
            static const ui::LocalizedText modes[] = {{"一定", "Constant"}, {"線形", "Linear"}, {"滑らか", "Smooth"}};
            for (int i=0; i<std::clamp(p.rampCount,2,8); ++i) {
                ImGui::PushID(i);
                if (ui::BeginPropertyTable("snowCoverRamp")) {
                    changed |= ui::PropertyFloat(ui::Tr("位置", "Position"), &p.ramp[i].position, 0, 1, d.ramp[i].position, ui::Tr("降雪線の下端が 0、上端が 1", "0 at the bottom of the snow line, 1 at the top"));
                    changed |= ui::PropertyFloat(ui::Tr("値", "Value"), &p.ramp[i].value, 0, 1, d.ramp[i].value, ui::Tr("この位置での降雪量", "Snowfall at this position"));
                    changed |= ui::PropertyCombo(ui::Tr("補間", "Interpolation"), &p.ramp[i].interpolation, ui::Tr(modes).data(), 3, d.ramp[i].interpolation, ui::Tr("次の制御点への繋ぎ方", "How to connect to the next control point"));
                    ui::EndPropertyTable();
                }
                ImGui::PopID();
            }
        }
        return changed;
    }

    if (layer.kind == compositor::LayerKind::Snow) {
        const compositor::MaterialLayer::SnowSettings snowDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("snowBasicRows")) {
            char snowName[128] = {};
            std::snprintf(snowName, sizeof(snowName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), snowName, sizeof(snowName))) {
                layer.name = snowName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("積雪", "Snow"));
        if (ui::BeginPropertyTable("snowRows")) {
            changed |= ui::PropertyFloat(ui::Tr("積雪量", "Snow Amount"), &layer.snow.emissionMeters, 0.0f, 50.0f,
                                         snowDefaults.emissionMeters,
                                         ui::Tr("地形へ降らせる雪の総量。0 なら入力をそのまま通す。"
                                                "Mask 入力を繋ぐとその明るさに比例して降らせる"
                                                "（繋がなければ全面へ一様）",
                                                "Total snow to drop on the terrain. 0 passes the input through. "
                                                "With the Mask input connected, it falls in proportion to its brightness "
                                                "(uniform everywhere otherwise)"),
                                         "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("供給時間", "Emission Time"), &layer.snow.emissionTime, 0.0f, 1.0f, snowDefaults.emissionTime,
                ui::Tr("0 なら最初に全量を置いてから流す。上げるほど段に分けて降らせるので、"
                       "溜まる所がはっきりする",
                       "0 places everything first and then lets it flow. Higher drops it in stages, "
                       "so where it collects becomes clearer"),
                "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("安息角", "Angle of Repose"), &layer.snow.motionSlopeDegrees, 0.0f, 89.0f,
                snowDefaults.motionSlopeDegrees,
                ui::Tr("雪面がこれ以下の傾斜なら雪は動かない（地形ではなく雪面の角度）。"
                       "下げるほど急な所に残らなくなる",
                       "Snow does not move where the snow surface is at or below this slope (the snow surface angle, not the terrain). "
                       "Lower leaves less snow on steep spots"),
                ui::Tr("%.1f 度", "%.1f°"));
            changed |= ui::PropertyFloat(
                ui::Tr("流動率", "Transport Rate"), &layer.snow.transportRate, 0.0f, 1.0f, snowDefaults.transportRate,
                ui::Tr("1 回の滑らせで動く割合。高いほど急斜面から早く逃げる",
                       "Fraction moved per slide. Higher clears steep slopes faster"), "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("雪面のならし", "Surface Smoothing"), &layer.snow.surfaceSmoothing, 0.0f, 1.0f,
                snowDefaults.surfaceSmoothing,
                ui::Tr("積もった雪面だけをならす強さ。0 で切る（地形の凹凸がそのまま出る）",
                       "Smooths only the deposited snow surface. 0 turns it off (terrain bumps show through)"),
                "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("最大ディテール", "Max Detail"), &layer.snow.detailMeters, 1.0f, 512.0f,
                snowDefaults.detailMeters,
                ui::Tr("雪が移動先を探す最大の距離。大きいほど広い斜面の下まで流れるが重い。"
                       "雪面をならす半径にも使う",
                       "Farthest distance snow searches for a destination. Larger flows down wider slopes but is heavier. "
                       "Also used as the smoothing radius of the snow surface"),
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyInt(ui::Tr("反復", "Iterations"), &layer.snow.iterations, 1, 256,
                                       snowDefaults.iterations,
                                       ui::Tr("シミュレーションの段数。多いほど雪が落ち着く",
                                              "Number of simulation steps. More lets the snow settle further"));
            changed |= ui::PropertyInt(ui::Tr("安定化", "Stabilization"), &layer.snow.settlingPasses, 1, 16,
                                       snowDefaults.settlingPasses,
                                       ui::Tr("1 段のなかで雪を滑らせる回数", "Slide passes per step"));
            int snowResolutionIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kSnowResolutionValues); ++i) {
                if (kSnowResolutionValues[i] == layer.snow.resolution) {
                    snowResolutionIndex = i;
                }
            }
            if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &snowResolutionIndex, kSnowResolutionLabels,
                                  IM_ARRAYSIZE(kSnowResolutionLabels), 1,
                                  ui::Tr("雪を動かすグリッド。合成解像度とは別。"
                                         "上げるほど細かい雪の筋が出るが重い",
                                         "Grid that moves the snow, separate from the composite resolution. "
                                         "Higher shows finer snow streaks but is heavier"))) {
                layer.snow.resolution = kSnowResolutionValues[snowResolutionIndex];
                changed = true;
            }
            changed |= ui::PropertyFloat(
                ui::Tr("被覆のしきい値", "Coverage Threshold"), &layer.snow.maskThresholdMeters, 0.0f, 5.0f,
                snowDefaults.maskThresholdMeters,
                ui::Tr("Mask 出力がこの積雪厚で白へ寄る", "The Mask output turns white at this snow depth"), "%.3f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("被覆のぼかし", "Coverage Feather"), &layer.snow.maskFeatherMeters, 0.0f, 5.0f,
                snowDefaults.maskFeatherMeters,
                ui::Tr("積雪境界のグレーの幅。0 に近いほど二値に近いマスクになる",
                       "Width of the gray band at the snow edge. Closer to 0 makes the mask closer to binary"), "%.3f m",
                ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("雪を降らせ、急な雪面から低い所へ滑らせて溜める。"
                                  "Mask 入力で降らせる場所を絞れる（Mask Height を繋げば雪線になる。"
                                  "滑った雪はマスクの外へも出る）。"
                                  "Mask 出力は雪の被覆なので、積もった所へ雪のマテリアルを乗せられる",
                                  "Drops snow and lets it slide from steep snow surfaces down to collect in low spots. "
                                  "The Mask input limits where it falls (connect Mask Height for a snow line; "
                                  "sliding snow can leave the mask). "
                                  "The Mask output is the snow coverage, so you can put a snow material where it piled up"));
        return changed;
    }

    // 河川も合成レイヤーではなく「川筋から河床を掘って水を張る加工」。
    // 川の出どころは Mask 入力（Seed）で受けるので、マスクの節は出さない。
    if (layer.kind == compositor::LayerKind::River) {
        const compositor::MaterialLayer::RiverSettings riverDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("riverBasicRows")) {
            char riverName[128] = {};
            std::snprintf(riverName, sizeof(riverName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), riverName, sizeof(riverName))) {
                layer.name = riverName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("川筋", "Channels"));
        if (ui::BeginPropertyTable("riverFlowRows")) {
            changed |= ui::PropertyFloat(
                ui::Tr("川のしきい値", "River Threshold"), &layer.river.threshold, 0.0f, 0.05f, riverDefaults.threshold,
                ui::Tr("川とみなす流量。全セル数に対する割合（Mask Fluvial と同じ単位）。"
                       "下げるほど細い沢まで川になる",
                       "Flow counted as a river, as a fraction of all cells (same unit as Mask Fluvial). "
                       "Lower turns even thin streams into rivers"),
                "%.4f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("最大ディテール", "Max Detail"), &layer.river.detailMeters, 1.0f, 512.0f,
                riverDefaults.detailMeters,
                ui::Tr("流向を読む前にならす大きさ。大きいほど大きな谷筋を追う",
                       "Smoothing size before reading flow directions. Larger follows bigger valleys"), "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("集中度", "Concentration"), &layer.river.concentration, 0.1f, 16.0f, riverDefaults.concentration,
                ui::Tr("下流への配分の集中度。大きいほど主流へ集まる",
                       "How concentrated the downstream distribution is. Larger gathers flow into the main stream"), "%.2f");
            int riverResolutionIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.river.resolution) {
                    riverResolutionIndex = i;
                }
            }
            if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &riverResolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 1,
                                  ui::Tr("川筋を計算するグリッド。合成解像度とは別。"
                                         "反復回数が比例して増えるので、調整中は 256 が軽い",
                                         "Grid used to compute the channels, separate from the composite resolution. "
                                         "Iterations grow in proportion, so 256 is light while tuning"))) {
                layer.river.resolution = kSedimentResolutionValues[riverResolutionIndex];
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("掘る", "Carve"));
        if (ui::BeginPropertyTable("riverCarveRows")) {
            changed |= ui::PropertyFloat(
                ui::Tr("主流の幅", "Main Width"), &layer.river.mainWidthMeters, 1.0f, 1000.0f,
                riverDefaults.mainWidthMeters,
                ui::Tr("流量が最大のセルでの川幅。幅の基準はここで、支流は流量に応じて細くなる",
                       "River width at the cell with the most flow. This sets the width; tributaries narrow with their flow"),
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("最小幅", "Min Width"), &layer.river.minWidthMeters, 0.5f, 100.0f, riverDefaults.minWidthMeters,
                ui::Tr("しきい値ぎりぎりの細い沢でも、これより細くしない",
                       "Even thin streams right at the threshold are not made narrower than this"), "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("幅の伸び", "Width Exponent"), &layer.river.widthExponent, 0.0f, 1.0f, riverDefaults.widthExponent,
                ui::Tr("流量に対する幅の指数。0.5 が水理幾何の標準、0 で一定幅",
                       "Exponent of width versus flow. 0.5 is standard hydraulic geometry; 0 is constant width"), "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("河床の深さ", "Bed Depth"), &layer.river.bedDepthMeters, 0.0f, 50.0f,
                riverDefaults.bedDepthMeters,
                ui::Tr("川の中心での水面から河床までの深さ。水際へ向けて U 字に浅くなる",
                       "Depth from the water surface to the bed at the river center. Shallows in a U shape toward the water's edge"), "%.2f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("岸の幅", "Bank Width"), &layer.river.bankWidthMeters, 0.0f, 100.0f,
                riverDefaults.bankWidthMeters,
                ui::Tr("水際から岸の上端までの距離。岸は水面の高さから立ち上がる"
                       "（掘る形の話。河原の広がりとは別）",
                       "Distance from the water's edge to the top of the bank. The bank rises from the water level "
                       "(this is the carved shape, separate from Shore Width)"),
                "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("岸の硬さ", "Bank Hardness"), &layer.river.bankHardness, 0.0f, 1.0f, riverDefaults.bankHardness,
                ui::Tr("0 でなだらかな土手、1 で切り立った崖", "0 is a gentle embankment, 1 is a sheer cliff"), "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("水面", "Water Surface"));
        if (ui::BeginPropertyTable("riverWaterRows")) {
            changed |= ui::PropertyBool(
                ui::Tr("水を張る", "Fill Water"), &layer.river.fillWater, riverDefaults.fillWater,
                ui::Tr("切ると乾いた河床のまま残す（涸れ川・旧河道）。Water / Depth の Mask は出る",
                       "When off, leaves a dry riverbed (dry or abandoned channel). The Water / Depth masks are still output"));
            changed |= ui::PropertyFloat(
                ui::Tr("最小勾配", "Min Gradient"), &layer.river.minSlope, 0.0f, 0.02f, riverDefaults.minSlope,
                ui::Tr("水面が下流へ下がる最小の傾き（0.001 = 1 km で 1 m）。"
                       "盆地はこの傾きで出口まで埋まって湖になる。"
                       "大きくすると平坦な谷底まで水に浸かる",
                       "Minimum downstream slope of the water surface (0.001 = 1 m per 1 km). "
                       "Basins fill to their outlet at this slope and become lakes. "
                       "Larger floods even flat valley floors"),
                "%.4f", ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("河原", "Shore"));
        if (ui::BeginPropertyTable("riverShoreRows")) {
            changed |= ui::PropertyFloat(
                ui::Tr("河原の広がり", "Shore Width"), &layer.river.shoreWidthMeters, 0.0f, 200.0f,
                riverDefaults.shoreWidthMeters,
                ui::Tr("水際から外側へ、河原（Bank）とみなす距離。主流での値で、"
                       "支流は幅と同じ比で縮む",
                       "Distance outward from the water's edge treated as shore (Bank). This is the main-stream value; "
                       "tributaries shrink in the same ratio as their width"),
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("河原の比高", "Shore Height"), &layer.river.shoreHeightMeters, 0.0f, 20.0f,
                riverDefaults.shoreHeightMeters,
                ui::Tr("水面からこの高さまでを河原とする（増水時に浸かる帯）。"
                       "谷壁を駆け上がらないように高さでも切る",
                       "Up to this height above the water counts as shore (the band flooded at high water). "
                       "Also cut by height so it does not climb the valley walls"),
                "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("河原のぼかし", "Shore Feather"), &layer.river.shoreFeather, 0.0f, 1.0f, riverDefaults.shoreFeather,
                ui::Tr("河原の縁のなだらかさ。広がりと比高それぞれに対する割合",
                       "Softness of the shore edge, as a fraction of both Shore Width and Shore Height"), "%.2f");
            ui::EndPropertyTable();
        }
        changed |= DrawWaterAppearance(layer, kDefaultRiverLayer, false, "riverWaterRows");
        // 流れは合成に焼かず、地形の描画が水面へ下流へ流れる模様を重ねる。
        ui::SectionHeader(ui::Tr("流れ", "Flow"));
        if (ui::BeginPropertyTable("layerRiverFlowRows")) {
            changed |= ui::PropertyFloat(
                ui::Tr("波の強さ", "Wave Strength"), &layer.river.flowWaveStrength, 0.0f, 1.0f, riverDefaults.flowWaveStrength,
                ui::Tr("下流へ流れる波の傾きの強さ。0 で流れの波なし",
                       "Slope strength of the waves flowing downstream. 0 means no flow waves"), "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("波の大きさ", "Wave Size"), &layer.river.flowWaveScaleMeters, 0.5f, 100.0f,
                riverDefaults.flowWaveScaleMeters,
                ui::Tr("一番大きい模様の波長（m）。細かいさざ波はここから段々に小さくして重ねる",
                       "Wavelength (m) of the largest pattern. Finer ripples are layered at progressively smaller sizes"), "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("流れの速さ", "Flow Speed"), &layer.river.flowSpeed, 0.0f, 4.0f, riverDefaults.flowSpeed,
                ui::Tr("速さの倍率。1 で水面の傾きから求めた速さ（急な所ほど速く、川の中央ほど速い）。0 で止める",
                       "Speed multiplier. 1 uses the speed from the water surface slope (faster where steep and toward the center). 0 stops it"),
                "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("白波の量", "Foam"), &layer.river.flowFoam, 0.0f, 1.0f, riverDefaults.flowFoam,
                ui::Tr("早瀬（水面の傾きが急な所）に立つ白波の濃さ。0 で白波なし",
                       "Density of white water on rapids (where the water surface is steep). 0 means no foam"), "%.2f");
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("流れの波は地形の描画で動かす（合成結果や書き出すテクスチャには入らない）。"
                                  "向きは水面の傾きから決まる。River が複数あるときは、最初の有効な 1 つの設定を使う",
                                  "Flow waves are animated by the terrain renderer (not baked into the composite or exported textures). "
                                  "Their direction comes from the water surface slope. With several Rivers, the first enabled one's settings are used"));
        ui::HintText("%s", ui::Tr("川筋から河床を掘り、下流へ単調に下がる水面を張る。盆地は湖になる。"
                                  "Water は水面の被覆、Bank は河原（岩・砂利を置く帯）、Depth は水深。"
                                  "「水を描く」が切のときは、後ろの Surface で水の色を付ける"
                                  "（その Surface はハイトを定数にすること。水面の形は River が決める）。"
                                  "水の上に Surface を重ねると、そこは陸として扱い、流れは消える",
                                  "Carves a riverbed along the channels and fills it with water that steadily descends downstream. Basins become lakes. "
                                  "Water is the water coverage, Bank is the shore (a band for rocks and gravel), Depth is the water depth. "
                                  "With Paint Water off, color the water with a later Surface "
                                  "(set that Surface's height to a constant; River decides the water shape). "
                                  "Where a Surface is layered over the water, it is treated as land and the flow disappears"));
        return changed;
    }

    // 散布は合成レイヤーではなく「形をばら撒く加工」。
    // マスク入力は**散布する範囲**（明るい所ほど置かれやすい）。
    if (layer.kind == compositor::LayerKind::Scatter) {
        const compositor::MaterialLayer::ScatterSettings scatterDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("scatterBasicRows")) {
            const compositor::PlacementPointSet* points = nullptr;
            const PlacementPointsState pointsState = PlacementPointsOf(m_selectedGraphNode, &points);
            if (!layer.enabled) ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%u", 0u);
            else if (pointsState == PlacementPointsState::Missing) ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%s", ui::Tr("未評価", "Not evaluated"));
            else if (pointsState == PlacementPointsState::Evaluating || (points && !points->countReady))
                ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%s", ui::Tr("計算中", "Computing"));
            else ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%u", points ? points->activeCount : 0u);
            char scatterName[128] = {};
            std::snprintf(scatterName, sizeof(scatterName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), scatterName, sizeof(scatterName))) {
                layer.name = scatterName;
                changed = true;
            }

            static const ui::LocalizedText kShapeLabels[] = {{"半球", "Hemisphere"}, {"円錐", "Cone"}};
            int shape = static_cast<int>(layer.scatter.shape);
            if (ui::PropertyCombo(ui::Tr("形", "Shape"), &shape, ui::Tr(kShapeLabels).data(), IM_ARRAYSIZE(kShapeLabels),
                                  static_cast<int>(scatterDefaults.shape),
                                  ui::Tr("半球は丸い低木や樹冠、円錐は尖った草や針葉樹の目安",
                                         "Hemisphere suits round shrubs and canopies; Cone suits pointed grass and conifers"))) {
                layer.scatter.shape = static_cast<compositor::ScatterShape>(shape);
                changed = true;
            }

            static const ui::LocalizedText kOrientationLabels[] = {
                {"平ら", "Flat"}, {"地面に沿う", "Follow Ground"}, {"斜面向き", "Face Slope"}};
            int orientation = static_cast<int>(layer.scatter.orientation);
            if (ui::PropertyCombo(ui::Tr("向き", "Direction"), &orientation, ui::Tr(kOrientationLabels).data(),
                                  IM_ARRAYSIZE(kOrientationLabels),
                                  static_cast<int>(scatterDefaults.orientation),
                                  ui::Tr("地形の傾きの扱い。地面に沿うは斜面で低く潰れ、"
                                         "斜面向きは個体の向きを傾きの向きへ寄せる",
                                         "How terrain slope is handled. Follow Ground flattens on slopes; "
                                         "Face Slope turns each instance toward the slope direction"))) {
                layer.scatter.orientation =
                    static_cast<compositor::ScatterOrientation>(orientation);
                changed = true;
            }

            changed |= ui::PropertyFloat(ui::Tr("間隔", "Spacing"), &layer.scatter.densityMeters, 0.5f, 200.0f,
                                         scatterDefaults.densityMeters,
                                         ui::Tr("散布点の間隔（m）。小さいほど密に置かれる",
                                                "Spacing (m) between scatter points. Smaller places them more densely"), "%.1f m",
                                         ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("被覆", "Coverage"), &layer.scatter.coverage, 0.0f, 1.0f,
                                         scatterDefaults.coverage,
                                         ui::Tr("散布点に実際に置く確率。Mask 入力があれば掛かる",
                                                "Probability that a scatter point is actually placed. Multiplied by the Mask input if connected"),
                                         "%.2f");
            changed |= ui::PropertyInt(ui::Tr("シード", "Seed"), &layer.scatter.seed, 0, 9999,
                                       scatterDefaults.seed, ui::Tr("配置と個体差の種", "Seed for placement and per-instance variation"));
            ui::EndPropertyTable();
        }

        ui::SectionHeader(ui::Tr("個体", "Instances"));
        if (ui::BeginPropertyTable("scatterInstanceRows")) {
            const char* kDiameterTooltip =
                ui::Tr("点 1 つの直径（m）。最小〜最大の間で点ごとに決まる。地面に作る形と Mask 出力の大きさになる"
                       "（点の間隔ではない）。Model Scatter の「点の直径に合わせる」がオンなら、モデルもこの直径になる",
                       "Diameter (m) of each point, chosen per point between min and max. Sets the size of the ground shape and the Mask output "
                       "(not the point spacing). With Model Scatter's Fit Point Diameter on, models use this diameter too");
            changed |= ui::PropertyFloat(ui::Tr("最小の直径", "Min Diameter"), &layer.scatter.sizeMinMeters, 0.1f, 200.0f,
                                         scatterDefaults.sizeMinMeters, kDiameterTooltip,
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("最大の直径", "Max Diameter"), &layer.scatter.sizeMaxMeters, 0.1f, 200.0f,
                                         scatterDefaults.sizeMaxMeters, kDiameterTooltip,
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            // Scale 入力のマスクがモデルを何倍まで縮めるかは、点を受ける Model Scatter が決める。
            // こちらにも出して、どこで決まっているかを辿れるようにする。
            if (const graph::Node* self = m_graph.FindNode(m_selectedGraphNode);
                self != nullptr && self->inputs.size() > 3) {
                if (m_graph.FindUpstreamNodeForPin(self->inputs[3].id) == nullptr) {
                    ui::PropertyValue(ui::Tr("Scale マスク", "Scale Mask"), "%s", ui::Tr("未接続（モデルは等倍）", "Not connected (models at 1x)"));
                } else {
                    std::string text;
                    for (const graph::Node& other : m_graph.Nodes()) {
                        const auto* models = std::get_if<graph::ModelScatterSettings>(&other.settings);
                        if (models == nullptr || other.inputs.empty() ||
                            m_graph.FindUpstreamNodeForPin(other.inputs[0].id) != self) continue;
                        char line[96];
                        std::snprintf(line, sizeof(line), ui::Tr("%s黒 %.2f 倍 → 白 等倍", "%sblack %.2fx → white 1x"), text.empty() ? "" : " / ",
                                      models->sizeAtZero);
                        text += line;
                    }
                    ui::PropertyValue(ui::Tr("Scale マスク", "Scale Mask"), "%s", text.empty() ? ui::Tr("接続中（Model Scatter が無い）", "Connected (no Model Scatter)") : text.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s", ui::Tr("Scale 入力のマスクで、置くモデルの倍率が変わる（点の直径と間隔は変わらない）。\n"
                                                       "黒の所の倍率は、Points を繋いだ Model Scatter の「Scale マスクの最小倍率」で決める",
                                                       "The Scale input mask changes the scale of placed models (point diameter and spacing stay the same).\n"
                                                       "The scale at black is set by Scale Mask Min on the Model Scatter connected to Points"));
                }
            }
            changed |= ui::PropertyFloat(ui::Tr("高さ", "Height"), &layer.scatter.heightMeters, 0.0f, 100.0f,
                                         scatterDefaults.heightMeters,
                                         ui::Tr("地形に盛り上げる高さ（m）。0 でも Mask は出る",
                                                "Height (m) raised on the terrain. The Mask is output even at 0"),
                                         "%.2f m");
            changed |= ui::PropertyFloat(ui::Tr("高さのばらつき", "Height Jitter"), &layer.scatter.heightJitter, 0.0f, 1.0f,
                                         scatterDefaults.heightJitter, ui::Tr("個体ごとの高さの差", "Height variation per instance"),
                                         "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("向きのばらつき", "Rotation Jitter"), &layer.scatter.rotationVariation, 0.0f,
                                         1.0f, scatterDefaults.rotationVariation,
                                         ui::Tr("個体ごとの回転の差", "Rotation variation per instance"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("細長さ", "Elongation"), &layer.scatter.aspectVariation, 0.0f, 1.0f,
                                         scatterDefaults.aspectVariation,
                                         ui::Tr("個体ごとの縦横比の差。0 で真円",
                                                "Aspect ratio variation per instance. 0 is a perfect circle"), "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("なめらかさ", "Smoothness"), &layer.scatter.smoothness, 0.0f, 1.0f, scatterDefaults.smoothness,
                ui::Tr("重なった所の溶け方。0 で折り目が立ち、上げるほど溶け合う",
                       "How overlaps blend. 0 leaves sharp creases; higher blends them together"), "%.2f");
            ui::EndPropertyTable();
        }

        // 群生。親の点（格子の 1 マスに 1 つ）の周りに子の点を置く。Points 出力だけに効く。
        ui::SectionHeader(ui::Tr("群生", "Clusters"));
        if (ui::BeginPropertyTable("scatterClusterRows", ui::Tr("子の大きさの下限", "Min Child Size"))) {
            changed |= ui::PropertyInt(ui::Tr("子の数", "Child Count"), &layer.scatter.clusterCount, 0, 8, scatterDefaults.clusterCount,
                                       ui::Tr("点 1 つ（親）の周りに置く子の点の数の上限。0 で群生なし。"
                                              "子は 4 つに 1 つ欠けるので、株ごとに本数が変わる。"
                                              "数本が寄り添う株立ちや、小さな木立ちになる。点が増えるぶん、間隔を広げて使う",
                                              "Max number of child points around each (parent) point. 0 means no clusters. "
                                              "About 1 in 4 children is dropped, so the count varies per clump. "
                                              "Makes multi-stem clumps or small groves. Widen the spacing to offset the extra points"));
            if (layer.scatter.clusterCount > 0) {
                changed |= ui::PropertyFloat(ui::Tr("広がり", "Spread"), &layer.scatter.clusterRadiusMeters, 0.1f, 200.0f,
                                             scatterDefaults.clusterRadiusMeters,
                                             ui::Tr("親から子までの距離の上限（m）。子はこの 3 割〜10 割の距離に置かれる。"
                                                    "間隔より小さくすると塊と隙間がはっきりする",
                                                    "Max distance (m) from parent to child. Children are placed at 0.3-1.0 times this distance. "
                                                    "Smaller than the spacing gives distinct clumps and gaps"),
                                             "%.1f m", ImGuiSliderFlags_Logarithmic);
                changed |= ui::PropertyFloat(ui::Tr("子の大きさの下限", "Min Child Size"), &layer.scatter.clusterScale, 0.05f, 1.0f,
                                             scatterDefaults.clusterScale,
                                             ui::Tr("子の大きさ（親に対する比）の下限。子はこの値〜1 の間で決まる。"
                                                    "Model Scatter のモデルの倍率と、点の直径の両方に掛かる",
                                                    "Lower limit of child size (relative to the parent). Children range from this value to 1. "
                                                    "Applies to both the Model Scatter model scale and the point diameter"),
                                             "%.2f");
            }
            ui::EndPropertyTable();
        }
        if (layer.scatter.clusterCount > 0)
            ui::HintText("%s", ui::Tr("群生は Points 出力だけに効く。地面に作る形と Mask 出力は親の点だけ",
                                      "Clusters only affect the Points output. The ground shape and Mask output use parent points only"));
        ui::HintText("%s", ui::Tr("Mask 入力で散布する範囲を絞れる（明るい所ほど置かれる）",
                                  "The Mask input limits the scatter area (brighter areas get more)"));
        ui::HintText("%s", ui::Tr("Mask 出力は分布、Unique は個体ごとの乱数。"
                                  "高さ 0 のまま Mask だけを使ってもよい",
                                  "The Mask output is the distribution; Unique is a random value per instance. "
                                  "You can leave Height at 0 and use only the Mask"));
        return changed;
    }

    // 水滴侵食も合成レイヤーではなく「水滴で削って運んで積む加工」。マスク入力は持たない。
    if (layer.kind == compositor::LayerKind::FluvialErosion) {
        auto& params = layer.fluvialErosion;
        const compositor::MaterialLayer::FluvialErosionSettings erosionDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("fluvialErosionRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) { layer.name = name; changed = true; }
            const char* labels[] = {"64", "128", "256", "512", "1024", "2048"};
            int selected = 0, defaultIndex = 0;
            for (int i = 0; i < 6; ++i) {
                if ((64u << i) == params.resolution) selected = i;
                if ((64u << i) == erosionDefaults.resolution) defaultIndex = i;
            }
            if (ui::PropertyCombo(ui::Tr("計算解像度", "Resolution"), &selected, labels, 6, defaultIndex, ui::Tr("侵食を計算する格子。合成解像度とは独立", "Grid used to compute erosion, independent of the composite resolution"))) {
                params.resolution = 64u << selected; changed = true;
            }
            changed |= ui::PropertyInt(ui::Tr("反復回数", "Iterations"), &params.iterations, 0, 100, erosionDefaults.iterations);
            changed |= ui::PropertyFloat(ui::Tr("地形の特徴サイズ", "Feature Size"), &params.featureSize, 0.0f, 32.0f, erosionDefaults.featureSize);
            changed |= ui::PropertyFloat(ui::Tr("地質年代", "Geological Age"), &params.geologicalAge, 0.0f, 20.0f, erosionDefaults.geologicalAge);
            changed |= ui::PropertyFloat(ui::Tr("流路の長さ", "Channel Length"), &params.channelLength, 0.0f, 512.0f, erosionDefaults.channelLength);
            changed |= ui::PropertyFloat(ui::Tr("侵食の強さ", "Erosion Strength"), &params.strength, 0.0f, 1.0f, erosionDefaults.strength);
            changed |= ui::PropertyFloat(ui::Tr("流路の掘り込み", "Channeling"), &params.channeling, 0.0f, 1.0f, erosionDefaults.channeling);
            changed |= ui::PropertyFloat(ui::Tr("摩擦", "Friction"), &params.friction, 0.0f, 1.0f, erosionDefaults.friction);
            changed |= ui::PropertyFloat(ui::Tr("侵食開始角度", "Wear Angle"), &params.wearAngle, 0.0f, 89.0f, erosionDefaults.wearAngle);
            changed |= ui::PropertyFloat(ui::Tr("堆積停止角度", "Deposit Angle"), &params.depositAngle, 0.0f, 89.0f, erosionDefaults.depositAngle);
            changed |= ui::PropertyFloat(ui::Tr("侵食上限角度", "Max Angle"), &params.maxAngle, 0.0f, 89.0f, erosionDefaults.maxAngle);
            changed |= ui::PropertyFloat(ui::Tr("粒度", "Granularity"), &params.granularity, 0.0f, 100.0f, erosionDefaults.granularity);
            changed |= ui::PropertyFloat(ui::Tr("侵食履歴の影響", "Erosion History"), &params.flowVolume, 0.0f, 1.0f, erosionDefaults.flowVolume);
            changed |= ui::PropertyFloat(ui::Tr("細い流路の影響", "Small Channels"), &params.smallChannels, 0.0f, 1.0f, erosionDefaults.smallChannels);
            changed |= ui::PropertyFloat(ui::Tr("移動速度", "Velocity"), &params.velocity, 0.0f, 2.0f, erosionDefaults.velocity);
            changed |= ui::PropertyFloat(ui::Tr("基準のセル幅", "Ref. Cell Size"), &params.detailMeters, 0.1f, 32.0f, erosionDefaults.detailMeters);
            changed |= ui::PropertyFloat(ui::Tr("侵食部分の平滑化", "Detail Smoothing"), &params.detailSmoothing, 0.0f, 10.0f, erosionDefaults.detailSmoothing);
            changed |= ui::PropertyFloat(ui::Tr("外力 X", "Force X"), &params.forceX, -1.0f, 1.0f, erosionDefaults.forceX);
            changed |= ui::PropertyFloat(ui::Tr("外力 Z", "Force Z"), &params.forceZ, -1.0f, 1.0f, erosionDefaults.forceZ);
            changed |= ui::PropertyFloat(ui::Tr("せん断 X", "Shear X"), &params.shearX, -0.1f, 0.1f, erosionDefaults.shearX);
            changed |= ui::PropertyFloat(ui::Tr("せん断 Z", "Shear Z"), &params.shearZ, -0.1f, 0.1f, erosionDefaults.shearZ);
            changed |= ui::PropertyFloat(ui::Tr("硬度", "Hardness"), &params.hardness, 0.0f, 1.0f, erosionDefaults.hardness, ui::Tr("Hardness 入力と合わせて使う。1 で地形を保護する", "Used together with the Hardness input. 1 protects the terrain"));
        ui::EndPropertyTable();
        }
        return changed;
    }

    if (layer.kind == compositor::LayerKind::FlattenBorders) {
        auto& params = layer.flattenBorders;
        const compositor::MaterialLayer::FlattenBordersSettings erosionDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("flattenBordersRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) { layer.name = name; changed = true; }
            changed |= ui::PropertyFloat(ui::Tr("外周の幅", "Border Width"), &params.falloffMeters, 0.0f, 2048.0f, erosionDefaults.falloffMeters);
            changed |= ui::PropertyFloat(ui::Tr("外周の標高", "Border Elevation"), &params.elevationMeters, -1000.0f, 4000.0f, erosionDefaults.elevationMeters);
            changed |= ui::PropertyFloat(ui::Tr("地形の持ち上げ", "Terrain Lift"), &params.liftMeters, -1000.0f, 4000.0f, erosionDefaults.liftMeters);
            changed |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &params.strength, 0.0f, 1.0f, erosionDefaults.strength);
            changed |= ui::PropertyBool(ui::Tr("X 下側", "Min X Edge"), &params.lowerX, erosionDefaults.lowerX);
            changed |= ui::PropertyBool(ui::Tr("X 上側", "Max X Edge"), &params.upperX, erosionDefaults.upperX);
            changed |= ui::PropertyBool(ui::Tr("Z 下側", "Min Z Edge"), &params.lowerZ, erosionDefaults.lowerZ);
            changed |= ui::PropertyBool(ui::Tr("Z 上側", "Max Z Edge"), &params.upperZ, erosionDefaults.upperZ);
        ui::EndPropertyTable();
        }
        return changed;
    }

    // 均し（Grading）は、道路や敷地のメッシュに合わせて地形を切土・盛土の形へ変える加工。
    if (layer.kind == compositor::LayerKind::RoadGrading) {
        auto& params = layer.roadGrading;
        const compositor::MaterialLayer::RoadGradingSettings d;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("roadGradingBasicRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) { layer.name = name; changed = true; }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("メッシュの下", "Under Mesh"));
        if (ui::BeginPropertyTable("roadGradingRoadRows", ui::Tr("路面下の余裕", "Flat Width"))) {
            changed |= ui::PropertyFloat(ui::Tr("路面下の余裕", "Clearance"), &params.clearanceMeters, 0.0f, 2.0f, d.clearanceMeters,
                                         ui::Tr("路面や敷地の下の地形を、その面からどれだけ下げるか（m）。"
                                                "地形が路面を突き抜けないための余裕",
                                                "How far (m) the terrain under the road or pad is lowered below that surface. "
                                                "Margin that keeps the terrain from poking through the road"),
                                         "%.2f m");
            changed |= ui::PropertyFloat(ui::Tr("平らな幅", "Flat Width"), &params.vergeMeters, 0.0f, 20.0f, d.vergeMeters,
                                         ui::Tr("道路や敷地の端から、法面が始まるまでの平らな幅（m）",
                                                "Flat width (m) from the edge of the road or pad to where the embankment starts"), "%.1f m");
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("法面", "Embankment"));
        if (ui::BeginPropertyTable("roadGradingSlopeRows", ui::Tr("法面の長さの上限", "Max Slope Length"))) {
            changed |= ui::PropertyFloat(ui::Tr("切土の勾配", "Cut Slope"), &params.cutRatio, 0.1f, 5.0f, d.cutRatio,
                                         ui::Tr("地形が道路より高い側の法面。1 : n（高さ 1 に対して水平 n）。"
                                                "小さいほど急（岩は 0.3〜0.5、土は 1.0〜1.5 が目安）",
                                                "Embankment on the side where the terrain is above the road. 1 : n (n horizontal per 1 vertical). "
                                                "Smaller is steeper (about 0.3-0.5 for rock, 1.0-1.5 for soil)"),
                                         "1 : %.2f");
            changed |= ui::PropertyFloat(ui::Tr("盛土の勾配", "Fill Slope"), &params.fillRatio, 0.1f, 5.0f, d.fillRatio,
                                         ui::Tr("地形が道路より低い側の法面。1 : n。切土より緩いのが普通（1.5〜2.0 が目安）",
                                                "Embankment on the side where the terrain is below the road. 1 : n. Usually gentler than cut (about 1.5-2.0)"),
                                         "1 : %.2f");
            changed |= ui::PropertyFloat(ui::Tr("法面の長さの上限", "Max Slope Length"), &params.maxRunMeters, 0.0f, 500.0f, d.maxRunMeters,
                                         ui::Tr("法面の水平の長さの上限（m）。これより遠くは元の地形のまま。0 で無制限",
                                                "Max horizontal length (m) of the embankment. Beyond this the original terrain stays. 0 is unlimited"),
                                         "%.0f m");
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("マスク出力", "Mask Output"));
        if (ui::BeginPropertyTable("roadGradingMaskRows", ui::Tr("法面の長さの上限", "Max Slope Length"))) {
            changed |= ui::PropertyFloat(ui::Tr("ぼかし", "Blur"), &params.maskSoftMeters, 0.01f, 5.0f, d.maskSoftMeters,
                                         ui::Tr("Cut / Fill のマスクが 1 になる、削った / 盛った量（m）",
                                                "Cut / fill amount (m) at which the Cut / Fill masks reach 1"), "%.2f m");
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("Mesh 入力に Road Mesh / Shoulder か、Model Place の Pad（建物の敷地）を繋ぐ。"
                                  "Road は路面や敷地の下（平らな幅まで）、Cut は切土の法面、Fill は盛土の法面。"
                                  "Road Path の Base は、この Grading より手前の地形に繋ぐこと"
                                  "（均した後の地形に繋ぐと、道路が自分で均した地形を読み直して循環する）",
                                  "Connect Road Mesh / Shoulder or a Model Place Pad (building lot) to the Mesh input. "
                                  "Road is under the road or pad (out to Flat Width), Cut is the cut embankment, Fill is the fill embankment. "
                                  "Connect Road Path's Base to terrain before this Grading "
                                  "(connecting it to the graded terrain makes the road re-read its own grading and loop)"));
        return changed;
    }

    if (layer.kind == compositor::LayerKind::HeightLevels) {
        auto& params = layer.heightLevels;
        const compositor::MaterialLayer::HeightLevelsSettings levelsDefaults;
        // m の行の上限は地形の標高差（ハイト 0〜1 の全幅）。プレビューの変位量がそれに従う。
        const float heightMeters = std::max(1.0f, m_renderer.DisplacementScale());
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("heightLevelsBasicRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) { layer.name = name; changed = true; }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("入力の範囲", "Input Range"));
        if (ui::BeginPropertyTable("heightLevelsInputRows")) {
            changed |= ui::PropertyBool(ui::Tr("自動", "Auto"), &params.autoInput, levelsDefaults.autoInput,
                                        ui::Tr("今の地形の最低〜最高を入力の範囲にする（評価のたびに集計する）",
                                               "Uses the current terrain's min to max as the input range (measured on every evaluation)"));
            if (!params.autoInput) {
                changed |= ui::PropertyFloat(ui::Tr("最低", "Min"), &params.inputMinMeters, 0.0f, heightMeters,
                                             levelsDefaults.inputMinMeters,
                                             ui::Tr("地形の底からの高さ（m）。これより低い所は出力の最低になる",
                                                    "Height (m) above the terrain bottom. Anything lower maps to the output min"), "%.1f m");
                changed |= ui::PropertyFloat(ui::Tr("最高", "Max"), &params.inputMaxMeters, 0.0f, heightMeters,
                                             std::min(levelsDefaults.inputMaxMeters, heightMeters),
                                             ui::Tr("地形の底からの高さ（m）。これより高い所は出力の最高になる",
                                                    "Height (m) above the terrain bottom. Anything higher maps to the output max"), "%.1f m");
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("出力の範囲", "Output Range"));
        if (ui::BeginPropertyTable("heightLevelsOutputRows")) {
            changed |= ui::PropertyBool(ui::Tr("全幅", "Full Range"), &params.fullOutput, levelsDefaults.fullOutput,
                                        ui::Tr("出力を 0〜標高差（地形の全幅）にする。侵食で縮んだ範囲を元に戻すときはこのまま",
                                               "Outputs 0 to the elevation range (the full terrain range). Leave on to restore a range shrunk by erosion"));
            if (!params.fullOutput) {
                changed |= ui::PropertyFloat(ui::Tr("最低", "Min"), &params.outputMinMeters, 0.0f, heightMeters,
                                             levelsDefaults.outputMinMeters, ui::Tr("地形の底からの高さ（m）", "Height (m) above the terrain bottom"), "%.1f m");
                changed |= ui::PropertyFloat(ui::Tr("最高", "Max"), &params.outputMaxMeters, 0.0f, heightMeters,
                                             std::min(levelsDefaults.outputMaxMeters, heightMeters),
                                             ui::Tr("地形の底からの高さ（m）", "Height (m) above the terrain bottom"), "%.1f m");
            }
            changed |= ui::PropertyFloat(ui::Tr("ガンマ", "Gamma"), &params.gamma, 0.1f, 10.0f, levelsDefaults.gamma,
                                         ui::Tr("1 で直線。1 より大きいと中間の高さが下がり、小さいと上がる",
                                                "1 is linear. Above 1 lowers the middle heights; below 1 raises them"), "%.2f",
                                         ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("Mask の白い所だけに効く（黒い所は元の高さのまま）",
                                  "Affects only the white areas of the Mask (black areas keep their original height)"));
        return changed;
    }

    if (layer.kind == compositor::LayerKind::MultiScaleErosion) {
        auto& params = layer.multiScaleErosion;
        const compositor::MaterialLayer::MultiScaleErosionSettings mseDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("multiScaleBasicRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), name, sizeof(name))) {
                layer.name = name;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("段階と侵食量", "Stages & Erosion Amount"));
        if (ui::BeginPropertyTable("multiScaleErosionRows")) {
            const uint32_t resolutions[] = {64, 128, 256, 512, 1024, 2048};
            const char* labels[] = {"64", "128", "256", "512", "1024", "2048"};
            const auto resolutionRow = [&](const char* label, uint32_t& value, uint32_t defaultValue) {
                int selected = 0, defaultIndex = 0;
                for (int i = 0; i < IM_ARRAYSIZE(resolutions); ++i) {
                    if (resolutions[i] == value) selected = i;
                    if (resolutions[i] == defaultValue) defaultIndex = i;
                }
                if (ui::PropertyCombo(label, &selected, labels, IM_ARRAYSIZE(labels), defaultIndex,
                                      ui::Tr("粗い解像度から倍々に上げて計算する。合成解像度とは独立",
                                             "Computes from a coarse resolution, doubling each stage. Independent of the composite resolution"))) {
                    value = resolutions[selected];
                    changed = true;
                }
            };
            resolutionRow(ui::Tr("開始解像度", "Start Resolution"), params.baseResolution, mseDefaults.baseResolution);
            resolutionRow(ui::Tr("最終解像度", "Final Resolution"), params.resolution, mseDefaults.resolution);
            params.baseResolution = std::min(params.baseResolution, params.resolution);
            changed |= ui::PropertyFloat(ui::Tr("最大侵食深さ", "Max Erosion Depth"), &params.coarseDepthMeters, 0.0f, 100.0f,
                mseDefaults.coarseDepthMeters, ui::Tr("最も粗い段階で河川侵食が削れる深さの上限。崩落と堆積は別",
                                                      "Max depth fluvial erosion can cut at the coarsest stage. Crumbling and deposition are separate"), "%.2f m");
            changed |= ui::PropertyFloat(ui::Tr("細部の強さ", "Detail Decay"), &params.detailDecay, 0.0f, 1.0f,
                mseDefaults.detailDecay, ui::Tr("解像度を倍にするたび、最大侵食深さに掛ける割合",
                                                "Factor applied to Max Erosion Depth each time the resolution doubles"), "%.2f");
            changed |= ui::PropertyInt(ui::Tr("侵食の反復", "Erosion Iterations"), &params.erosionIterations, 0, 4000,
                mseDefaults.erosionIterations, ui::Tr("各段階の反復数。深さの上限は変えず、水系を育てる。0 で河川侵食なし",
                                                      "Iterations per stage. Grows the drainage network without changing the depth limit. 0 means no fluvial erosion"));
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("水の流れ", "Water Flow"));
        if (ui::BeginPropertyTable("multiScaleFlowRows")) {
            changed |= ui::PropertyFloat(ui::Tr("流れの集中", "Flow Exponent"), &params.flowExponent, 1.0f, 8.0f,
                mseDefaults.flowExponent, ui::Tr("大きいほど急な方向へ集中する。論文の既定は 1.3",
                                                 "Larger concentrates flow toward the steepest direction. The paper's default is 1.3"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("傾斜の指数", "Slope Exponent"), &params.slopeExponent, 0.1f, 4.0f,
                mseDefaults.slopeExponent, ui::Tr("侵食の強さに対する傾斜の効き方", "How slope affects erosion strength"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("集水の指数", "Drainage Exponent"), &params.drainageExponent, 0.1f, 2.0f,
                mseDefaults.drainageExponent, ui::Tr("侵食の強さに対する集水面積の効き方", "How drainage area affects erosion strength"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("傾斜の上限", "Max Slope"), &params.maximumSlope, 0.01f, 10.0f,
                mseDefaults.maximumSlope, ui::Tr("これより急でも河川侵食は強くならない。1 で 45 度",
                                                 "Fluvial erosion gets no stronger beyond this slope. 1 is 45 degrees"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("集水の上限", "Max Drainage Area"), &params.maximumDrainageArea, 1.0f, 1000000.0f,
                mseDefaults.maximumDrainageArea,
                ui::Tr("これより水が集まっても河川侵食は強くならない。開始時のセル面積"
                       "（地形の一辺 ÷ 開始解像度）の二乗より小さいと、尾根と谷の差が出なくなる",
                       "Fluvial erosion gets no stronger beyond this much drainage. Below the starting cell area "
                       "(terrain side / Start Resolution, squared), ridges and valleys stop differing"),
                "%.0f m²",
                ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("斜面と堆積", "Slopes & Deposition"));
        if (ui::BeginPropertyTable("multiScaleSedimentRows")) {
            changed |= ui::PropertyFloat(ui::Tr("安息角", "Angle of Repose"), &params.talusDegrees, 1.0f, 85.0f,
                mseDefaults.talusDegrees, ui::Tr("これより急な隣接斜面から土砂を下へ動かす",
                                                 "Moves sediment down from neighboring slopes steeper than this"), ui::Tr("%.1f 度", "%.1f°"));
            changed |= ui::PropertyFloat(ui::Tr("土砂の移動量", "Thermal Step"), &params.thermalStepMeters, 0.0f, 0.1f,
                mseDefaults.thermalStepMeters, ui::Tr("1 反復で隣接セルへ移す高さ。セル幅の 1% を上限にする",
                                                      "Height moved to a neighboring cell per iteration. Capped at one hundredth of the cell width"), "%.4f m");
            changed |= ui::PropertyInt(ui::Tr("斜面の反復", "Thermal Iterations"), &params.thermalIterations, 0, 1000,
                mseDefaults.thermalIterations, ui::Tr("河川侵食の後に斜面を安定させる回数。0 で無効",
                                                      "Passes that stabilize slopes after fluvial erosion. 0 disables"));
            changed |= ui::PropertyFloat(ui::Tr("土砂の供給", "Sediment Creation"), &params.sedimentCreation, 0.0f, 1.0f,
                mseDefaults.sedimentCreation, ui::Tr("水の侵食力に比例して供給する浮遊土砂の係数",
                                                     "Coefficient for suspended sediment supplied in proportion to the water's erosive power"), "%.3f");
            changed |= ui::PropertyFloat(ui::Tr("堆積率", "Deposition Rate"), &params.depositionRate, 0.0f, 1.0f,
                mseDefaults.depositionRate, ui::Tr("運べる量を超えた浮遊土砂が積もる割合",
                                                   "Fraction of suspended sediment above capacity that settles"), "%.3f");
            changed |= ui::PropertyFloat(ui::Tr("堆積の高さ", "Sediment Height"), &params.sedimentHeightScale, 0.0f, 0.1f,
                mseDefaults.sedimentHeightScale, ui::Tr("堆積する土砂の内部量を高さに換算する倍率。大きいほど厚く積もる",
                                                        "Converts the internal amount of deposited sediment to height. Larger deposits thicker"),
                "%.4f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyInt(ui::Tr("堆積の反復", "Deposit Iterations"), &params.depositionIterations, 0, 2000,
                mseDefaults.depositionIterations, ui::Tr("斜面安定化の後に土砂を流して積む回数。0 で無効",
                                                         "Passes that flow and deposit sediment after slope stabilization. 0 disables"));
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("地形の仕上げ", "Terrain Finishing"));
        if (ui::BeginPropertyTable("multiScaleFinishingRows")) {
            changed |= ui::PropertyFloat(ui::Tr("稜線の高さ復元", "Ridge Restoration"), &params.ridgeRestoration, 0.0f, 1.0f,
                mseDefaults.ridgeRestoration, ui::Tr("尾根・山頂を元の高さへ戻す強さ。0 で無効",
                                                     "How strongly ridges and peaks return to their original height. 0 disables"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("尾根の集水閾値", "Ridge Threshold"), &params.ridgeAreaThreshold, 1.01f, 8.0f,
                mseDefaults.ridgeAreaThreshold, ui::Tr("集水面積がこのセル数未満の点を復元する。大きいほど対象が広がる",
                                                       "Restores points whose drainage area is below this many cells. Larger widens the affected area"),
                ui::Tr("%.2f セル", "%.2f cells"));
            changed |= ui::PropertyInt(ui::Tr("復元の反復", "Restore Iterations"), &params.restorationIterations, 1, 2000,
                mseDefaults.restorationIterations, ui::Tr("尾根の標高差を周囲へ滑らかに広げる回数",
                                                          "Passes that smoothly spread ridge height differences to the surroundings"));
            changed |= ui::PropertyBool(ui::Tr("排水路補正", "Drain Correction"), &params.drainageCorrection,
                mseDefaults.drainageCorrection, ui::Tr("窪地から外周へ排水路を掘る。湖を残したい場合は無効にする",
                                                       "Carves drainage channels from basins to the edge. Turn off to keep lakes"));
            changed |= ui::PropertyInt(ui::Tr("排水路の補正幅", "Breaching Radius"), &params.breachingRadius, 1, 64,
                mseDefaults.breachingRadius, ui::Tr("最終グリッドのセル数。広い幅から半分ずつ狭める半径。1 で細い流路のみ",
                                                    "In final grid cells. Radius that halves from wide to narrow. 1 gives only thin channels"));
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("粗い地形から段階的に谷筋を作る。開始解像度より細かな元の凹凸は再構成される。"
                                  "Mask は結果を適用する範囲（白で適用）。",
                                  "Builds valleys in stages starting from a coarse terrain. Original detail finer than Start Resolution is reconstructed. "
                                  "The Mask is where the result applies (white applies)."));
        return changed;
    }
    if (layer.kind == compositor::LayerKind::Droplet) {
        const compositor::MaterialLayer::DropletSettings dropletDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("dropletBasicRows")) {
            char dropletName[128] = {};
            std::snprintf(dropletName, sizeof(dropletName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), dropletName, sizeof(dropletName))) {
                layer.name = dropletName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("水滴", "Droplets"));
        if (ui::BeginPropertyTable("dropletFlowRows")) {
            changed |= ui::PropertyFloat(
                ui::Tr("密度", "Density"), &layer.droplet.dropletDensity, 0.01f, 4.0f, dropletDefaults.dropletDensity,
                ui::Tr("水滴の数。解析グリッドのセルあたりで持つので、同じ密度なら解像度を変えても"
                       "同じくらいの量になる。上げるほど水系が密になり、重くなる",
                       "Number of droplets, per cell of the simulation grid, so the same density gives about the same amount "
                       "at any resolution. Higher makes a denser drainage network and is heavier"),
                ui::Tr("%.2f /セル", "%.2f /cell"), ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("移動距離", "Travel Distance"), &layer.droplet.travelMeters, 16.0f, 4096.0f,
                dropletDefaults.travelMeters, ui::Tr("1 滴が進む距離。長いほど谷が下流まで繋がる",
                                                     "Distance each droplet travels. Longer connects valleys farther downstream"),
                "%.0f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("慣性", "Inertia"), &layer.droplet.inertia, 0.0f, 0.99f, dropletDefaults.inertia,
                ui::Tr("0 で傾斜どおりに曲がり、1 に近いほど前の向きを保つ（谷が真っ直ぐになる）",
                       "0 turns with the slope; closer to 1 keeps the previous direction (straighter valleys)"),
                "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("蒸発", "Evaporation"), &layer.droplet.evaporationPerMeter, 0.0f, 0.05f,
                dropletDefaults.evaporationPerMeter,
                ui::Tr("1 m 進むごとに失う水の割合。水が減ると運べる量も減り、途中で積み始める",
                       "Fraction of water lost per meter traveled. Less water carries less, so it starts depositing on the way"),
                "%.4f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("重力", "Gravity"), &layer.droplet.gravity, 0.0f, 20.0f,
                                         dropletDefaults.gravity,
                                         ui::Tr("下りでの加速。速いほど多く運べる",
                                                "Acceleration going downhill. Faster droplets carry more"), "%.1f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("削る / 積む", "Erode / Deposit"));
        if (ui::BeginPropertyTable("dropletCarveRows")) {
            changed |= ui::PropertyFloat(
                ui::Tr("侵食", "Erosion"), &layer.droplet.erosionStrength, 0.0f, 1.0f,
                dropletDefaults.erosionStrength, ui::Tr("1 歩で削る割合（容量の不足ぶんに掛ける）",
                                                        "Fraction eroded per step (applied to the unused capacity)"),
                "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("堆積", "Deposition"), &layer.droplet.depositionStrength, 0.0f, 1.0f,
                dropletDefaults.depositionStrength, ui::Tr("容量を超えた土砂を捨てる割合",
                                                           "Fraction of sediment above capacity that is dropped"), "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("容量", "Capacity"), &layer.droplet.sedimentCapacity, 0.1f, 20.0f,
                dropletDefaults.sedimentCapacity,
                ui::Tr("運べる量の係数（傾斜 × 速度 × 水量に掛ける）。大きいほど深く削る",
                       "Carrying capacity factor (multiplies slope x speed x water). Larger erodes deeper"), "%.1f",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                ui::Tr("最小傾斜", "Min Slope"), &layer.droplet.minSlope, 0.0001f, 0.1f, dropletDefaults.minSlope,
                ui::Tr("平坦な所でも運べるようにする傾斜の下限",
                       "Lower slope limit so droplets can carry sediment even on flat ground"), "%.4f",
                ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("計算", "Computation"));
        if (ui::BeginPropertyTable("dropletComputeRows")) {
            int dropletResolutionIndex = 2;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.droplet.resolution) {
                    dropletResolutionIndex = i;
                }
            }
            if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &dropletResolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 2,
                                  ui::Tr("水滴を流すグリッド。合成解像度とは別。結果は合成解像度には"
                                         "依らないが、ここを変えると細かい枝が変わる",
                                         "Grid the droplets run on, separate from the composite resolution. The result does not "
                                         "depend on the composite resolution, but changing this changes the fine branches"))) {
                layer.droplet.resolution = kSedimentResolutionValues[dropletResolutionIndex];
                changed = true;
            }
            changed |= ui::PropertyBool(
                ui::Tr("マルチグリッド", "Multigrid"), &layer.droplet.multigrid, dropletDefaults.multigrid,
                ui::Tr("64² から倍々に上げて流す。粗いレベルが大きな谷を、細かいレベルが枝を決める",
                       "Runs from 64² upward, doubling each level. Coarse levels shape the large valleys, fine levels the branches"));
            changed |= ui::PropertyInt(
                ui::Tr("反復", "Iterations"), &layer.droplet.iterations, 1, 200, dropletDefaults.iterations,
                ui::Tr("水滴を分けて流す回数。掘れた谷を次の反復が見るので、多いほど水系が育つ",
                       "Number of batches of droplets. Each iteration sees the valleys carved before, so more grows the drainage network"));
            changed |= ui::PropertyInt(ui::Tr("シード", "Seed"), &layer.droplet.seed, 0, 1000000,
                                       dropletDefaults.seed, ui::Tr("水滴の落とし方の乱数", "Random seed for dropping droplets"));
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("水滴を落として斜面を下らせ、運べる量より少なければ削り、多ければ積む"
                                  "（terrain-editor の Droplet Erosion）。Flow は水の通った量（谷筋に砂利）、"
                                  "Deposit は積もった量（谷底や扇状地に土）。地形の変更は差分で足すので、"
                                  "素材の凹凸は壊さない",
                                  "Drops droplets that run downhill, eroding when carrying less than capacity and depositing when carrying more "
                                  "(terrain-editor's Droplet Erosion). Flow is how much water passed (gravel in valleys), "
                                  "Deposit is how much settled (soil on valley floors and fans). Terrain changes are added as a difference, "
                                  "so material detail is preserved"));
        return changed;
    }

    // 崩落も合成レイヤーではなく「下地のハイトへ岩屑を積む加工」。
    // 発生源は Mask 入力（Emission）で受けるので、マスクの節は出さない。
    if (layer.kind == compositor::LayerKind::Crumbling) {
        const compositor::MaterialLayer::CrumblingSettings crumblingDefaults;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("crumblingBasicRows")) {
            char crumblingName[128] = {};
            std::snprintf(crumblingName, sizeof(crumblingName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), crumblingName, sizeof(crumblingName))) {
                layer.name = crumblingName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("崩落", "Crumbling"));
        if (ui::BeginPropertyTable("crumblingRows")) {
            const compositor::PlacementPointSet* points = nullptr;
            const PlacementPointsState pointsState = PlacementPointsOf(m_selectedGraphNode, &points);
            if (!layer.enabled) ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%u", 0u);
            else if (pointsState == PlacementPointsState::Missing) ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%s", ui::Tr("未評価", "Not evaluated"));
            else if (pointsState == PlacementPointsState::Evaluating || (points && !points->countReady))
                ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%s", ui::Tr("計算中", "Computing"));
            else ui::PropertyValue(ui::Tr("ポイント数", "Points"), "%u", points ? points->activeCount : 0u);
            changed |= ui::PropertyBool(ui::Tr("重なり回避", "Avoid Overlap"), &layer.crumbling.avoidPointOverlap,
                crumblingDefaults.avoidPointOverlap,
                ui::Tr("Points出力の停止位置を直径に応じて間引きます。ResultやMaskには影響しません。後段のサイズ倍率は含みません。",
                       "Thins out the stopping positions in the Points output based on diameter. Does not affect Result or Mask. Does not account for later size scaling."));
            changed |= ui::PropertyFloat(
                ui::Tr("岩屑の量", "Debris Amount"), &layer.crumbling.amount, 0.0f, 1.0f, crumblingDefaults.amount,
                ui::Tr("生む岩片の数と、盛り上がりの強さに効く",
                       "Affects the number of rock fragments and how much they build up"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("最小の直径", "Min Diameter"), &layer.crumbling.sizeMinMeters, 0.1f,
                                         100.0f, crumblingDefaults.sizeMinMeters,
                                         ui::Tr("岩片の直径の下限", "Lower limit of fragment diameter"), "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("最大の直径", "Max Diameter"), &layer.crumbling.sizeMaxMeters, 0.1f,
                                         100.0f, crumblingDefaults.sizeMaxMeters,
                                         ui::Tr("岩片の直径の上限", "Upper limit of fragment diameter"), "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
            int style = static_cast<int>(layer.crumbling.style);
            if (ui::PropertyCombo(ui::Tr("形", "Shape"), &style, ui::Tr(kRockStyleLabels).data(), IM_ARRAYSIZE(kRockStyleLabels),
                                  static_cast<int>(crumblingDefaults.style),
                                  ui::Tr("岩片の輪郭。丸い / 多面体 / 尖った破片",
                                         "Outline of the fragments: round / faceted / sharp shards"))) {
                layer.crumbling.style = static_cast<compositor::RockStyle>(style);
                changed = true;
            }
            changed |= ui::PropertyInt(ui::Tr("歩数", "Steps"), &layer.crumbling.physicsCount, 0, 512,
                                       crumblingDefaults.physicsCount,
                                       ui::Tr("岩片を下へ進めるステップ数。"
                                              "大きいほど斜面の下まで流れる",
                                              "Number of steps fragments move downhill. "
                                              "Larger lets them travel farther down the slope"));
            changed |= ui::PropertyFloat(
                ui::Tr("重力", "Gravity"), &layer.crumbling.gravity, 0.0f, 1.0f, crumblingDefaults.gravity,
                ui::Tr("低い方へ向かう強さ。高いほど一直線に下る",
                       "How strongly fragments head downhill. Higher makes them go straight down"), "%.2f");
            changed |= ui::PropertyFloat(
                ui::Tr("散らばり", "Scatter"), &layer.crumbling.spread, 0.0f, 1.0f, crumblingDefaults.spread,
                ui::Tr("進行方向から横へ逸れる強さ。上げると筋状の重なりがほぐれる",
                       "How strongly fragments drift sideways. Higher breaks up streaky overlaps"), "%.2f");
            changed |= ui::PropertyInt(ui::Tr("シード", "Seed"), &layer.crumbling.seed, 0, 9999,
                                       crumblingDefaults.seed,
                                       ui::Tr("発生位置とばらつきの種", "Seed for spawn positions and variation"));
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("発生源（Emission 入力）の明るい所から岩片を生み、斜面を下らせて積む。"
                                  "Mask は岩屑の厚み、Unique は岩片ごとの乱数を出す",
                                  "Spawns rock fragments from bright areas of the source (Emission input) and lets them roll downhill and pile up. "
                                  "Mask outputs the debris thickness; Unique outputs a random value per fragment"));
        return changed;
    }

    // ブラーは合成レイヤーではなく「下地のハイトをぼかす加工」。
    // 色もハイトのソースもマスクも持たないので、専用の行だけを出す。
    if (layer.kind == compositor::LayerKind::Blur) {
        const compositor::MaterialLayer& blurDefaults = kDefaultBlurLayer;
        ui::SectionHeader(ui::Tr("基本", "General"));
        if (ui::BeginPropertyTable("blurBasicRows")) {
            char blurName[128] = {};
            std::snprintf(blurName, sizeof(blurName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput(ui::Tr("名前", "Name"), blurName, sizeof(blurName))) {
                layer.name = blurName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("ぼかし", "Blur"));
        if (ui::BeginPropertyTable("blurRows")) {
            // 半径は実寸（m）。合成解像度を変えても効きが変わらない。
            changed |= ui::PropertyFloat(ui::Tr("半径", "Radius"), &layer.blur.radiusMeters, 0.0f, 512.0f,
                                         blurDefaults.blur.radiusMeters,
                                         ui::Tr("ぼかす範囲（m）。大きいほど広くならす",
                                                "Blur range (m). Larger smooths a wider area"), "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &layer.blur.strength, 0.0f, 1.0f,
                                         blurDefaults.blur.strength,
                                         ui::Tr("元の高さとぼかした高さを混ぜる量。1 で完全なぼかし",
                                                "Blend between the original and blurred heights. 1 is a full blur"),
                                         "%.2f");
            changed |= ui::PropertyInt(ui::Tr("反復", "Iterations"), &layer.blur.iterations, 1, 16,
                                       blurDefaults.blur.iterations,
                                       ui::Tr("ぼかしを重ねる回数。多いほど広く均される"
                                              "（実効半径はおよそ 半径 x sqrt(反復)）",
                                              "Number of blur passes. More smooths a wider area "
                                              "(effective radius is about Radius x sqrt(Iterations))"));
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("下地のハイトをぼかし、ぼかした形から法線を作り直す。"
                                  "素材の法線ディテールを載せるなら、このノードより後ろに繋ぐ",
                                  "Blurs the base height and rebuilds the normals from the blurred shape. "
                                  "To add material normal detail, connect it after this node"));
        return changed;
    }

    ui::SectionHeader(ui::Tr("基本", "General"));
    if (ui::BeginPropertyTable("layerBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", layer.name.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), nameBuffer, sizeof(nameBuffer))) {
            layer.name = nameBuffer;
            // 名前もアンドゥの対象。落とすと、次のアンドゥで改名まで巻き戻る。
            changed = true;
        }
        // マテリアルを割り当てているときは、見た目はマテリアル側の値で決まる。
        // 同じ意味の値を 2 か所に置くと、どちらが効いているのか分からなくなる。
        // シェイプは色を書かないので、色とサーフェスの行そのものを出さない。
        const bool hasMaterial = (layer.material != compositor::kNoMaterialAsset);
        if (!isShape && !hasMaterial) {
            changed |= ui::PropertyColorLinear(ui::Tr("ベースカラー", "Base Color"), &layer.baseColor.x,
                                               &defaults.baseColor.x);
            changed |= ui::PropertyFloat(ui::Tr("ラフネス", "Roughness"), &layer.roughness, 0.0f, 1.0f,
                                         defaults.roughness, nullptr, "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("メタルネス", "Metalness"), &layer.metallic, 0.0f, 1.0f,
                                         defaults.metallic, nullptr, "%.2f");
            changed |= ui::PropertyFloat("AO", &layer.ambientOcclusion, 0.0f, 1.0f,
                                         defaults.ambientOcclusion, nullptr, "%.2f");
        }
        if (!isShape && !isLiquid && !pathUv) {
            changed |= ui::PropertyFloat(ui::Tr("1 周の長さ", "Tile Length"), &layer.tileMeters, 0.0f, 50000.0f,
                                         defaults.tileMeters,
                                         ui::Tr("このレイヤーの模様が 1 周する長さ（m）。0 で地形の一辺と同じ"
                                                "（地形全体に 1 枚）。地形の実寸を変えても模様の大きさは変わらない",
                                                "Length (m) over which this layer's pattern repeats. 0 equals the terrain side "
                                                "(one tile over the whole terrain). Changing the terrain size does not change the pattern size"),
                                         "%.0f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("近景の 1 周の長さ", "Near Tile Length"), &layer.nearTileMeters, 0.0f, 50.0f,
                                         defaults.nearTileMeters,
                                         ui::Tr("カメラが近づいたときに、同じマテリアルを貼り直す長さ（m）。人間のスケール（2 m 前後）に"
                                                "する。合成結果と地形の形には効かない。0 で貼り直さない。切り替える距離は"
                                                "プレビュー設定の「近景マテリアル」。Layered Material と UV Path の Surface には効かない",
                                                "Length (m) at which the same material is re-tiled when the camera is close. Use a human scale (about 2 m). "
                                                "Does not affect the composite or the terrain shape. 0 disables re-tiling. The switch distance is "
                                                "Near Material in Preview Settings. Has no effect on Layered Material or UV Path Surfaces"),
                                         "%.2f m", ImGuiSliderFlags_Logarithmic);
            if (layer.legacyUvScale > 0.0f) {
                ui::PropertyValue(ui::Tr("旧 UV スケール", "Legacy UV Scale"), ui::Tr("%.2f 倍（m へ換算できていない）", "%.2fx (not converted to m)"), layer.legacyUvScale);
            }
        }
        ui::EndPropertyTable();
    }
    if (!isShape && layer.material != compositor::kNoMaterialAsset) {
        ui::HintText("%s", ui::Tr("色とサーフェスの値はマテリアル側で決まる",
                                  "Color and surface values come from the material"));
    }

    // パス UV。UV Path に繋いだパスに沿った帯の座標で貼る。地形の UV は使わないので
    // UV スケールの代わりにここを出す。幅は Path の点が持つ。
    if (pathUv) {
        ui::SectionHeader(ui::Tr("パス UV", "Path UV"));
        if (ui::BeginPropertyTable("layerPathUvRows")) {
            changed |= ui::PropertyFloat(ui::Tr("繰り返し長", "Repeat Length"), &layer.pathUv.repeatMeters, 0.5f, 20000.0f,
                                         defaults.pathUv.repeatMeters,
                                         ui::Tr("進行方向に模様が 1 周する長さ（m）。この間隔でループする",
                                                "Length (m) over which the pattern repeats along the path. It loops at this interval"),
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("幅方向の枚数", "Width Repeat"), &layer.pathUv.widthRepeat, 0.01f, 8.0f,
                                         defaults.pathUv.widthRepeat,
                                         ui::Tr("帯の幅（Path の点の幅）に模様を何枚並べるか。1 で幅いっぱいに 1 枚、"
                                                "1 未満で模様の一部だけを幅に広げる",
                                                "How many pattern tiles span the strip width (the Path point width). 1 fits one tile across, "
                                                "below 1 stretches part of the pattern across the width"),
                                         "%.3f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(ui::Tr("進行方向のずれ", "Along Offset"), &layer.pathUv.offsetMeters, -500.0f,
                                         500.0f, defaults.pathUv.offsetMeters,
                                         ui::Tr("模様を進行方向へずらす距離（m）。継ぎ目の位置を動かす",
                                                "Distance (m) to shift the pattern along the path. Moves the seam"), "%.1f m");
            changed |= ui::PropertyFloat(ui::Tr("縁のカーブ", "Edge Curve"), &layer.pathUv.edgeGamma, 0.05f, 8.0f,
                                         defaults.pathUv.edgeGamma,
                                         ui::Tr("帯のフェザーの落ち方。1 未満で外側まで残り、1 より大きいと内側へ締まる"
                                                "（Mask Path のガンマと同じ）",
                                                "Falloff of the strip feather. Below 1 it reaches the outside; above 1 it tightens inward "
                                                "(same as Mask Path's Gamma)"),
                                         "%.2f", ImGuiSliderFlags_Logarithmic);
            static const ui::LocalizedText kAlongAxisLabels[] = {{"縦（V）", "Vertical (V)"}, {"横（U）", "Horizontal (U)"}};
            int alongAxis = layer.pathUv.alongU ? 1 : 0;
            if (ui::PropertyCombo(ui::Tr("進行方向の軸", "Along Axis"), &alongAxis, ui::Tr(kAlongAxisLabels).data(), 2,
                                  defaults.pathUv.alongU ? 1 : 0,
                                  ui::Tr("テクスチャのどの軸をパスの進行方向に当てるか。"
                                         "縦なら繰り返し長が V、幅方向の枚数が U に効く。横なら逆",
                                         "Which texture axis runs along the path. "
                                         "Vertical applies Repeat Length to V and Width Repeat to U; Horizontal is the reverse"))) {
                layer.pathUv.alongU = (alongAxis == 1);
                changed = true;
            }
            changed |= ui::PropertyBool(ui::Tr("点の強さ", "Point Strength"), &layer.pathUv.usePointIntensity,
                                        defaults.pathUv.usePointIntensity,
                                        ui::Tr("Path の点が持つ「強さ」を帯の覆い具合に掛ける（Mask Path と同じ効き方）。"
                                               "強さを下げた点へ向けて、素材の凹凸の高い所から順に欠けて消えていく",
                                               "Multiplies the strip coverage by the Path points' Strength (same as Mask Path). "
                                               "Toward points with lower strength, the material fades out starting from its high spots"));
            // マスク画像。帯の座標で貼って覆い具合に掛ける（モレーンの筋、轍、破線）。
            changed |= DrawMapSlotRow(ui::Tr("マスク画像", "Mask Image"), layer.pathUv.mask, m_textureLibrary, m_pendingAssetReveal);
            if (layer.pathUv.mask.texture != compositor::kNoTexture) {
                // サムネイル。読むチャンネルだけを灰色で出す（実際に掛かる絵）。
                // リンク切れは絵が無い。ImGui::Image に 0 を渡すと落ちるので警告のタイルにする。
                if (const compositor::LibraryTexture* maskTexture =
                        m_textureLibrary.Find(layer.pathUv.mask.texture); maskTexture != nullptr) {
                    ui::PropertyLabelEmpty("pathUvMaskThumbnail");
                    const float thumbnailSize = ui::Scaled(72.0f);
                    if (maskTexture->missing) {
                        const ImVec2 min = ImGui::GetCursorScreenPos();
                        ui::MissingThumbnail(min, ImVec2(min.x + thumbnailSize, min.y + thumbnailSize));
                        ImGui::Dummy(ImVec2(thumbnailSize, thumbnailSize));
                    } else {
                        ImGui::Image(static_cast<ImTextureID>(
                                         maskTexture->ChannelHandle(static_cast<int>(layer.pathUv.mask.channel)).ptr),
                                     ImVec2(thumbnailSize, thumbnailSize));
                    }
                    ui::PropertyEnd();
                }
                changed |= ui::PropertyFloat(ui::Tr("マスク周期", "Mask Repeat"), &layer.pathUv.maskRepeatMeters, 0.5f,
                                             20000.0f, defaults.pathUv.maskRepeatMeters,
                                             ui::Tr("進行方向にマスク画像が 1 周する長さ（m）。素材の繰り返し長とは別に決める",
                                                    "Length (m) over which the mask image repeats along the path. Set separately from the material's Repeat Length"),
                                             "%.1f m", ImGuiSliderFlags_Logarithmic);
                changed |= ui::PropertyFloat(ui::Tr("マスク枚数", "Mask Width Repeat"), &layer.pathUv.maskWidthRepeat, 0.01f,
                                             8.0f, defaults.pathUv.maskWidthRepeat,
                                             ui::Tr("帯の幅にマスク画像を何枚並べるか。1 で幅いっぱいに 1 枚",
                                                    "How many mask image tiles span the strip width. 1 fits one tile across"),
                                             "%.3f", ImGuiSliderFlags_Logarithmic);
                changed |= ui::PropertyBool(ui::Tr("マスク反転", "Invert Mask"), &layer.pathUv.maskInvert,
                                            defaults.pathUv.maskInvert,
                                            ui::Tr("マスク画像の白黒を反転して掛ける", "Applies the mask image with black and white inverted"));
            }
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("UV Path のパスに沿って素材を貼る。進行方向が模様の縦（既定）または横。"
                                  "幅とフェザーは Path の点が持ち、帯の外には乗らない",
                                  "Applies the material along the UV Path's path. The path runs along the pattern's vertical (default) or horizontal axis. "
                                  "Width and feather come from the Path points; nothing is applied outside the strip"));
    }

    if (isLiquid) {
        // 水位は絶対値で、下地の高さと比べて「低い所」にだけ水が張る。
        // 重みは水位と下地の差だけで決まるので、水位を動かしても下地は変形しない。
        ui::SectionHeader(ui::Tr("水面", "Water Surface"));
        if (ui::BeginPropertyTable("layerLiquidRows")) {
            changed |= ui::PropertyFloat(ui::Tr("水位", "Water Level"), &layer.heightBase, 0.0f, 1.0f,
                                         defaults.heightBase,
                                         ui::Tr("この高さより低い所に水面が張る", "Water fills areas lower than this height"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("フェザー", "Feather"), &layer.blendRange, 0.0f, 0.2f,
                                         defaults.blendRange,
                                         ui::Tr("汀線の柔らかさ。0 に近いほど硬い水際になる",
                                                "Softness of the shoreline. Closer to 0 gives a harder water's edge"), "%.3f");
            // 水深で色と透け方を決める。深い所の色は上の「ベースカラー」。
            changed |= ui::PropertyColorLinear(ui::Tr("浅瀬の色", "Shallow Color"), &layer.liquid.shallowColor.x,
                                               &defaults.liquid.shallowColor.x,
                                               ui::Tr("水の浅い所の色。深くなるほど上のベースカラー（深い所の色）へ寄る",
                                                      "Color of shallow water. Shifts toward the Base Color above (the deep color) as depth increases"));
            changed |= ui::PropertyFloat(ui::Tr("色の変わる深さ", "Color Change Depth"), &layer.liquid.colorDepthMeters, 0.0f, 100.0f,
                                         defaults.liquid.colorDepthMeters,
                                         ui::Tr("浅瀬の色から深い所の色へ変わる水深（m）。大きいほど沖まで浅瀬の色が続く。"
                                                "0 で深い所の色だけ",
                                                "Water depth (m) over which the shallow color turns into the deep color. Larger extends the shallow color farther out. "
                                                "0 uses only the deep color"),
                                         "%.1f m");
            changed |= ui::PropertyFloat(ui::Tr("透ける深さ", "Clarity Depth"), &layer.liquid.clarityMeters, 0.0f, 50.0f,
                                         defaults.liquid.clarityMeters,
                                         ui::Tr("水越しに底の色が見える水深（m）。大きいほど澄んだ水。0 で透けない",
                                                "Water depth (m) through which the bottom is visible. Larger is clearer water. 0 is opaque"),
                                         "%.1f m");
            ui::EndPropertyTable();
        }
        // 波は合成に焼かず、地形の描画が水面へ動く模様を重ねる。
        ui::SectionHeader(ui::Tr("波", "Waves"));
        if (ui::BeginPropertyTable("layerLiquidWaveRows")) {
            changed |= ui::PropertyFloat(ui::Tr("強さ", "Strength"), &layer.liquid.waveStrength, 0.0f, 1.0f,
                                         defaults.liquid.waveStrength,
                                         ui::Tr("波の傾きの強さ。0 で平らな水面", "Slope strength of the waves. 0 is a flat surface"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("大きさ", "Size"), &layer.liquid.waveScaleMeters, 1.0f, 500.0f,
                                         defaults.liquid.waveScaleMeters,
                                         ui::Tr("一番大きなうねりの波長（m）。細かいさざ波はここから段々に小さくして重ねる",
                                                "Wavelength (m) of the largest swell. Finer ripples are layered at progressively smaller sizes"),
                                         "%.0f m");
            changed |= ui::PropertyFloat(ui::Tr("速さ", "Speed"), &layer.liquid.waveSpeed, 0.0f, 4.0f,
                                         defaults.liquid.waveSpeed,
                                         ui::Tr("進む速さの倍率。1 で波長に見合った実際の速さ。0 で止める",
                                                "Speed multiplier. 1 is the physical speed for the wavelength. 0 stops it"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("向き", "Direction"), &layer.liquid.waveDirectionDegrees, -180.0f, 180.0f,
                                         defaults.liquid.waveDirectionDegrees,
                                         ui::Tr("うねりの進む向き（度）。0 で +X、90 で +Z へ進む",
                                                "Direction the swell travels (degrees). 0 goes toward +X, 90 toward +Z"), "%.0f°");
            changed |= ui::PropertyFloat(ui::Tr("向きの揃い", "Directionality"), &layer.liquid.waveDirectional, 0.0f, 1.0f,
                                         defaults.liquid.waveDirectional,
                                         ui::Tr("波が「向き」へ揃って進む度合い。1 で全部の波が向きの前後へ進む（外洋のうねり）。"
                                                "0 で決まった向きを持たない（湾や内海）",
                                                "How much the waves align with Direction. 1 sends every wave forward or back along it (open-ocean swell). "
                                                "0 has no set direction (bays and inland seas)"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("波面の大きさ", "Facet Size"), &layer.liquid.waveFacetSizeMeters, 0.0f, 0.2f,
                                         defaults.liquid.waveFacetSizeMeters,
                                         ui::Tr("太陽のきらめきを作る、一番小さな波面の大きさ（m）。画素の中の波面を数え、"
                                                "太陽を目へ返す向きのものだけを光らせる。小さいほど粒が細かく、"
                                                "大きいほど粗く明るい。全体の明るさは変わらない。0 で粒なし",
                                                "Size (m) of the smallest wave facets that make sun glints. Counts the facets in each pixel "
                                                "and lights only those that reflect the sun toward the eye. Smaller gives finer sparkles, "
                                                "larger coarser and brighter ones. Overall brightness stays the same. 0 means no sparkles"),
                                         "%.3f m");
            ui::EndPropertyTable();
        }
        // 波打ち際も描画で動かす。泡の筋は水際からの距離で進み、岸の形に沿って岸へ向かう。
        ui::SectionHeader(ui::Tr("波打ち際", "Shoreline"));
        if (ui::BeginPropertyTable("layerLiquidShoreRows")) {
            changed |= ui::PropertyFloat(ui::Tr("泡の量", "Foam Amount"), &layer.liquid.shoreFoam, 0.0f, 1.0f,
                                         defaults.liquid.shoreFoam,
                                         ui::Tr("岸へ向かって進む泡の筋と、寄せた波の先端の泡の濃さ。0 で泡なし",
                                                "Density of the foam lines moving toward shore and of the foam at the edge of the run-up. 0 means no foam"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("寄せる高さ", "Run-Up Height"), &layer.liquid.shoreRunupMeters, 0.0f, 3.0f,
                                         defaults.liquid.shoreRunupMeters,
                                         ui::Tr("寄せる波が水位より上へ這い上がる高さ（m）。浜が緩いほど遠くまで届く。"
                                                "届く範囲は濡れた色になる。0 で寄せ返しなし",
                                                "Height (m) the waves run up above the water level. Gentler beaches let them reach farther. "
                                                "The reached area looks wet. 0 means no run-up"),
                                         "%.2f m");
            changed |= ui::PropertyFloat(ui::Tr("波の間隔", "Wave Spacing"), &layer.liquid.shoreSpacingMeters, 2.0f, 200.0f,
                                         defaults.liquid.shoreSpacingMeters,
                                         ui::Tr("泡の筋どうしの間隔（m）", "Spacing (m) between foam lines"), "%.0f m");
            changed |= ui::PropertyFloat(ui::Tr("波の出る幅", "Wave Band Width"), &layer.liquid.shoreWidthMeters, 0.0f, 500.0f,
                                         defaults.liquid.shoreWidthMeters,
                                         ui::Tr("泡の筋が出る、水際からの幅（m）", "Width (m) from the water's edge where foam lines appear"), "%.0f m");
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("波と波打ち際は地形の描画で動かす（合成結果や書き出すテクスチャには入らない）。"
                                  "Sea が複数あるときは、最初の有効な 1 つの設定を使う",
                                  "Waves and shoreline are animated by the terrain renderer (not baked into the composite or exported textures). "
                                  "With several Seas, the first enabled one's settings are used"));
        // Mask 出力（Water / Depth / Shore）の目盛り。
        ui::SectionHeader(ui::Tr("マスク出力", "Mask Output"));
        if (ui::BeginPropertyTable("layerLiquidMaskRows")) {
            changed |= ui::PropertyFloat(ui::Tr("Depth の深さ", "Depth Range"), &layer.liquid.maskDepthMeters, 0.1f, 100.0f,
                                         defaults.liquid.maskDepthMeters,
                                         ui::Tr("Depth の出力が 1 になる水深（m）。浅瀬と沖を分けるのに使う",
                                                "Water depth (m) at which the Depth output reaches 1. Use it to separate shallows from open water"), "%.1f m");
            changed |= ui::PropertyFloat(ui::Tr("Shore の幅", "Shore Width"), &layer.liquid.maskShoreMeters, 0.5f, 200.0f,
                                         defaults.liquid.maskShoreMeters,
                                         ui::Tr("Shore の帯の幅（m）。水際で 1、陸側・水側ともこの距離で 0",
                                                "Width (m) of the Shore band. 1 at the water's edge, 0 at this distance on both the land and water sides"), "%.1f m");
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("Water は水面の範囲（水の中が 1）、Depth は水深、Shore は水際の帯。"
                                  "この Sea より後ろのノードで使う",
                                  "Water is the water area (1 inside the water), Depth is the water depth, Shore is the band at the water's edge. "
                                  "Use them in nodes after this Sea"));
    } else if (layeredMaterial) {
        ui::SectionHeader(ui::Tr("ハイト", "Height"));
        if (ui::BeginPropertyTable("layerHeightRows")) {
            changed |= ui::PropertyFloat(ui::Tr("基準の高さ", "Base Height"), &layer.heightBase, -2.0f, 2.0f, defaults.heightBase,
                                         ui::Tr("合成材質を置く基準の高さ。起伏の実寸は材質側の変位量で決まる",
                                                "Base height at which the layered material is placed. The actual relief comes from the material's displacement"));
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("ハイトと法線はレイヤーマテリアル内部で合成する。変位量は材質側で設定する",
                                  "Height and normals are blended inside the Layered Material. Set displacement on the material"));
    } else {
        ui::SectionHeader(ui::Tr("ハイト", "Height"));
        if (ui::BeginPropertyTable("layerHeightRows")) {
            int heightSource = static_cast<int>(layer.heightSource);
            if (ui::PropertyCombo(ui::Tr("ソース", "Source"), &heightSource, ui::Tr(kValueSourceLabels).data(),
                                  IM_ARRAYSIZE(kValueSourceLabels),
                                  static_cast<int>(defaults.heightSource))) {
                layer.heightSource = static_cast<compositor::ValueSource>(heightSource);
                changed = true;
            }
            if (isShape) {
                // シェイプはマテリアルを持たないので、ハイトマップは
                // レイヤー直結のスロットから読む（マスクの画像と同じ作法）。
                if (layer.heightSource == compositor::ValueSource::Texture) {
                    changed |= DrawMapSlotRow(ui::Tr("画像", "Image"), layer.heightTexture, m_textureLibrary, m_pendingAssetReveal);
                }
                // シェイプの基準の高さは「全体の持ち上げ」。0.5 で変化なし。
                changed |= ui::PropertyFloat(ui::Tr("持ち上げ", "Lift"), &layer.heightBase, 0.0f, 1.0f,
                                             defaults.heightBase,
                                             ui::Tr("0.5 で変化なし。上げると全体が盛り上がり、"
                                                    "下げると沈む",
                                                    "0.5 means no change. Raising it lifts everything, "
                                                    "lowering it sinks everything"),
                                             "%.2f");
            } else {
                changed |= ui::PropertyFloat(
                    ui::Tr("基準の高さ", "Base Height"), &layer.heightBase, -2.0f, 2.0f, defaults.heightBase,
                    ui::Tr("このレイヤーが「溜まる水位」。下地の高さと比べて勝敗が決まる。"
                           "起伏の強さを変えてもここは動かない",
                           "The level this layer fills to, like water. Compared with the base height to decide which wins. "
                           "Changing Relief Strength does not move it"),
                    "%.2f");
            }
            // ソース（Heightmap）は画像の 0〜1 がそのままハイトの全幅（強さ 1.0 固定）。
            // 振れ幅は下の「スケール」の標高差（m）が決めるので、二重に持たせない。
            if (layer.heightSource != compositor::ValueSource::Constant && !isSource) {
                changed |= ui::PropertyFloat(ui::Tr("起伏の強さ", "Relief Strength"), &layer.heightGain, 0.0f, 3.0f,
                                             defaults.heightGain,
                                             ui::Tr("基準の高さを中心とした凹凸の振れ幅。0 で平らになる",
                                                    "Amplitude of the relief around Base Height. 0 makes it flat"),
                                             "%.2f");
            }
            if (layer.heightSource == compositor::ValueSource::Noise) {
                changed |= DrawNoiseRows(layer.heightNoise, defaults.heightNoise, false);
            }
            ui::EndPropertyTable();
        }
        if (isSource) {
            // ハイトは 0〜1 の正規化値。**何 m かは下の「スケール」の標高差**が決める。
            ui::HintText("%s", ui::Tr("画像の 0〜1 がハイトの全幅。実際の高さ（m）はスケールの標高差で決まる",
                                      "The image's 0-1 range is the full height range. The actual height (m) comes from the Scale's elevation range"));
        } else if (isShape) {
            ui::HintText("%s", ui::Tr("下地の高さへ加算し、0〜1 に切り詰める。細部は下のレイヤーのまま残る",
                                      "Adds to the base height and clamps to 0-1. Detail from the layers below is kept"));
        }
    }

    // マスクは「下地と競合させるための不透明度」なので、
    // 入力を持たないソース（ハイトマップ）では意味を持たない。行ごと出さない。
    if (!isSource) {
        ui::SectionHeader(ui::Tr("マスク", "Mask"));
        if (ui::BeginPropertyTable("layerMaskRows")) {
            // Mask 入力にノードが繋がっているときは、そちらが出どころ。
            if (maskFromNode) {
                ui::PropertyValue(ui::Tr("ソース", "Source"), "%s", ui::Tr("画像（Mask 入力）", "Image (Mask input)"));
            } else {
                int maskSource = static_cast<int>(layer.mask.source);
                if (ui::PropertyCombo(ui::Tr("ソース", "Source"), &maskSource, ui::Tr(kMaskSourceLabels).data(),
                                      IM_ARRAYSIZE(kMaskSourceLabels),
                                      static_cast<int>(kDefaultLayer.mask.source),
                                      ui::Tr("マスクは被覆率。1.0 で全面を覆い、"
                                             "中間はこのレイヤーの起伏の高い所から順に出る",
                                             "The mask is coverage. 1.0 covers everything; "
                                             "in between, this layer shows up starting from its highest relief"))) {
                    layer.mask.source = static_cast<compositor::MaskSource>(maskSource);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat(ui::Tr("定数", "Constant"), &layer.mask.constant, 0.0f, 1.0f,
                                         kDefaultLayer.mask.constant,
                                         ui::Tr("ソースの値に掛ける係数", "Factor multiplied with the source value"), "%.2f");

            if (!maskFromNode && layer.mask.source == compositor::MaskSource::Texture) {
                changed |= DrawMapSlotRow(ui::Tr("画像", "Image"), layer.mask.texture, m_textureLibrary, m_pendingAssetReveal);
            }
            if (!maskFromNode && layer.mask.source == compositor::MaskSource::Noise) {
                changed |= DrawNoiseRows(layer.mask.noise, kDefaultLayer.mask.noise);
            }
            if (compositor::IsDerivedMaskSource(layer.mask.source)) {
                changed |= ui::PropertyFloat(ui::Tr("強調", "Emphasis"), &layer.mask.derivedScale, 0.0f, 8.0f,
                                             kDefaultLayer.mask.derivedScale,
                                             ui::Tr("下地から作った値の効き方", "How strongly the value derived from the base applies"), "%.2f");
            }

            changed |= ui::PropertyFloat(ui::Tr("カーブ", "Curve"), &layer.mask.contrast, 0.0f, 4.0f,
                                         kDefaultLayer.mask.contrast,
                                         ui::Tr("1 で線形。大きいほど境界がはっきりする", "1 is linear. Larger makes edges sharper"), "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("レベル下限", "Levels Low"), &layer.mask.levelsLow, 0.0f, 1.0f,
                                         kDefaultLayer.mask.levelsLow, nullptr, "%.2f");
            changed |= ui::PropertyFloat(ui::Tr("レベル上限", "Levels High"), &layer.mask.levelsHigh, 0.0f, 1.0f,
                                         kDefaultLayer.mask.levelsHigh, nullptr, "%.2f");
            changed |= ui::PropertyBool(ui::Tr("反転", "Invert"), &layer.mask.invert, kDefaultLayer.mask.invert);
            ui::EndPropertyTable();
        }

        if (isBase) {
            ui::HintText("%s", ui::Tr("一番下のレイヤーは下地なのでマスクは効かない",
                                      "The bottom layer is the base, so the mask has no effect"));
        }
        switch (layer.mask.source) {
            case compositor::MaskSource::Slope:
                ui::HintText("%s", ui::Tr("急な面ほど 1 に近づく", "Steeper surfaces approach 1"));
                break;
            case compositor::MaskSource::Curvature:
                ui::HintText("%s", ui::Tr("0.5 が平坦。凸で大、凹で小", "0.5 is flat. Higher on convex, lower on concave"));
                break;
            case compositor::MaskSource::Cavity:
                ui::HintText("%s", ui::Tr("窪んでいるほど 1 に近づく", "Deeper hollows approach 1"));
                break;
            case compositor::MaskSource::Height:
                ui::HintText("%s", ui::Tr("下地が高いほど 1 に近づく", "Higher base areas approach 1"));
                break;
            default:
                break;
        }
    }

    if (layer.mask.source == compositor::MaskSource::Paint) {
        ui::SectionHeader(ui::Tr("ペイント", "Paint"));
        changed |= DrawPaintSection(layer);
    }

    // マテリアルの割り当てと合成の調整はサーフェスだけのもの。
    // シェイプは Normal / Height を加算で書くと決まっており、
    // 水面は水位とフェザーが合成のすべてなので、出しても意味を持たない。
    if (!isShape && !isLiquid) {
        ui::SectionHeader(ui::Tr("マテリアル", "Material"));
        if (ui::BeginPropertyTable("layerMaterialRows")) {
            changed |= DrawMaterialSlotRow(ui::Tr("マテリアル", "Material"), layer.material, m_materialLibrary, m_pendingAssetReveal);
            ui::EndPropertyTable();
        }
        if (const compositor::MaterialAsset* material = m_materialLibrary.Find(layer.material);
            material != nullptr && material->thumbnail.IsValid()) {
            ImGui::Image(static_cast<ImTextureID>(material->thumbnail.srv.gpu.ptr),
                         ImVec2(ui::Scaled(72.0f), ui::Scaled(72.0f)));
        } else {
            ui::HintText("%s", ui::Tr("マテリアルパネルで作って割り当てる",
                                      "Create one in the Material panel and assign it"));
        }

        ui::SectionHeader(ui::Tr("合成", "Blend"));
        if (ui::BeginPropertyTable("layerBlendRows")) {
            changed |= ui::PropertyFloat(ui::Tr("境界の柔らかさ", "Edge Softness"), &layer.blendRange, 0.0f, 1.0f,
                                         defaults.blendRange,
                                         ui::Tr("0 に近いほど硬い置き換えになる", "Closer to 0 gives a harder replacement"), "%.2f");
            changed |= ui::PropertyBool(ui::Tr("下地に沿わせる", "Wrap to Base"), &layer.wrapToUnderlying,
                                        defaults.wrapToUnderlying,
                                        ui::Tr("下地の形を保ったまま表面を被せる（コーティング）。"
                                               "基準の高さの 0.5 からのずれが被せ物の厚みになる",
                                               "Coats the surface while keeping the base shape. "
                                               "The offset of Base Height from 0.5 becomes the coating thickness"));

            ui::PropertyLabel(ui::Tr("書き込み", "Write"), ui::Tr("このレイヤーが書き込むチャンネル", "Channels this layer writes"));
            for (uint32_t i = 0; i < IM_ARRAYSIZE(kChannelLabels); ++i) {
                bool enabled = (layer.channelMask & (1u << i)) != 0u;
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Checkbox(kChannelLabels[i], &enabled)) {
                    layer.channelMask = enabled ? (layer.channelMask | (1u << i))
                                                : (layer.channelMask & ~(1u << i));
                    changed = true;
                }
                ImGui::PopID();
            }
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }
    }

    return changed;
}

bool Application::DrawPaintSection(compositor::MaterialLayer& layer) {
    bool changed = false;

    if (layer.mask.paint == compositor::kNoPaintMask) {
        ui::HintText("%s", ui::Tr("このレイヤーにはまだペイントマスクがない", "This layer has no paint mask yet"));
        if (ui::Button(ui::Tr("マスクを作成", "Create Mask"), ui::kWideButtonWidth)) {
            layer.mask.paint = m_paintMasks.Add(m_device, 0.0f);
            m_paintMode = (layer.mask.paint != compositor::kNoPaintMask);
            changed = true;
        }
        return changed;
    }

    if (ui::BeginPropertyTable("layerPaintRows")) {
        ui::PropertyBool(ui::Tr("ペイントモード", "Paint Mode"), &m_paintMode, false,
                         ui::Tr("オンの間、ビューポートのドラッグがブラシになる",
                                "While on, dragging in the viewport paints with the brush"));
        ui::PropertyFloat(ui::Tr("ブラシ半径", "Brush Radius"), &m_brush.radiusPixels, 4.0f, 256.0f,
                          kDefaultBrush.radiusPixels,
                          ui::Tr("画面上の半径。視点や UV スケールを変えても見た目の大きさは変わらない",
                                 "Radius on screen. Its apparent size stays the same when the view or UV scale changes"),
                          "%.0f px");
        ui::PropertyFloat(ui::Tr("強さ", "Strength"), &m_brush.strength, 0.01f, 1.0f, kDefaultBrush.strength,
                          ui::Tr("1 回の適用で足す量", "Amount added per application"), "%.2f");
        ui::PropertyFloat(ui::Tr("減衰", "Falloff"), &m_brush.falloff, 0.2f, 8.0f, kDefaultBrush.falloff,
                          ui::Tr("1 で線形。大きいほど中心に集中する", "1 is linear. Larger concentrates toward the center"), "%.2f");
        ui::PropertyBool(ui::Tr("消しゴム", "Eraser"), &m_brush.erase, kDefaultBrush.erase,
                         ui::Tr("左右のドラッグの意味を入れ替える", "Swaps the roles of left and right drag"));

        ui::PropertyLabelEmpty("paintFill");
        if (ui::Button(ui::Tr("全消去", "Clear All"))) {
            m_paintMasks.QueueSnapshot(m_device, layer.mask.paint);
            m_paintMasks.QueueFill(layer.mask.paint, 0.0f);
        }
        ImGui::SameLine();
        if (ui::Button(ui::Tr("全塗り", "Fill All"))) {
            m_paintMasks.QueueSnapshot(m_device, layer.mask.paint);
            m_paintMasks.QueueFill(layer.mask.paint, 1.0f);
        }
        ui::PropertyEnd();

        ui::PropertyLabelEmpty("paintHistory");
        ImGui::BeginDisabled(!m_paintMasks.CanUndo());
        if (ui::Button(ui::Tr("アンドゥ", "Undo"))) {
            m_paintMasks.QueueUndo(m_device);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_paintMasks.CanRedo());
        if (ui::Button(ui::Tr("リドゥ", "Redo"))) {
            m_paintMasks.QueueRedo(m_device);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(ui::Tr("(%zu 段)", "(%zu steps)"), m_paintMasks.UndoCount());
        ui::PropertyEnd();

        // 解像度はすべてのペイントマスクで共通。
        int resolution = ResolutionIndex(m_paintMasks.RequestedResolution());
        if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &resolution, kResolutionLabels,
                              IM_ARRAYSIZE(kResolutionLabels), 1,
                              ui::Tr("全ペイントマスクを拡大縮小する。履歴は破棄される",
                                     "Rescales all paint masks. The history is discarded"))) {
            m_paintMasks.RequestResolution(kResolutionValues[resolution]);
        }

        ui::PropertyLabelEmpty("paintDiscard");
        if (ui::Button(ui::Tr("マスクを破棄", "Discard Mask"), ui::kWideButtonWidth)) {
            // 実体はここでは消さない。履歴から参照されている間は SweepPaintMasks が
            // 持っておき、アンドゥで戻したときに描いた内容が失われないようにする
            // （RemoveLayer と同じ方針）。
            layer.mask.paint = compositor::kNoPaintMask;
            m_paintMode = false;
            changed = true;
        }
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }

    ui::HintText("%s", ui::Tr("左ドラッグで塗る / 右ドラッグで消す / Alt + 左ドラッグで視点を回す",
                              "Left-drag to paint / right-drag to erase / Alt + left-drag to orbit the view"));
    return changed;
}

}  // namespace tg
