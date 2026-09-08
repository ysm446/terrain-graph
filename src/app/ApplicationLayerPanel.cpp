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
                                         "地形へ降らせる雪の総量。0 なら入力をそのまま通す。"
                                         "Mask 入力を繋ぐとその明るさに比例して降らせる"
                                         "（繋がなければ全面へ一様）",
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
        ui::HintText("雪を降らせ、急な雪面から低い所へ滑らせて溜める。"
                     "Mask 入力で降らせる場所を絞れる（Mask Height を繋げば雪線になる。"
                     "滑った雪はマスクの外へも出る）。"
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
    if (layer.kind == compositor::LayerKind::FluvialErosion) {
        auto& params = layer.fluvialErosion;
        const compositor::MaterialLayer::FluvialErosionSettings erosionDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("fluvialErosionRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", name, sizeof(name))) { layer.name = name; changed = true; }
            const char* labels[] = {"64", "128", "256", "512", "1024", "2048"};
            int selected = 0, defaultIndex = 0;
            for (int i = 0; i < 6; ++i) {
                if ((64u << i) == params.resolution) selected = i;
                if ((64u << i) == erosionDefaults.resolution) defaultIndex = i;
            }
            if (ui::PropertyCombo("計算解像度", &selected, labels, 6, defaultIndex, "侵食を計算する格子。合成解像度とは独立")) {
                params.resolution = 64u << selected; changed = true;
            }
            changed |= ui::PropertyInt("反復回数", &params.iterations, 0, 100, erosionDefaults.iterations, "");
            changed |= ui::PropertyFloat("地形の特徴サイズ", &params.featureSize, 0.0f, 32.0f, erosionDefaults.featureSize, "");
            changed |= ui::PropertyFloat("地質年代", &params.geologicalAge, 0.0f, 20.0f, erosionDefaults.geologicalAge, "");
            changed |= ui::PropertyFloat("流路の長さ", &params.channelLength, 0.0f, 512.0f, erosionDefaults.channelLength, "");
            changed |= ui::PropertyFloat("侵食の強さ", &params.strength, 0.0f, 1.0f, erosionDefaults.strength, "");
            changed |= ui::PropertyFloat("流路の掘り込み", &params.channeling, 0.0f, 1.0f, erosionDefaults.channeling, "");
            changed |= ui::PropertyFloat("摩擦", &params.friction, 0.0f, 1.0f, erosionDefaults.friction, "");
            changed |= ui::PropertyFloat("侵食開始角度", &params.wearAngle, 0.0f, 89.0f, erosionDefaults.wearAngle, "");
            changed |= ui::PropertyFloat("堆積停止角度", &params.depositAngle, 0.0f, 89.0f, erosionDefaults.depositAngle, "");
            changed |= ui::PropertyFloat("侵食上限角度", &params.maxAngle, 0.0f, 89.0f, erosionDefaults.maxAngle, "");
            changed |= ui::PropertyFloat("粒度", &params.granularity, 0.0f, 100.0f, erosionDefaults.granularity, "");
            changed |= ui::PropertyFloat("侵食履歴の影響", &params.flowVolume, 0.0f, 1.0f, erosionDefaults.flowVolume, "");
            changed |= ui::PropertyFloat("細い流路の影響", &params.smallChannels, 0.0f, 1.0f, erosionDefaults.smallChannels, "");
            changed |= ui::PropertyFloat("移動速度", &params.velocity, 0.0f, 2.0f, erosionDefaults.velocity, "");
            changed |= ui::PropertyFloat("基準のセル幅", &params.detailMeters, 0.1f, 32.0f, erosionDefaults.detailMeters, "");
            changed |= ui::PropertyFloat("侵食部分の平滑化", &params.detailSmoothing, 0.0f, 10.0f, erosionDefaults.detailSmoothing, "");
            changed |= ui::PropertyFloat("外力 X", &params.forceX, -1.0f, 1.0f, erosionDefaults.forceX, "");
            changed |= ui::PropertyFloat("外力 Z", &params.forceZ, -1.0f, 1.0f, erosionDefaults.forceZ, "");
            changed |= ui::PropertyFloat("せん断 X", &params.shearX, -0.1f, 0.1f, erosionDefaults.shearX, "");
            changed |= ui::PropertyFloat("せん断 Z", &params.shearZ, -0.1f, 0.1f, erosionDefaults.shearZ, "");
            changed |= ui::PropertyFloat("硬度", &params.hardness, 0.0f, 1.0f, erosionDefaults.hardness, "Hardness 入力と合わせて使う。1 で地形を保護する");
        ui::EndPropertyTable();
        }
        return changed;
    }

    if (layer.kind == compositor::LayerKind::FlattenBorders) {
        auto& params = layer.flattenBorders;
        const compositor::MaterialLayer::FlattenBordersSettings erosionDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("flattenBordersRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", name, sizeof(name))) { layer.name = name; changed = true; }
            changed |= ui::PropertyFloat("外周の幅", &params.falloffMeters, 0.0f, 2048.0f, erosionDefaults.falloffMeters, "");
            changed |= ui::PropertyFloat("外周の標高", &params.elevationMeters, -1000.0f, 4000.0f, erosionDefaults.elevationMeters, "");
            changed |= ui::PropertyFloat("地形の持ち上げ", &params.liftMeters, -1000.0f, 4000.0f, erosionDefaults.liftMeters, "");
            changed |= ui::PropertyFloat("強さ", &params.strength, 0.0f, 1.0f, erosionDefaults.strength, "");
            changed |= ui::PropertyBool("X 下側", &params.lowerX, erosionDefaults.lowerX);
            changed |= ui::PropertyBool("X 上側", &params.upperX, erosionDefaults.upperX);
            changed |= ui::PropertyBool("Z 下側", &params.lowerZ, erosionDefaults.lowerZ);
            changed |= ui::PropertyBool("Z 上側", &params.upperZ, erosionDefaults.upperZ);
        ui::EndPropertyTable();
        }
        return changed;
    }

    if (layer.kind == compositor::LayerKind::MultiScaleErosion) {
        auto& params = layer.multiScaleErosion;
        const compositor::MaterialLayer::MultiScaleErosionSettings mseDefaults;
        ui::SectionHeader("基本");
        if (ui::BeginPropertyTable("multiScaleBasicRows")) {
            char name[128] = {};
            std::snprintf(name, sizeof(name), "%s", layer.name.c_str());
            if (ui::PropertyTextInput("名前", name, sizeof(name))) {
                layer.name = name;
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader("段階と侵食量");
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
                                      "粗い解像度から倍々に上げて計算する。合成解像度とは独立")) {
                    value = resolutions[selected];
                    changed = true;
                }
            };
            resolutionRow("開始解像度", params.baseResolution, mseDefaults.baseResolution);
            resolutionRow("最終解像度", params.resolution, mseDefaults.resolution);
            params.baseResolution = std::min(params.baseResolution, params.resolution);
            changed |= ui::PropertyFloat("最大侵食深さ", &params.coarseDepthMeters, 0.0f, 100.0f,
                mseDefaults.coarseDepthMeters, "最も粗い段階で河川侵食が削れる深さの上限。崩落と堆積は別", "%.2f m");
            changed |= ui::PropertyFloat("細部の強さ", &params.detailDecay, 0.0f, 1.0f,
                mseDefaults.detailDecay, "解像度を倍にするたび、最大侵食深さに掛ける割合", "%.2f");
            changed |= ui::PropertyInt("侵食の反復", &params.erosionIterations, 0, 4000,
                mseDefaults.erosionIterations, "各段階の反復数。深さの上限は変えず、水系を育てる。0 で河川侵食なし");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("水の流れ");
        if (ui::BeginPropertyTable("multiScaleFlowRows")) {
            changed |= ui::PropertyFloat("流れの集中", &params.flowExponent, 1.0f, 8.0f,
                mseDefaults.flowExponent, "大きいほど急な方向へ集中する。論文の既定は 1.3", "%.2f");
            changed |= ui::PropertyFloat("傾斜の指数", &params.slopeExponent, 0.1f, 4.0f,
                mseDefaults.slopeExponent, "侵食の強さに対する傾斜の効き方", "%.2f");
            changed |= ui::PropertyFloat("集水の指数", &params.drainageExponent, 0.1f, 2.0f,
                mseDefaults.drainageExponent, "侵食の強さに対する集水面積の効き方", "%.2f");
            changed |= ui::PropertyFloat("傾斜の上限", &params.maximumSlope, 0.01f, 10.0f,
                mseDefaults.maximumSlope, "これより急でも河川侵食は強くならない。1 で 45 度", "%.2f");
            changed |= ui::PropertyFloat("集水の上限", &params.maximumDrainageArea, 1.0f, 1000000.0f,
                mseDefaults.maximumDrainageArea,
                "これより水が集まっても河川侵食は強くならない。開始時のセル面積"
                "（地形の一辺 ÷ 開始解像度）の二乗より小さいと、尾根と谷の差が出なくなる",
                "%.0f m²",
                ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::SectionHeader("斜面と堆積");
        if (ui::BeginPropertyTable("multiScaleSedimentRows")) {
            changed |= ui::PropertyFloat("安息角", &params.talusDegrees, 1.0f, 85.0f,
                mseDefaults.talusDegrees, "これより急な隣接斜面から土砂を下へ動かす", "%.1f 度");
            changed |= ui::PropertyFloat("土砂の移動量", &params.thermalStepMeters, 0.0f, 0.1f,
                mseDefaults.thermalStepMeters, "1 反復で隣接セルへ移す高さ。セル幅の 1% を上限にする", "%.4f m");
            changed |= ui::PropertyInt("斜面の反復", &params.thermalIterations, 0, 1000,
                mseDefaults.thermalIterations, "河川侵食の後に斜面を安定させる回数。0 で無効");
            changed |= ui::PropertyFloat("土砂の供給", &params.sedimentCreation, 0.0f, 1.0f,
                mseDefaults.sedimentCreation, "水の侵食力に比例して供給する浮遊土砂の係数", "%.3f");
            changed |= ui::PropertyFloat("堆積率", &params.depositionRate, 0.0f, 1.0f,
                mseDefaults.depositionRate, "運べる量を超えた浮遊土砂が積もる割合", "%.3f");
            changed |= ui::PropertyFloat("堆積の高さ", &params.sedimentHeightScale, 0.0f, 0.1f,
                mseDefaults.sedimentHeightScale, "堆積する土砂の内部量を高さに換算する倍率。大きいほど厚く積もる",
                "%.4f m", ImGuiSliderFlags_Logarithmic);
            changed |= ui::PropertyInt("堆積の反復", &params.depositionIterations, 0, 2000,
                mseDefaults.depositionIterations, "斜面安定化の後に土砂を流して積む回数。0 で無効");
            ui::EndPropertyTable();
        }
        ui::SectionHeader("地形の仕上げ");
        if (ui::BeginPropertyTable("multiScaleFinishingRows")) {
            changed |= ui::PropertyFloat("稜線の高さ復元", &params.ridgeRestoration, 0.0f, 1.0f,
                mseDefaults.ridgeRestoration, "尾根・山頂を元の高さへ戻す強さ。0 で無効", "%.2f");
            changed |= ui::PropertyFloat("尾根の集水閾値", &params.ridgeAreaThreshold, 1.01f, 8.0f,
                mseDefaults.ridgeAreaThreshold, "集水面積がこのセル数未満の点を復元する。大きいほど対象が広がる", "%.2f セル");
            changed |= ui::PropertyInt("復元の反復", &params.restorationIterations, 1, 2000,
                mseDefaults.restorationIterations, "尾根の標高差を周囲へ滑らかに広げる回数");
            changed |= ui::PropertyBool("排水路補正", &params.drainageCorrection,
                mseDefaults.drainageCorrection, "窪地から外周へ排水路を掘る。湖を残したい場合は無効にする");
            changed |= ui::PropertyInt("排水路の補正幅", &params.breachingRadius, 1, 64,
                mseDefaults.breachingRadius, "最終グリッドのセル数。広い幅から半分ずつ狭める半径。1 で細い流路のみ");
            ui::EndPropertyTable();
        }
        ui::HintText("粗い地形から段階的に谷筋を作る。開始解像度より細かな元の凹凸は再構成される。"
                     "Mask は結果を適用する範囲（白で適用）。");
        return changed;
    }
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
