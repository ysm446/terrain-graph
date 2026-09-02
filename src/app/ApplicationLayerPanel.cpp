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
                                   bool maskFromNode) {
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
                                         "全体へ上乗せする土砂の厚み（m）", "%.2f m");
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
                                  "土砂を動かすグリッド。**合成解像度とは別**。"
                                  "上げるほど細かい筋が出るが、反復のコストも比例して増える")) {
                layer.sediment.resolution = kSedimentResolutionValues[resolutionIndex];
                changed = true;
            }
            ui::EndPropertyTable();
        }
        ui::HintText("重力で土砂を再分配する。谷底に厚く積もり、尾根は痩せる。"
                     "動いたぶんだけを下地のハイトへ足すので、細部は残る");
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
                                      "マスクは不透明度として高さと同じ土俵で競合する。"
                                      "1.0 にすると高さに関係なく全面を覆う")) {
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
