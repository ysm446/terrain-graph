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
bool Application::DrawLayerSettings(compositor::MaterialLayer& layer, bool isBase, bool isSource,
                                   bool maskFromNode, bool maskResolves) {
    // **困っていることは一番上に出す。** 設定の行が多いレイヤーだと、
    // マスクの節はスクロールの外へ行ってしまう。
    if (!maskResolves) {
        ui::HintText("Mask 入力に繋いだノードがこのチェーンの中にいないので、"
                     "マスクが効いていない。そのノードの Result を下地として通すこと");
    }
    // 既定値マーカーは種類ごとの既定値を参照する（追加時の初期値と揃える）。
    const compositor::MaterialLayer& defaults = DefaultLayerFor(layer.kind);
    const bool isShape = (layer.kind == compositor::LayerKind::Shape);
    const bool isLiquid = (layer.kind == compositor::LayerKind::Liquid);
    bool changed = false;

    // 堆積は合成レイヤーではなく「下地のハイトを土砂で作り替える加工」。
    if (layer.kind == compositor::LayerKind::Sediment) {
        const compositor::MaterialLayer::SedimentSettings sedimentDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("sedimentBasicRows")) {
            char sedimentName[128] = {};
            std::snprintf(sedimentName, sizeof(sedimentName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", sedimentName, sizeof(sedimentName))) {
                layer.name = sedimentName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("堆積");
        if (ui::BeginPropertyTable("sedimentRows")) {
            changed |= ui::PropertyBool(
                "地形を土砂にする", &layer.sediment.convertTerrain,
                sedimentDefaults.convertTerrain,
                "入の間は入力の地形そのものが崩れて谷を埋める。"
                "切ると入力は動かない基盤になり、供給量で足したぶんだけが流れる");
            changed |= ui::PropertyFloat("供給量", &layer.sediment.emissionMeters, 0.0f, 20.0f,
                                         sedimentDefaults.emissionMeters,
                                         "上乗せする土砂の厚み（m）。Emission 入力を繋ぐと"
                                         "その明るさに比例して積む（繋がなければ全面へ一様）",
                                         "%.2f m");
            changed |= ui::PropertyFloat(
                "供給時間", &layer.sediment.emissionTime, 0.0f, 1.0f,
                sedimentDefaults.emissionTime,
                "0 で最初に全量を積む。上げるほど反復に分けて積むので、"
                "前の反復が彫った河道に流れ込んで筋がはっきりする",
                "%.2f");
            changed |= ui::PropertyFloat(
                "最大ディテール", &layer.sediment.detailMeters, 1.0f, 512.0f,
                sedimentDefaults.detailMeters,
                "1 反復でどこまで土砂を運ぶか（m）。大きいほど広い盆地が早く落ち着くが重い",
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "粘性", &layer.sediment.viscosity, 0.0f, 1.0f, sedimentDefaults.viscosity,
                "安息角。0 で水平に均され、上げるほど急な斜面のまま留まる（1 で 80 度）",
                "%.2f");
            changed |= ui::PropertyInt("反復", &layer.sediment.iterations, 1, 200,
                                       sedimentDefaults.iterations,
                                       "崩し直す回数。多いほど落ち着くが重い");
            changed |= ui::PropertyInt("安定化", &layer.sediment.stabilization, 1, 8,
                                       sedimentDefaults.stabilization,
                                       "1 反復のなかで何回滑らせるか。"
                                       "粘性が低いときは上げると暴れにくい");
            int resolutionIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.sediment.resolution) {
                    resolutionIndex = i;
                }
            }
            if (ui::PropertyCombo("解像度", &resolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 1,
                                  "土砂を動かすグリッド。合成解像度とは別。"
                                  "上げるほど細かい筋が出るが、反復のコストも比例して増える")) {
                layer.sediment.resolution = kSedimentResolutionValues[resolutionIndex];
                changed = true;
            }
            changed |= ui::PropertyFloat(
                "基準の厚み", &layer.sediment.maskThicknessMeters, 0.0f, 20.0f,
                sedimentDefaults.maskThicknessMeters,
                "Mask 出力がこの厚みで 1 になる。0 にすると一番厚い所が 1 に"
                "なるが、少数の分厚い点が基準になって残りが 0 付近へ潰れる",
                "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "マスクの締まり", &layer.sediment.maskContrast, 0.0f, 1.0f,
                sedimentDefaults.maskContrast,
                "Mask 出力（積もった厚み）のコントラスト。0 で線形、"
                "上げるほど「積もった / 積もっていない」がはっきり分かれる",
                "%.2f");
            ui::EndPropertyTable();
        }
        ui::HintText("重力で土砂を再分配する。谷底に厚く積もり、尾根は痩せる。"
                     "Emission 入力で供給する場所を絞れる（「地形を土砂にする」を切り、"
                     "粘性を下げると、注ぎ口から流した液体になる）。"
                     "Mask 出力は積もった厚みなので、堆積した所へ別のマテリアルを乗せられる");
        return changed;
    }

    // 積雪も合成レイヤーではなく「下地のハイトへ雪を積む加工」。
    // 降る量は一様なので、マスクの節は出さない（どこに積もるかは雪面が決める）。
    if (layer.kind == compositor::LayerKind::Snow) {
        const compositor::MaterialLayer::SnowSettings snowDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("snowBasicRows")) {
            char snowName[128] = {};
            std::snprintf(snowName, sizeof(snowName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", snowName, sizeof(snowName))) {
                layer.name = snowName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("積雪");
        if (ui::BeginPropertyTable("snowRows")) {
            changed |= ui::PropertyFloat("積雪量", &layer.snow.emissionMeters, 0.0f, 50.0f,
                                         snowDefaults.emissionMeters,
                                         "地形へ降らせる雪の総量。0 なら入力をそのまま通す",
                                         "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "供給時間", &layer.snow.emissionTime, 0.0f, 1.0f, snowDefaults.emissionTime,
                "0 なら最初に全量を置いてから流す。上げるほど段に分けて降らせるので、"
                "溜まる所がはっきりする",
                "%.2f");
            changed |= ui::PropertyFloat(
                "安息角", &layer.snow.motionSlopeDegrees, 0.0f, 89.0f,
                snowDefaults.motionSlopeDegrees,
                "雪面がこれ以下の傾斜なら雪は動かない（地形ではなく雪面の角度）。"
                "下げるほど急な所に残らなくなる",
                "%.1f 度");
            changed |= ui::PropertyFloat(
                "流動率", &layer.snow.transportRate, 0.0f, 1.0f, snowDefaults.transportRate,
                "1 回の滑らせで動く割合。高いほど急斜面から早く逃げる", "%.2f");
            changed |= ui::PropertyFloat(
                "雪面のならし", &layer.snow.surfaceSmoothing, 0.0f, 1.0f,
                snowDefaults.surfaceSmoothing,
                "積もった雪面だけをならす強さ。0 で切る（地形の凹凸がそのまま出る）",
                "%.2f");
            changed |= ui::PropertyFloat(
                "最大ディテール", &layer.snow.detailMeters, 1.0f, 512.0f,
                snowDefaults.detailMeters,
                "雪が移動先を探す最大の距離。大きいほど広い斜面の下まで流れるが重い。"
                "雪面をならす半径にも使う",
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyInt("反復", &layer.snow.iterations, 1, 256,
                                       snowDefaults.iterations,
                                       "シミュレーションの段数。多いほど雪が落ち着く");
            changed |= ui::PropertyInt("安定化", &layer.snow.settlingPasses, 1, 16,
                                       snowDefaults.settlingPasses,
                                       "1 段のなかで雪を滑らせる回数");
            int snowResolutionIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.snow.resolution) {
                    snowResolutionIndex = i;
                }
            }
            if (ui::PropertyCombo("解像度", &snowResolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 1,
                                  "雪を動かすグリッド。合成解像度とは別。"
                                  "上げるほど細かい雪の筋が出るが重い")) {
                layer.snow.resolution = kSedimentResolutionValues[snowResolutionIndex];
                changed = true;
            }
            changed |= ui::PropertyFloat(
                "被覆のしきい値", &layer.snow.maskThresholdMeters, 0.0f, 5.0f,
                snowDefaults.maskThresholdMeters,
                "Mask 出力がこの積雪厚で白へ寄る", "%.3f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "被覆のぼかし", &layer.snow.maskFeatherMeters, 0.0f, 5.0f,
                snowDefaults.maskFeatherMeters,
                "積雪境界のグレーの幅。0 に近いほど二値に近いマスクになる", "%.3f m",
                ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::HintText("雪を一様に降らせ、急な雪面から低い所へ滑らせて溜める。"
                     "Mask 出力は雪の被覆なので、積もった所へ雪のマテリアルを乗せられる");
        return changed;
    }

    // 河川も合成レイヤーではなく「川筋から河床を掘って水を張る加工」。
    // 川の出どころは Mask 入力（Seed）で受けるので、マスクの節は出さない。
    if (layer.kind == compositor::LayerKind::River) {
        const compositor::MaterialLayer::RiverSettings riverDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("riverBasicRows")) {
            char riverName[128] = {};
            std::snprintf(riverName, sizeof(riverName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", riverName, sizeof(riverName))) {
                layer.name = riverName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("川筋");
        if (ui::BeginPropertyTable("riverFlowRows")) {
            changed |= ui::PropertyFloat(
                "川のしきい値", &layer.river.threshold, 0.0f, 0.05f, riverDefaults.threshold,
                "川とみなす流量。全セル数に対する割合（Mask Fluvial と同じ単位）。"
                "下げるほど細い沢まで川になる",
                "%.4f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "最大ディテール", &layer.river.detailMeters, 1.0f, 512.0f,
                riverDefaults.detailMeters,
                "流向を読む前にならす大きさ。大きいほど大きな谷筋を追う", "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "集中度", &layer.river.concentration, 0.1f, 16.0f, riverDefaults.concentration,
                "下流への配分の集中度。大きいほど主流へ集まる", "%.2f");
            int riverResolutionIndex = 1;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.river.resolution) {
                    riverResolutionIndex = i;
                }
            }
            if (ui::PropertyCombo("解像度", &riverResolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 1,
                                  "川筋を計算するグリッド。合成解像度とは別。"
                                  "反復回数が比例して増えるので、調整中は 256 が軽い")) {
                layer.river.resolution = kSedimentResolutionValues[riverResolutionIndex];
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("掘る");
        if (ui::BeginPropertyTable("riverCarveRows")) {
            changed |= ui::PropertyFloat(
                "主流の幅", &layer.river.mainWidthMeters, 1.0f, 1000.0f,
                riverDefaults.mainWidthMeters,
                "流量が最大のセルでの川幅。幅の基準はここで、支流は流量に応じて細くなる",
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "最小幅", &layer.river.minWidthMeters, 0.5f, 100.0f, riverDefaults.minWidthMeters,
                "しきい値ぎりぎりの細い沢でも、これより細くしない", "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "幅の伸び", &layer.river.widthExponent, 0.0f, 1.0f, riverDefaults.widthExponent,
                "流量に対する幅の指数。0.5 が水理幾何の標準、0 で一定幅", "%.2f");
            changed |= ui::PropertyFloat(
                "河床の深さ", &layer.river.bedDepthMeters, 0.0f, 50.0f,
                riverDefaults.bedDepthMeters,
                "川の中心での水面から河床までの深さ。水際へ向けて U 字に浅くなる", "%.2f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "岸の幅", &layer.river.bankWidthMeters, 0.0f, 100.0f,
                riverDefaults.bankWidthMeters,
                "水際から岸の上端までの距離。岸は水面の高さから立ち上がる"
                "（掘る形の話。河原の広がりとは別）",
                "%.1f m",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "岸の硬さ", &layer.river.bankHardness, 0.0f, 1.0f, riverDefaults.bankHardness,
                "0 でなだらかな土手、1 で切り立った崖", "%.2f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("水面");
        if (ui::BeginPropertyTable("riverWaterRows")) {
            changed |= ui::PropertyBool(
                "水を張る", &layer.river.fillWater, riverDefaults.fillWater,
                "切ると乾いた河床のまま残す（涸れ川・旧河道）。Water / Depth の Mask は出る");
            changed |= ui::PropertyFloat(
                "最小勾配", &layer.river.minSlope, 0.0f, 0.02f, riverDefaults.minSlope,
                "水面が下流へ下がる最小の傾き（0.001 = 1 km で 1 m）。"
                "盆地はこの傾きで出口まで埋まって湖になる。"
                "大きくすると平坦な谷底まで水に浸かる",
                "%.4f", ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::SectionHeader("河原");
        if (ui::BeginPropertyTable("riverShoreRows")) {
            changed |= ui::PropertyFloat(
                "河原の広がり", &layer.river.shoreWidthMeters, 0.0f, 200.0f,
                riverDefaults.shoreWidthMeters,
                "水際から外側へ、河原（Bank）とみなす距離。主流での値で、"
                "支流は幅と同じ比で縮む",
                "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "河原の比高", &layer.river.shoreHeightMeters, 0.0f, 20.0f,
                riverDefaults.shoreHeightMeters,
                "水面からこの高さまでを河原とする（増水時に浸かる帯）。"
                "谷壁を駆け上がらないように高さでも切る",
                "%.2f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "河原のぼかし", &layer.river.shoreFeather, 0.0f, 1.0f, riverDefaults.shoreFeather,
                "河原の縁のなだらかさ。広がりと比高それぞれに対する割合", "%.2f");
            ui::EndPropertyTable();
        }
        ui::HintText("川筋から河床を掘り、下流へ単調に下がる水面を張る。盆地は湖になる。"
                     "Water は水面の被覆、Bank は河原（岩・砂利を置く帯）、Depth は水深。"
                     "水の Surface はハイトを定数にすること（水面の形は River が決める）");
        return changed;
    }

    // 散布は合成レイヤーではなく「形をばら撒く加工」。
    // マスク入力は**散布する範囲**（明るい所ほど置かれやすい）。
    if (layer.kind == compositor::LayerKind::Scatter) {
        const compositor::MaterialLayer::ScatterSettings scatterDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("scatterBasicRows")) {
            char scatterName[128] = {};
            std::snprintf(scatterName, sizeof(scatterName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", scatterName, sizeof(scatterName))) {
                layer.name = scatterName;
                changed = true;
            }

            static const char* const kShapeLabels[] = {"半球", "円錐"};
            int shape = static_cast<int>(layer.scatter.shape);
            if (ui::PropertyCombo("形", &shape, kShapeLabels, IM_ARRAYSIZE(kShapeLabels),
                                  static_cast<int>(scatterDefaults.shape),
                                  "半球は丸い低木や樹冠、円錐は尖った草や針葉樹の目安")) {
                layer.scatter.shape = static_cast<compositor::ScatterShape>(shape);
                changed = true;
            }

            static const char* const kOrientationLabels[] = {"平ら", "地面に沿う", "斜面向き"};
            int orientation = static_cast<int>(layer.scatter.orientation);
            if (ui::PropertyCombo("向き", &orientation, kOrientationLabels,
                                  IM_ARRAYSIZE(kOrientationLabels),
                                  static_cast<int>(scatterDefaults.orientation),
                                  "地形の傾きの扱い。地面に沿うは斜面で低く潰れ、"
                                  "斜面向きは個体の向きを傾きの向きへ寄せる")) {
                layer.scatter.orientation =
                    static_cast<compositor::ScatterOrientation>(orientation);
                changed = true;
            }

            changed |= ui::PropertyFloat("間隔", &layer.scatter.densityMeters, 0.5f, 200.0f,
                                         scatterDefaults.densityMeters,
                                         "散布点の間隔（m）。小さいほど密に置かれる", "%.1f m",
                                         ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("被覆", &layer.scatter.coverage, 0.0f, 1.0f,
                                         scatterDefaults.coverage,
                                         "散布点に実際に置く確率。Mask 入力があれば掛かる",
                                         "%.2f");
            changed |= ui::PropertyInt("シード", &layer.scatter.seed, 0, 9999,
                                       scatterDefaults.seed, "配置と個体差の種");
            ui::EndPropertyTable();
        }

        ui::SectionHeader("個体");
        if (ui::BeginPropertyTable("scatterInstanceRows")) {
            changed |= ui::PropertyFloat("最小サイズ", &layer.scatter.sizeMinMeters, 0.1f, 200.0f,
                                         scatterDefaults.sizeMinMeters, "個体の直径（m）",
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("最大サイズ", &layer.scatter.sizeMaxMeters, 0.1f, 200.0f,
                                         scatterDefaults.sizeMaxMeters, "個体の直径（m）",
                                         "%.1f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("高さ", &layer.scatter.heightMeters, 0.0f, 100.0f,
                                         scatterDefaults.heightMeters,
                                         "地形に盛り上げる高さ（m）。0 でも Mask は出る",
                                         "%.2f m");
            changed |= ui::PropertyFloat("高さのばらつき", &layer.scatter.heightJitter, 0.0f, 1.0f,
                                         scatterDefaults.heightJitter, "個体ごとの高さの差",
                                         "%.2f");
            changed |= ui::PropertyFloat("向きのばらつき", &layer.scatter.rotationVariation, 0.0f,
                                         1.0f, scatterDefaults.rotationVariation,
                                         "個体ごとの回転の差", "%.2f");
            changed |= ui::PropertyFloat("細長さ", &layer.scatter.aspectVariation, 0.0f, 1.0f,
                                         scatterDefaults.aspectVariation,
                                         "個体ごとの縦横比の差。0 で真円", "%.2f");
            changed |= ui::PropertyFloat(
                "なめらかさ", &layer.scatter.smoothness, 0.0f, 1.0f, scatterDefaults.smoothness,
                "重なった所の溶け方。0 で折り目が立ち、上げるほど溶け合う", "%.2f");
            ui::EndPropertyTable();
        }
        ui::HintText("Mask 入力で散布する範囲を絞れる（明るい所ほど置かれる）");
        ui::HintText("Mask 出力は分布、Unique は個体ごとの乱数。"
                     "高さ 0 のまま Mask だけを使ってもよい");
        return changed;
    }

    // 水滴侵食も合成レイヤーではなく「水滴で削って運んで積む加工」。マスク入力は持たない。
    if (layer.kind == compositor::LayerKind::Droplet) {
        const compositor::MaterialLayer::DropletSettings dropletDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("dropletBasicRows")) {
            char dropletName[128] = {};
            std::snprintf(dropletName, sizeof(dropletName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", dropletName, sizeof(dropletName))) {
                layer.name = dropletName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("水滴");
        if (ui::BeginPropertyTable("dropletFlowRows")) {
            changed |= ui::PropertyFloat(
                "密度", &layer.droplet.dropletDensity, 0.01f, 4.0f, dropletDefaults.dropletDensity,
                "水滴の数。解析グリッドのセルあたりで持つので、同じ密度なら解像度を変えても"
                "同じくらいの量になる。上げるほど水系が密になり、重くなる",
                "%.2f /セル", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "移動距離", &layer.droplet.travelMeters, 16.0f, 4096.0f,
                dropletDefaults.travelMeters, "1 滴が進む距離。長いほど谷が下流まで繋がる",
                "%.0f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "慣性", &layer.droplet.inertia, 0.0f, 0.99f, dropletDefaults.inertia,
                "0 で傾斜どおりに曲がり、1 に近いほど前の向きを保つ（谷が真っ直ぐになる）",
                "%.2f");
            changed |= ui::PropertyFloat(
                "蒸発", &layer.droplet.evaporationPerMeter, 0.0f, 0.05f,
                dropletDefaults.evaporationPerMeter,
                "1 m 進むごとに失う水の割合。水が減ると運べる量も減り、途中で積み始める",
                "%.4f", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("重力", &layer.droplet.gravity, 0.0f, 20.0f,
                                         dropletDefaults.gravity,
                                         "下りでの加速。速いほど多く運べる", "%.1f");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("削る / 積む");
        if (ui::BeginPropertyTable("dropletCarveRows")) {
            changed |= ui::PropertyFloat(
                "侵食", &layer.droplet.erosionStrength, 0.0f, 1.0f,
                dropletDefaults.erosionStrength, "1 歩で削る割合（容量の不足ぶんに掛ける）",
                "%.2f");
            changed |= ui::PropertyFloat(
                "堆積", &layer.droplet.depositionStrength, 0.0f, 1.0f,
                dropletDefaults.depositionStrength, "容量を超えた土砂を捨てる割合", "%.2f");
            changed |= ui::PropertyFloat(
                "容量", &layer.droplet.sedimentCapacity, 0.1f, 20.0f,
                dropletDefaults.sedimentCapacity,
                "運べる量の係数（傾斜 × 速度 × 水量に掛ける）。大きいほど深く削る", "%.1f",
                ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat(
                "最小傾斜", &layer.droplet.minSlope, 0.0001f, 0.1f, dropletDefaults.minSlope,
                "平坦な所でも運べるようにする傾斜の下限", "%.4f",
                ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::SectionHeader("計算");
        if (ui::BeginPropertyTable("dropletComputeRows")) {
            int dropletResolutionIndex = 2;
            for (int i = 0; i < IM_ARRAYSIZE(kSedimentResolutionValues); ++i) {
                if (kSedimentResolutionValues[i] == layer.droplet.resolution) {
                    dropletResolutionIndex = i;
                }
            }
            if (ui::PropertyCombo("解像度", &dropletResolutionIndex, kSedimentResolutionLabels,
                                  IM_ARRAYSIZE(kSedimentResolutionLabels), 2,
                                  "水滴を流すグリッド。合成解像度とは別。結果は合成解像度には"
                                  "依らないが、ここを変えると細かい枝が変わる")) {
                layer.droplet.resolution = kSedimentResolutionValues[dropletResolutionIndex];
                changed = true;
            }
            changed |= ui::PropertyBool(
                "マルチグリッド", &layer.droplet.multigrid, dropletDefaults.multigrid,
                "64² から倍々に上げて流す。粗いレベルが大きな谷を、細かいレベルが枝を決める");
            changed |= ui::PropertyInt(
                "反復", &layer.droplet.iterations, 1, 200, dropletDefaults.iterations,
                "水滴を分けて流す回数。掘れた谷を次の反復が見るので、多いほど水系が育つ");
            changed |= ui::PropertyInt("シード", &layer.droplet.seed, 0, 1000000,
                                       dropletDefaults.seed, "水滴の落とし方の乱数");
            ui::EndPropertyTable();
        }
        ui::HintText("水滴を落として斜面を下らせ、運べる量より少なければ削り、多ければ積む"
                     "（terrain-editor の Droplet Erosion）。Flow は水の通った量（谷筋に砂利）、"
                     "Deposit は積もった量（谷底や扇状地に土）。地形の変更は差分で足すので、"
                     "素材の凹凸は壊さない");
        return changed;
    }

    // 崩落も合成レイヤーではなく「下地のハイトへ岩屑を積む加工」。
    // 発生源は Mask 入力（Emission）で受けるので、マスクの節は出さない。
    if (layer.kind == compositor::LayerKind::Crumbling) {
        const compositor::MaterialLayer::CrumblingSettings crumblingDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("crumblingBasicRows")) {
            char crumblingName[128] = {};
            std::snprintf(crumblingName, sizeof(crumblingName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", crumblingName, sizeof(crumblingName))) {
                layer.name = crumblingName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("崩落");
        if (ui::BeginPropertyTable("crumblingRows")) {
            changed |= ui::PropertyFloat(
                "岩屑の量", &layer.crumbling.amount, 0.0f, 1.0f, crumblingDefaults.amount,
                "生む岩片の数と、盛り上がりの強さに効く", "%.2f");
            changed |= ui::PropertyFloat("最小サイズ", &layer.crumbling.sizeMinMeters, 0.1f,
                                         100.0f, crumblingDefaults.sizeMinMeters,
                                         "岩片の直径の下限", "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("最大サイズ", &layer.crumbling.sizeMaxMeters, 0.1f,
                                         100.0f, crumblingDefaults.sizeMaxMeters,
                                         "岩片の直径の上限", "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
            int style = static_cast<int>(layer.crumbling.style);
            if (ui::PropertyCombo("形", &style, kRockStyleLabels, IM_ARRAYSIZE(kRockStyleLabels),
                                  static_cast<int>(crumblingDefaults.style),
                                  "岩片の輪郭。丸い / 多面体 / 尖った破片")) {
                layer.crumbling.style = static_cast<compositor::RockStyle>(style);
                changed = true;
            }
            changed |= ui::PropertyInt("歩数", &layer.crumbling.physicsCount, 0, 512,
                                       crumblingDefaults.physicsCount,
                                       "岩片を下へ進めるステップ数。"
                                       "大きいほど斜面の下まで流れる");
            changed |= ui::PropertyFloat(
                "重力", &layer.crumbling.gravity, 0.0f, 1.0f, crumblingDefaults.gravity,
                "低い方へ向かう強さ。高いほど一直線に下る", "%.2f");
            changed |= ui::PropertyFloat(
                "散らばり", &layer.crumbling.spread, 0.0f, 1.0f, crumblingDefaults.spread,
                "進行方向から横へ逸れる強さ。上げると筋状の重なりがほぐれる", "%.2f");
            changed |= ui::PropertyInt("シード", &layer.crumbling.seed, 0, 9999,
                                       crumblingDefaults.seed,
                                       "発生位置とばらつきの種");
            ui::EndPropertyTable();
        }
        ui::HintText("発生源（Emission 入力）の明るい所から岩片を生み、斜面を下らせて積む。"
                     "Mask は岩屑の厚み、Unique は岩片ごとの乱数を出す");
        return changed;
    }

    // ブラーは合成レイヤーではなく「下地のハイトをぼかす加工」。
    // 色もハイトのソースもマスクも持たないので、専用の行だけを出す。
    if (layer.kind == compositor::LayerKind::Blur) {
        const compositor::MaterialLayer& blurDefaults = kDefaultBlurLayer;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("blurBasicRows")) {
            char blurName[128] = {};
            std::snprintf(blurName, sizeof(blurName), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", blurName, sizeof(blurName))) {
                layer.name = blurName;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("ぼかし");
        if (ui::BeginPropertyTable("blurRows")) {
            // 半径は実寸（m）。合成解像度を変えても効きが変わらない。
            changed |= ui::PropertyFloat("半径", &layer.blur.radiusMeters, 0.0f, 512.0f,
                                         blurDefaults.blur.radiusMeters,
                                         "ぼかす範囲（m）。大きいほど広くならす", "%.2f m",
                                         ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyFloat("強さ", &layer.blur.strength, 0.0f, 1.0f,
                                         blurDefaults.blur.strength,
                                         "元の高さとぼかした高さを混ぜる量。1 で完全なぼかし",
                                         "%.2f");
            changed |= ui::PropertyInt("反復", &layer.blur.iterations, 1, 16,
                                       blurDefaults.blur.iterations,
                                       "ぼかしを重ねる回数。多いほど広く均される"
                                       "（実効半径はおよそ 半径 x sqrt(反復)）");
            ui::EndPropertyTable();
        }
        ui::HintText("下地のハイトをぼかし、ぼかした形から法線を作り直す。"
                     "素材の法線ディテールを載せるなら、このノードより後ろに繋ぐ");
        return changed;
    }

    ui::SectionHeader("基本");
    if (ui::BeginPropertyTable("layerBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", layer.name.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer))) {
            layer.name = nameBuffer;
            // 名前もアンドゥの対象。落とすと、次のアンドゥで改名まで巻き戻る。
            changed = true;
        }
        // マテリアルを割り当てているときは、見た目はマテリアル側の値で決まる。
        // 同じ意味の値を 2 か所に置くと、どちらが効いているのか分からなくなる。
        // シェイプは色を書かないので、色とサーフェスの行そのものを出さない。
        const bool hasMaterial = (layer.material != compositor::kNoMaterialAsset);
        if (!isShape && !hasMaterial) {
            changed |= ui::PropertyColorLinear("ベースカラー", &layer.baseColor.x,
                                               &defaults.baseColor.x);
            changed |= ui::PropertyFloat("ラフネス", &layer.roughness, 0.0f, 1.0f,
                                         defaults.roughness, nullptr, "%.2f");
            changed |= ui::PropertyFloat("メタルネス", &layer.metallic, 0.0f, 1.0f,
                                         defaults.metallic, nullptr, "%.2f");
            changed |= ui::PropertyFloat("AO", &layer.ambientOcclusion, 0.0f, 1.0f,
                                         defaults.ambientOcclusion, nullptr, "%.2f");
        }
        if (!isShape && !isLiquid) {
            changed |= ui::PropertyFloat("UV スケール", &layer.uvScale, 0.25f, 16.0f,
                                         defaults.uvScale,
                                         "このレイヤーの模様を何回並べるか", "%.2f", 0, 0.25f);
        }
        ui::EndPropertyTable();
    }
    if (!isShape && layer.material != compositor::kNoMaterialAsset) {
        ui::HintText("色とサーフェスの値はマテリアル側で決まる");
    }

    if (isLiquid) {
        // 水位は絶対値で、下地の高さと比べて「低い所」にだけ水が張る。
        // 重みは水位と下地の差だけで決まるので、水位を動かしても下地は変形しない。
        ui::SectionHeader("水面");
        if (ui::BeginPropertyTable("layerLiquidRows")) {
            changed |= ui::PropertyFloat("水位", &layer.heightBase, 0.0f, 1.0f,
                                         defaults.heightBase,
                                         "この高さより低い所に水面が張る", "%.2f");
            changed |= ui::PropertyFloat("フェザー", &layer.blendRange, 0.0f, 0.2f,
                                         defaults.blendRange,
                                         "汀線の柔らかさ。0 に近いほど硬い水際になる", "%.3f");
            ui::EndPropertyTable();
        }
    } else {
        ui::SectionHeader("ハイト");
        if (ui::BeginPropertyTable("layerHeightRows")) {
            int heightSource = static_cast<int>(layer.heightSource);
            if (ui::PropertyCombo("ソース", &heightSource, kValueSourceLabels,
                                  IM_ARRAYSIZE(kValueSourceLabels),
                                  static_cast<int>(defaults.heightSource))) {
                layer.heightSource = static_cast<compositor::ValueSource>(heightSource);
                changed = true;
            }
            if (isShape) {
                // シェイプはマテリアルを持たないので、ハイトマップは
                // レイヤー直結のスロットから読む（マスクの画像と同じ作法）。
                if (layer.heightSource == compositor::ValueSource::Texture) {
                    changed |= DrawMapSlotRow("画像", layer.heightTexture, m_textureLibrary);
                }
                // シェイプの基準の高さは「全体の持ち上げ」。0.5 で変化なし。
                changed |= ui::PropertyFloat("持ち上げ", &layer.heightBase, 0.0f, 1.0f,
                                             defaults.heightBase,
                                             "0.5 で変化なし。上げると全体が盛り上がり、"
                                             "下げると沈む",
                                             "%.2f");
            } else {
                changed |= ui::PropertyFloat(
                    "基準の高さ", &layer.heightBase, -2.0f, 2.0f, defaults.heightBase,
                    "このレイヤーが「溜まる水位」。下地の高さと比べて勝敗が決まる。"
                    "起伏の強さを変えてもここは動かない",
                    "%.2f");
            }
            // ソース（Heightmap）は画像の 0〜1 がそのままハイトの全幅（強さ 1.0 固定）。
            // 振れ幅は下の「スケール」の標高差（m）が決めるので、二重に持たせない。
            if (layer.heightSource != compositor::ValueSource::Constant && !isSource) {
                changed |= ui::PropertyFloat("起伏の強さ", &layer.heightGain, 0.0f, 3.0f,
                                             defaults.heightGain,
                                             "基準の高さを中心とした凹凸の振れ幅。0 で平らになる",
                                             "%.2f");
            }
            if (layer.heightSource == compositor::ValueSource::Noise) {
                changed |= DrawNoiseRows(layer.heightNoise, defaults.heightNoise, false);
            }
            ui::EndPropertyTable();
        }
        if (isSource) {
            // ハイトは 0〜1 の正規化値。**何 m かは下の「スケール」の標高差**が決める。
            ui::HintText("画像の 0〜1 がハイトの全幅。実際の高さ（m）はスケールの標高差で決まる");
        } else if (isShape) {
            ui::HintText("下地の高さへ加算し、0〜1 に切り詰める。細部は下のレイヤーのまま残る");
        }
    }

    // マスクは「下地と競合させるための不透明度」なので、
    // 入力を持たないソース（ハイトマップ）では意味を持たない。行ごと出さない。
    if (!isSource) {
        ui::SectionHeader("マスク");
        if (ui::BeginPropertyTable("layerMaskRows")) {
            // Mask 入力にノードが繋がっているときは、そちらが出どころ。
            if (maskFromNode) {
                ui::PropertyValue("ソース", "%s", "画像（Mask 入力）");
            } else {
                int maskSource = static_cast<int>(layer.mask.source);
                if (ui::PropertyCombo("ソース", &maskSource, kMaskSourceLabels,
                                      IM_ARRAYSIZE(kMaskSourceLabels),
                                      static_cast<int>(kDefaultLayer.mask.source),
                                      "マスクは被覆率。1.0 で全面を覆い、"
                                      "中間はこのレイヤーの起伏の高い所から順に出る")) {
                    layer.mask.source = static_cast<compositor::MaskSource>(maskSource);
                    changed = true;
                }
            }
            changed |= ui::PropertyFloat("定数", &layer.mask.constant, 0.0f, 1.0f,
                                         kDefaultLayer.mask.constant,
                                         "ソースの値に掛ける係数", "%.2f");

            if (!maskFromNode && layer.mask.source == compositor::MaskSource::Texture) {
                changed |= DrawMapSlotRow("画像", layer.mask.texture, m_textureLibrary);
            }
            if (!maskFromNode && layer.mask.source == compositor::MaskSource::Noise) {
                changed |= DrawNoiseRows(layer.mask.noise, kDefaultLayer.mask.noise);
            }
            if (compositor::IsDerivedMaskSource(layer.mask.source)) {
                changed |= ui::PropertyFloat("強調", &layer.mask.derivedScale, 0.0f, 8.0f,
                                             kDefaultLayer.mask.derivedScale,
                                             "下地から作った値の効き方", "%.2f");
            }

            changed |= ui::PropertyFloat("カーブ", &layer.mask.contrast, 0.0f, 4.0f,
                                         kDefaultLayer.mask.contrast,
                                         "1 で線形。大きいほど境界がはっきりする", "%.2f");
            changed |= ui::PropertyFloat("レベル下限", &layer.mask.levelsLow, 0.0f, 1.0f,
                                         kDefaultLayer.mask.levelsLow, nullptr, "%.2f");
            changed |= ui::PropertyFloat("レベル上限", &layer.mask.levelsHigh, 0.0f, 1.0f,
                                         kDefaultLayer.mask.levelsHigh, nullptr, "%.2f");
            changed |= ui::PropertyBool("反転", &layer.mask.invert, kDefaultLayer.mask.invert);
            ui::EndPropertyTable();
        }

        if (isBase) {
            ui::HintText("一番下のレイヤーは下地なのでマスクは効かない");
        }
        switch (layer.mask.source) {
            case compositor::MaskSource::Slope:
                ui::HintText("急な面ほど 1 に近づく");
                break;
            case compositor::MaskSource::Curvature:
                ui::HintText("0.5 が平坦。凸で大、凹で小");
                break;
            case compositor::MaskSource::Cavity:
                ui::HintText("窪んでいるほど 1 に近づく");
                break;
            case compositor::MaskSource::Height:
                ui::HintText("下地が高いほど 1 に近づく");
                break;
            default:
                break;
        }
    }

    if (layer.mask.source == compositor::MaskSource::Paint) {
        ui::SectionHeader("ペイント");
        changed |= DrawPaintSection(layer);
    }

    // マテリアルの割り当てと合成の調整はサーフェスだけのもの。
    // シェイプは Normal / Height を加算で書くと決まっており、
    // 水面は水位とフェザーが合成のすべてなので、出しても意味を持たない。
    if (!isShape && !isLiquid) {
        ui::SectionHeader("マテリアル");
        if (ui::BeginPropertyTable("layerMaterialRows")) {
            changed |= DrawMaterialSlotRow("マテリアル", layer.material, m_materialLibrary);
            ui::EndPropertyTable();
        }
        if (const compositor::MaterialAsset* material = m_materialLibrary.Find(layer.material);
            material != nullptr && material->thumbnail.IsValid()) {
            ImGui::Image(static_cast<ImTextureID>(material->thumbnail.srv.gpu.ptr),
                         ImVec2(ui::Scaled(72.0f), ui::Scaled(72.0f)));
        } else {
            ui::HintText("マテリアルパネルで作って割り当てる");
        }

        ui::SectionHeader("合成");
        if (ui::BeginPropertyTable("layerBlendRows")) {
            changed |= ui::PropertyFloat("境界の柔らかさ", &layer.blendRange, 0.0f, 1.0f,
                                         defaults.blendRange,
                                         "0 に近いほど硬い置き換えになる", "%.2f");
            changed |= ui::PropertyBool("下地に沿わせる", &layer.wrapToUnderlying,
                                        defaults.wrapToUnderlying,
                                        "下地の形を保ったまま表面を被せる（コーティング）。"
                                        "基準の高さの 0.5 からのずれが被せ物の厚みになる");

            ui::PropertyLabel("書き込み", "このレイヤーが書き込むチャンネル");
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
        ui::HintText("このレイヤーにはまだペイントマスクがない");
        if (ui::Button("マスクを作成", ui::kWideButtonWidth)) {
            layer.mask.paint = m_paintMasks.Add(m_device, 0.0f);
            m_paintMode = (layer.mask.paint != compositor::kNoPaintMask);
            changed = true;
        }
        return changed;
    }

    if (ui::BeginPropertyTable("layerPaintRows")) {
        ui::PropertyBool("ペイントモード", &m_paintMode, false,
                         "オンの間、ビューポートのドラッグがブラシになる");
        ui::PropertyFloat("ブラシ半径", &m_brush.radiusPixels, 4.0f, 256.0f,
                          kDefaultBrush.radiusPixels,
                          "画面上の半径。視点や UV スケールを変えても見た目の大きさは変わらない",
                          "%.0f px");
        ui::PropertyFloat("強さ", &m_brush.strength, 0.01f, 1.0f, kDefaultBrush.strength,
                          "1 回の適用で足す量", "%.2f");
        ui::PropertyFloat("減衰", &m_brush.falloff, 0.2f, 8.0f, kDefaultBrush.falloff,
                          "1 で線形。大きいほど中心に集中する", "%.2f");
        ui::PropertyBool("消しゴム", &m_brush.erase, kDefaultBrush.erase,
                         "左右のドラッグの意味を入れ替える");

        ui::PropertyLabelEmpty("paintFill");
        if (ui::Button("全消去")) {
            m_paintMasks.QueueSnapshot(m_device, layer.mask.paint);
            m_paintMasks.QueueFill(layer.mask.paint, 0.0f);
        }
        ImGui::SameLine();
        if (ui::Button("全塗り")) {
            m_paintMasks.QueueSnapshot(m_device, layer.mask.paint);
            m_paintMasks.QueueFill(layer.mask.paint, 1.0f);
        }
        ui::PropertyEnd();

        ui::PropertyLabelEmpty("paintHistory");
        ImGui::BeginDisabled(!m_paintMasks.CanUndo());
        if (ui::Button("アンドゥ")) {
            m_paintMasks.QueueUndo(m_device);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_paintMasks.CanRedo());
        if (ui::Button("リドゥ")) {
            m_paintMasks.QueueRedo(m_device);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("(%zu 段)", m_paintMasks.UndoCount());
        ui::PropertyEnd();

        // 解像度はすべてのペイントマスクで共通。
        int resolution = ResolutionIndex(m_paintMasks.RequestedResolution());
        if (ui::PropertyCombo("解像度", &resolution, kResolutionLabels,
                              IM_ARRAYSIZE(kResolutionLabels), 1,
                              "全ペイントマスクを拡大縮小する。履歴は破棄される")) {
            m_paintMasks.RequestResolution(kResolutionValues[resolution]);
        }

        ui::PropertyLabelEmpty("paintDiscard");
        if (ui::Button("マスクを破棄", ui::kWideButtonWidth)) {
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

    ui::HintText("左ドラッグで塗る / 右ドラッグで消す / Alt + 左ドラッグで視点を回す");
    return changed;
}

}  // namespace tg
