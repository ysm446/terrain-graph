// プレビュー設定パネルと「ライティング」パネル。
// どちらも合成結果ではなく、見え方（レンダラ側の設定）を扱う。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
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

void Application::DrawMaterialPanel() {
    // **ここでは前面を要求しない。** レイヤーと同じ枠のタブなので、
    // 両方が要求すると後から描いたほうが勝ち、既定の前面が定まらない。
    if (ImGui::Begin("プレビュー設定")) {
        if (ui::BeginPropertyTable("previewRows")) {
            const renderer::PreviewDefaults& defaults = renderer::kPreviewDefaults;
            ui::PropertyBool("合成結果", &m_renderer.UseMaterialTextures(),
                             defaults.useMaterialTextures,
                             "オフにすると、レイヤー合成を使わず単色マテリアルで表示する");
            // マスクを見ているときだけ効く。行は常に出す（表示の好みなので）。
            ui::PropertyBool("マスクの飽和に斜線", &m_renderer.MaskSaturationHatch(),
                             defaults.maskSaturationHatch,
                             "マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引く。"
                             "濃淡が付いている所と、上限に当たって潰れた所を見分けられる");

            // 平面の大きさ（m）。**ジオメトリだけがメートル**で、
            // テクスチャは無次元のまま（1 UV が何 m かは決めない）。
            //
            // **グラフに Heightmap ノードがあるときは、その実寸に従う。**
            // 地形の大きさは見え方の設定ではなく読み込んだデータの性質なので、
            // 決める場所はノード側の 1 か所だけにする。
            const bool scaleFromGraph = (m_graph.FindChainScale(m_previewGraphNode) != nullptr);
            if (!scaleFromGraph) {
                ui::PropertyFloat("平面のサイズ", &m_renderer.PlaneSize(), 0.5f, 8192.0f,
                                  defaults.planeSize,
                                  "平面の一辺の長さ（m）。素材は 2m 前後、"
                                  "地形なら 1000m 以上。カメラと影の範囲もこれに追従する",
                                  "%.1f m", ImGuiSliderFlags_Logarithmic);
            }

            if (m_renderer.UseMaterialTextures()) {
                // 上限は「平面の辺の半分」。素材（2m 角）なら 1m、
                // 地形（2km 角）なら 1000m まで指定できる。
                // ハイト 0〜1 の全幅がこの高さに対応するので、
                // 「この地形の標高差は何 m か」をそのまま入れる。
                if (scaleFromGraph) {
                    // ノードが実寸を持っているときは表示だけにする。
                    // 同じ値を 2 か所から編集できると、どちらが効くのか分からなくなる。
                    ui::PropertyValue("平面のサイズ", "%.1f m", m_renderer.PlaneSize());
                    ui::PropertyValue("変位量", "%.1f m", m_renderer.DisplacementScale());
                } else {
                    const float displacementMax =
                        std::max(1.0f, m_renderer.PlaneSize() * 0.5f);
                    ui::PropertyFloat(
                        "変位量", &m_renderer.DisplacementScale(), 0.0f, displacementMax,
                        defaults.displacementScale,
                        "ハイトを形状に反映する量（ディスプレイスメント）。"
                        "ハイト 0〜1 の全幅がこの高さ（m）になる。0 なら形は変わらない",
                        "%.2f m", 0, 0.01f);
                }

                ui::PropertyBool("テセレーション", &m_renderer.TessellationEnabled(),
                                 defaults.tessellationEnabled,
                                 "画面上の辺が長いところだけメッシュを細かく割る。"
                                 "変位量を上げたときに形がなめらかになる");
                if (m_renderer.TessellationEnabled()) {
                    ui::PropertyFloat("分割の上限", &m_renderer.TessellationFactor(), 1.0f, 16.0f,
                                      defaults.tessellationFactor,
                                      "1 辺をこの回数まで割る。上げるほど重くなる",
                                      "%.0f", 0, 1.0f);
                }

                // 形の細かさの上限。地形の一辺 ÷ 分割数 が 1 マスの大きさになる。
                // テセレーションは「画面上で辺が伸びたときだけ」割るので、
                // 引きの絵ではこの分割数がそのまま形の限界になる。
                int subdivisions = 0;
                for (int i = 0; i < IM_ARRAYSIZE(kMeshSubdivisionValues); ++i) {
                    if (kMeshSubdivisionValues[i] == m_renderer.MeshSubdivisions()) {
                        subdivisions = i;
                    }
                }
                char meshHint[128] = {};
                std::snprintf(meshHint, sizeof(meshHint),
                              "平面を何分割するか。いまは 1 マス %.1f m。"
                              "上げるほど細かい形が出るが重くなる",
                              m_renderer.PlaneSize() /
                                  static_cast<float>(m_renderer.MeshSubdivisions()));
                if (ui::PropertyCombo("メッシュ分割", &subdivisions, kMeshSubdivisionLabels,
                                      IM_ARRAYSIZE(kMeshSubdivisionLabels), 0, meshHint)) {
                    m_renderer.RequestMeshSubdivisions(kMeshSubdivisionValues[subdivisions]);
                }

                int resolution = ResolutionIndex(m_renderer.MaterialResolution());
                if (ui::PropertyCombo("合成解像度", &resolution, kResolutionLabels,
                                      IM_ARRAYSIZE(kResolutionLabels),
                                      ResolutionIndex(defaults.materialResolution),
                                      "編集中のプレビュー解像度。上げるほど細部が出るが重くなる")) {
                    m_renderer.RequestMaterialResolution(kResolutionValues[resolution]);
                }
            } else {
                renderer::MaterialSettings& material = m_renderer.Material();
                ui::PropertyColorLinear("ベースカラー", &material.baseColor.x,
                                        &kDefaultMaterial.baseColor.x);
                ui::PropertyFloat("ラフネス", &material.roughness, 0.0f, 1.0f,
                                  kDefaultMaterial.roughness, nullptr, "%.2f");
                ui::PropertyFloat("メタルネス", &material.metallic, 0.0f, 1.0f,
                                  kDefaultMaterial.metallic, nullptr, "%.2f");
            }
            ui::EndPropertyTable();
        }

        ui::SectionHeader("カメラ");
        if (ui::BeginPropertyTable("cameraRows")) {
            // 露出を絞り / シャッター / ISO で決めているので、レンズも同じ言葉で扱う。
            // ラジアンのままだと何 mm 相当なのか分からない。
            renderer::Camera& camera = m_renderer.GetCamera();
            float focalLength = renderer::FocalLengthFromFovY(camera.FovY());
            if (ui::PropertyFloat("焦点距離", &focalLength, 12.0f, 200.0f,
                                  renderer::FocalLengthFromFovY(kDefaultCamera.fovY),
                                  "35mm フルサイズ換算。小さいほど広角で、遠近が強く出る",
                                  "%.0f mm", ImGuiSliderFlags_Logarithmic, 1.0f)) {
                camera.SetFovY(renderer::FovYFromFocalLength(focalLength));
            }
            // **F 値はレンズの値なのでここに置く。** 露出（絞り）とボケの
            // どちらも同じ 1 つの値で決まる。EV を直接指定していると露出の節から
            // 絞りの行が消えるので、レンズ側に置いておかないと触れなくなる。
            ui::PropertyFloat("F 値", &m_renderer.Exposure().aperture, 1.0f, 32.0f,
                              renderer::ExposureSettings{}.aperture,
                              "絞り。小さいほどボケが強く、露出は明るくなる", "F%.1f",
                              ImGuiSliderFlags_Logarithmic);
            ui::PropertyValue("画角", "%.1f 度（垂直）", RadiansToDegrees(camera.FovY()));
            ui::PropertyLabelEmpty("cameraReset");
            if (ui::Button("視点をリセット", ui::kWideButtonWidth)) {
                m_renderer.GetCamera().Reset();
            }
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }

        // **レンズの値はここに出さない。** 焦点距離も F 値もカメラの節が持っていて、
        // すぐ上に見えている。同じ値を並べると、どちらが効いているのか分からなくなる。
        ui::SectionHeader("被写界深度");
        renderer::DofSettings& dof = m_renderer.Dof();
        const renderer::DofSettings kDefaultDof;
        if (ui::BeginPropertyTable("dofRows")) {
            ui::PropertyBool("有効", &dof.enabled, kDefaultDof.enabled,
                             "ビューポートの見え方だけに掛かる。合成結果と書き出しには効かない");

            ImGui::BeginDisabled(!dof.enabled);

            ui::PropertyBool("注視点に追従", &dof.focusOnTarget, kDefaultDof.focusOnTarget,
                             "軌道カメラなので、見ているものは常に注視点にある。"
                             "切ると距離を手で決められる");
            ImGui::BeginDisabled(dof.focusOnTarget);
            ui::PropertyFloat("ピント距離", &dof.focusDistance, 0.1f, 50.0f,
                              kDefaultDof.focusDistance,
                              "カメラからピント面まで。ワールドの 1 単位を 1m とみなす",
                              "%.2f m", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ui::PropertyValue("実効ピント距離", "%.2f m", m_renderer.FocusDistance());

            ui::PropertyFloat("ミニチュア", &dof.miniatureScale, 1.0f, 10000.0f,
                              kDefaultDof.miniatureScale,
                              "1 で実物大。上げるほど模型を撮った計算になり、"
                              "同じレンズでもボケが強くなる。実寸のままだと地形は"
                              "遠すぎて 1 画素もボケない。2km の地形を引きで見るなら "
                              "3000〜10000 が目安",
                              "1 : %.0f", ImGuiSliderFlags_Logarithmic);
            ui::PropertyFloat("ボケの強さ", &dof.blurScale, 0.25f, 16.0f, kDefaultDof.blurScale,
                              "1 で現実どおり。2m 角の地面を広角で撮れば現実でも"
                              "全域にピントが合うので、見せたい量まで持ち上げるための誇張。"
                              "倍率を使わずに出したいなら望遠へ寄せる",
                              "x%.2f", ImGuiSliderFlags_Logarithmic);
            ui::PropertyFloat("最大ぼけ", &dof.maxBlurPixels, 1.0f, 64.0f,
                              kDefaultDof.maxBlurPixels,
                              "画面上のぼけ半径の上限。現実の式のままだと極端になるので、"
                              "表示のための頭打ちとして持つ。上げるほど重くなる",
                              "%.0f px");

            static const char* const kShapeLabels[] = {"円", "三角形", "六角形", "八角形"};
            int shape = static_cast<int>(dof.shape);
            if (ui::PropertyCombo("絞りの形", &shape, kShapeLabels, IM_ARRAYSIZE(kShapeLabels),
                                  static_cast<int>(kDefaultDof.shape), "ボケの形になる")) {
                dof.shape = static_cast<renderer::ApertureShape>(shape);
            }
            ImGui::BeginDisabled(dof.shape == renderer::ApertureShape::Circle);
            ui::PropertyFloat("絞りの向き", &dof.rotationDegrees, 0.0f, 180.0f,
                              kDefaultDof.rotationDegrees, "多角形のボケの角度", "%.0f 度");
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }
        ui::HintText("ボケ量は焦点距離・F 値・ピント距離で決まる。ワールドの 1 単位 = 1m");
    }
    ImGui::End();
}

void Application::DrawLightingPanel() {
    if (ImGui::Begin("ライティング")) {
        if (ui::BeginPropertyTable("lightingModeRows")) {
            const char* modes[] = {"環境マップ (IBL)", "大気散乱スカイ"};
            int mode = m_renderer.AtmosphericMode() ? 1 : 0;
            if (ui::PropertyCombo("モード", &mode, modes, 2, 0,
                                  "環境マップは天球アセットで照らします。大気散乱スカイは太陽と大気から空を生成し、雲も描画できます。\n"
                                  "切り替えても、それぞれの天球・太陽設定は保持されます。")) m_renderer.AtmosphericMode() = mode == 1;
            ui::EndPropertyTable();
        }
        renderer::LightSettings& light = m_renderer.Light();

        ui::SectionHeader("ライト");
        if (ui::BeginPropertyTable("lightRows")) {
            float azimuthDeg = RadiansToDegrees(light.azimuth);
            if (ui::PropertyFloat("方位角", &azimuthDeg, -180.0f, 180.0f,
                                  RadiansToDegrees(kDefaultLight.azimuth),
                                  "太陽の向き（水平方向）", "%.0f 度")) {
                light.azimuth = DegreesToRadians(azimuthDeg);
            }
            float elevationDeg = RadiansToDegrees(light.elevation);
            if (ui::PropertyFloat("仰角", &elevationDeg, -89.0f, 89.0f,
                                  RadiansToDegrees(kDefaultLight.elevation),
                                  "太陽の高さ。低いほど影が伸びる", "%.0f 度")) {
                light.elevation = DegreesToRadians(elevationDeg);
            }
            ui::PropertyFloat("照度", &light.illuminance, 0.0f, 200000.0f,
                              m_renderer.AtmosphericMode() ? renderer::AtmosphereSettings{}.illuminance : kDefaultLight.illuminance,
                              m_renderer.AtmosphericMode() ? "大気圏外の照度 (lux)。地表では大気の透過率で減衰する" : "lux。晴天の直射日光がおよそ 100000 lux", "%.0f");
            if (!m_renderer.AtmosphericMode())
                ui::PropertyColorLinear("光の色", &light.color.x, &kDefaultLight.color.x);
            else ui::PropertyValue("光の色", "大気の透過率から自動計算");
            ui::PropertyBool("影", &m_renderer.ShadowEnabled(),
                             renderer::kPreviewDefaults.shadowEnabled,
                             "ディレクショナルライトの影を落とす。"
                             "ディスプレイスメントで押し出した形にも落ちる。大気散乱スカイでは雲影も切り替える");
            ui::EndPropertyTable();
        }

        if (m_renderer.AtmosphericMode()) {
            auto& sky = m_renderer.AtmosphericSettings();
            const renderer::AtmosphereSettings defaults;
            ui::SectionHeader("大気");
            if (ui::BeginPropertyTable("atmosphereRows", "グラウンドアルベド")) {
                ui::PropertyBool("背景を表示", &m_renderer.ShowSkybox(), renderer::kPreviewDefaults.showSkybox,
                                 "空の背景を表示する。環境光と地形の手前の雲は残る");
                ui::PropertyFloat("レイリー散乱強度", &sky.density, 0.1f, 3.0f, defaults.density,
                                  "空の青さや夕焼けを生む大気の散乱量。1 が基準です。\n"
                                  "大きくすると散乱と太陽光の減衰が強くなり、空の色と地形の照明が変わります。");
                ui::PropertyFloat("ミー密度", &sky.mie, 0.0f, 2.0f, defaults.mie,
                                  "ミー散乱と吸収をまとめて変える粒子密度の倍率。大きくすると太陽の周囲や地平線が白っぽく霞み、直射光が弱まります。\n"
                                  "地形を距離に応じて隠すフォグとは別の設定です。");
                ui::PropertyFloat("ミー異方性", &sky.eccentricity, 0.0f, 0.95f, defaults.eccentricity,
                                  "ミー散乱の異方性（g）。光が太陽の方向へ集中する度合い。\n"
                                  "大きいほど太陽付近の光が鋭く集中し、小さいほど広い方向へ散らばります。");
                ui::PropertyFloat("地表の基準標高", &sky.altitude, 0.0f, 10000.0f, defaults.altitude,
                                  "地形の原点の海抜高度（m）。空と環境光を計算する基準です。\n"
                                  "高くすると上空の薄い大気を通した空になります。地形やカメラ自体は移動しません。", "%.0f m");
                int lowerHemisphere = static_cast<int>(sky.lowerHemisphere);
                const char* lowerHemisphereLabels[]{"空の延長", "地面反射"};
                if (ui::PropertyCombo("下半球", &lowerHemisphere, lowerHemisphereLabels, 2,
                                      static_cast<int>(defaults.lowerHemisphere),
                                      "空の延長：青空を下半球へ折り返す初期の表現。地面反射：日光と天空光を受けた地表の反射。\n"
                                      "背景・環境光・反射・雲の環境照明に共通で適用します。空の延長は見た目のための近似です。"))
                    sky.lowerHemisphere = static_cast<uint32_t>(lowerHemisphere);
                ui::PropertyFloat("グラウンドアルベド", &sky.groundAlbedo, 0.0f, 1.0f, defaults.groundAlbedo,
                                  "地面反射では直射光と天空光を反射する割合。空の延長では青みを保って下半球の明るさを調整します。\n"
                                  "どちらも大気の多重散乱へ反映します。\n"
                                  "0 は暗く、1 は強く反射します。地形マテリアルの色や反射率自体は変えません。");
                ui::EndPropertyTable();
            }
            ui::SectionHeader("環境光");
            if (ui::BeginPropertyTable("atmosphericEnvironmentRows", "スカイライト強度")) {
                ui::PropertyFloat("スカイライト強度", &m_renderer.AtmosphericEnvironmentIntensity(),
                                  0.0f, 8.0f, renderer::PreviewRenderer::DefaultSkylightIntensity,
                                  "空と地面反射から地形・マテリアル・雲へ届く環境光の倍率。1 は大気計算そのままです。\n"
                                  "上げると陰側・環境反射・雲の天空照明を明るくします。太陽の直射光・背景の空・露出は変えません。\n"
                                  "1 以外は照明バランスを調整するための補正です。局所的な地形の照り返しを計算する GI とは異なります。");
                ui::EndPropertyTable();
            }
            ui::SectionHeader("雲");
            const bool nodeCloud = m_graph.CompileCloud().hasOutput;
            if (nodeCloud) ui::HintText("雲はグラフの雲塊・雲出力で設定します");
            if (!nodeCloud && ui::BeginPropertyTable("cloudRows")) {
                bool clouds = sky.clouds != 0;
                if (ui::PropertyBool("雲を描画", &clouds, defaults.clouds != 0,
                                     "立体的な雲を描画し、環境光と反射にも反映します。\n"
                                     "ライトの「影」がオンなら地形に雲影も落とします。オフにしても雲の設定は保持されます。")) sky.clouds = clouds ? 1u : 0u;
                if (clouds) {
                    ui::PropertyFloat("Indirect Light", &sky.indirectLight, 0.0f, 5.0f, defaults.indirectLight,
                        "雲の中で繰り返し散乱する太陽光の倍率。上げると雲自体が明るくなります。\n"
                        "1 は従来の明るさ、0 は太陽光の多重散乱なし。密度・透過率・地形への雲影は変えません。", "%.2f");
                    ui::PropertyFloat("Ambient Light", &sky.ambientLight, 0.0f, 5.0f, defaults.ambientLight,
                        "雲が空と地面反射から受ける環境光の倍率。スカイライト強度に掛け合わせます。\n"
                        "0 は影響なし、1 は従来どおり。地形のスカイライト強度や雲の太陽光・密度は変えません。", "%.2f");
                    ui::PropertyLabel("雲の配置", "現在の地形サイズと変位量から、山にかかる低い雲層へまとめて設定します。\n"
                                      "地形の四隅より少し外まで広げます。適用後は各値を個別に調整できます。");
                    if (ui::Button("地形に合わせる", ui::kWideButtonWidth)) {
                        const float size = m_renderer.PlaneSize();
                        const float height = m_renderer.DisplacementScale();
                        // 地形の高さは (Height - 0.5) * 変位量。雲の濃い下部を山腹へ置く。
                        sky.cloudBottom = std::clamp(height * 0.1f, -10000.0f, 10000.0f);
                        sky.cloudThickness = std::clamp(height * 0.3f, 10.0f, 6000.0f);
                        sky.cloudScale = std::clamp(size * 0.3f, 10.0f, 40000.0f);
                        sky.fieldCenterX = sky.fieldCenterZ = 0.0f;
                        sky.fieldRadius = std::clamp(size * 0.78f, 1.0f, 200000.0f);
                        sky.fieldFalloff = std::clamp(size * 0.07f, 1.0f, 50000.0f);
                    }
                    ui::PropertyEnd();
                    ui::PropertyFloat("雲量", &sky.coverage, 0.0f, 1.0f, defaults.coverage,
                                      "雲のできる範囲を調整します。大きいほど雲が増えてつながり、小さいほど晴れ間が増えます。\n"
                                      "空を覆う面積の割合そのものではありません。");
                    ui::PropertyFloat("消散係数", &sky.extinction, 0.0001f, 0.03f, defaults.extinction,
                                      "雲の中を進む光の減衰の強さ（1/m）。大きいほど光を通しにくく、雲と雲影が濃くなります。\n"
                                      "雲の範囲は「雲量」、上下の寸法は「厚さ」で調整します。", "%.4f");
                    ui::PropertyFloat("雲底", &sky.cloudBottom, -10000.0f, 10000.0f, defaults.cloudBottom,
                                      "地形の原点から雲の下端までの高さ（m）。山頂からの高さではありません。\n"
                                      "負の値で原点より下へ下げられます。山にかけるときは厚さも小さくします。", "%.0f m");
                    ui::PropertyFloat("厚さ", &sky.cloudThickness, 10.0f, 6000.0f, defaults.cloudThickness,
                                      "雲層の上下方向の厚さ（m）。雲の上端は「雲底 + 厚さ」です。\n"
                                      "厚くすると光が通る雲の距離が増え、同じ消散係数でも光を遮りやすくなります。", "%.0f m");
                    const char* noiseTypes[] = {"Perlin fBM", "Perlin-Worley"};
                    int noiseType = static_cast<int>(sky.cloudNoiseType);
                    if (ui::PropertyCombo("ノイズの種類", &noiseType, noiseTypes, 2,
                        static_cast<int>(defaults.cloudNoiseType),
                        "Perlin fBM は従来の模様。Perlin-Worley は丸い細胞状の膨らみを使います。"))
                        sky.cloudNoiseType = static_cast<uint32_t>(noiseType);
                    ui::PropertyFloat("ノイズスケール", &sky.cloudScale, 10.0f, 40000.0f, defaults.cloudScale,
                                      "雲模様の水平方向の大きさ（m）。大きくすると大きな雲塊、小さくすると細かな雲になります。\n"
                                      "雲を描く範囲の端や、雲量を変える設定ではありません。", "%.0f m");
                    ui::PropertyFloat("範囲の中心 X", &sky.fieldCenterX, -200000.0f, 200000.0f, defaults.fieldCenterX,
                                      "雲が存在する円形範囲の中心（ワールド座標）。カメラを移動しても範囲は動きません。", "%.0f m");
                    ui::PropertyFloat("範囲の中心 Z", &sky.fieldCenterZ, -200000.0f, 200000.0f, defaults.fieldCenterZ,
                                      "雲が存在する円形範囲の中心（ワールド座標）。雲本体・雲影・環境光で同じ範囲を使います。", "%.0f m");
                    ui::PropertyFloat("雲の分布半径", &sky.fieldRadius, 1.0f, 200000.0f, defaults.fieldRadius,
                                      "雲が存在する水平範囲の半径。雲模様の大きさは「ノイズスケール」で調整します。", "%.0f m");
                    ui::PropertyFloat("境界フェード幅", &sky.fieldFalloff, 1.0f, 50000.0f, defaults.fieldFalloff,
                                      "範囲の外縁へ向けて密度をゼロにする幅。半径を超える値は半径として扱います。", "%.0f m");
                    bool animate = sky.animateClouds != 0;
                    if (ui::PropertyBool("アニメーション", &animate, defaults.animateClouds != 0,
                                         "風で雲模様を流します。オフで一時停止。雲と雲影は同期し、環境光と反射は約 1 秒ごとに更新します。"))
                        sky.animateClouds = animate ? 1u : 0u;
                    ui::PropertyFloat("風速", &sky.windSpeed, 0.0f, 1000.0f, defaults.windSpeed,
                                      "雲模様が進む速度。存在範囲の中心と半径は動きません。", "%.1f m/s");
                    float windDegrees = sky.windDirection * 180.0f / 3.14159265f;
                    if (ui::PropertyFloat("風向", &windDegrees, -180.0f, 180.0f, defaults.windDirection,
                                          "雲が進む方向。0 度は +Z、90 度は +X です。", "%.0f deg"))
                        sky.windDirection = windDegrees * 3.14159265f / 180.0f;
                    int seed = static_cast<int>(sky.seed);
                    if (ui::PropertyInt("シード", &seed, 0, 10000, static_cast<int>(defaults.seed),
                                        "雲模様の乱数の種。値を変えると雲の形と配置が変わります。\n"
                                        "同じシードと設定なら同じ雲を再現できます。")) sky.seed = static_cast<uint32_t>(seed);
                    const char* quality[] = {"低", "標準", "高"};
                    int index = sky.samples <= 32 ? 0 : sky.samples <= 64 ? 1 : 2;
                    if (ui::PropertyCombo("品質", &index, quality, 3, 1,
                                          "雲の奥行きを計算する細かさ。高くすると筋や段差が出にくくなりますが、描画が重くなります。\n"
                                          "操作が重いときは「低」、仕上がりの確認には「高」を選びます。")) sky.samples = 32u << index;
                }
                ui::EndPropertyTable();
            }
            ui::HintText("太陽に合わせて空・環境光・雲影が変わります");
        }

        ui::SectionHeader("露出");
        renderer::ExposureSettings& exposure = m_renderer.Exposure();
        if (ui::BeginPropertyTable("exposureRows")) {
            ui::PropertyBool("EV を直接指定", &exposure.useManualEv, kDefaultExposure.useManualEv,
                             "オフにすると絞り / シャッター / ISO から EV100 を求める");
            if (exposure.useManualEv) {
                ui::PropertyFloat("EV100", &exposure.manualEv100, -6.0f, 20.0f,
                                  kDefaultExposure.manualEv100, nullptr, "%.2f");
            } else {
                // **編集はカメラの節の「F 値」1 か所だけ。** ここは EV の内訳を
                // 読むための表示に留める（絞りはレンズの値で、ボケにも効くため）。
                ui::PropertyValue("絞り", "F%.1f（カメラ）", exposure.aperture);

                float shutterDenominator = 1.0f / exposure.shutterSpeed;
                if (ui::PropertyFloat("シャッター", &shutterDenominator, 1.0f, 4000.0f,
                                      1.0f / kDefaultExposure.shutterSpeed,
                                      "秒の逆数。大きいほど暗くなる", "1/%.0f 秒",
                                      ImGuiSliderFlags_Logarithmic)) {
                    exposure.shutterSpeed = 1.0f / shutterDenominator;
                }
                ui::PropertyFloat("ISO", &exposure.iso, 50.0f, 6400.0f, kDefaultExposure.iso,
                                  "感度。大きいほど明るくなる", "%.0f",
                                  ImGuiSliderFlags_Logarithmic);
            }
            ui::PropertyValue("EV100", "%.2f  (exposure %.3e)", exposure.Ev100(),
                              exposure.Exposure());
            ui::EndPropertyTable();
        }

        // **環境そのもの（何を空にするか）は天球パネルが持つ。**
        // ここに残すのは、天球ではなく見え方に属する設定だけ。
        if (!m_renderer.AtmosphericMode()) {
            ui::SectionHeader("環境 (IBL)");
            if (ui::BeginPropertyTable("iblRows")) {
                const renderer::SkyAsset* activeSky = m_skyLibrary.Active();
                ui::PropertyValue("天球", "%s", (activeSky != nullptr) ? activeSky->name.c_str() : "-");
                ui::PropertyValue("環境", "%s", m_renderer.GetEnvironment().SourceName().c_str());
                ui::PropertyValue("equirect", "%u x %u", m_renderer.GetEnvironment().EquirectWidth(),
                                  m_renderer.GetEnvironment().EquirectHeight());
                ui::PropertyBool("背景を表示", &m_renderer.ShowSkybox(),
                                 renderer::kPreviewDefaults.showSkybox,
                                 "オフにすると背景色だけになる。IBL の寄与は残る");
                ImGui::BeginDisabled(!m_renderer.ShowSkybox());
                ui::PropertyBool("背景をぼかす", &m_renderer.SkyboxBlur(),
                                 renderer::kPreviewDefaults.skyboxBlur,
                                 "背景だけを柔らかくする。素材を見比べるときに、"
                                 "背景の細部が目移りの原因にならないようにする。"
                                 "IBL の寄与と陰影は変わらない");
                ImGui::EndDisabled();
                ui::EndPropertyTable();
            }
            ui::HintText("空の切り替えと輝度は「天球」パネルで設定する");
        }

        ui::SectionHeader("トーンマップ");
        if (ui::BeginPropertyTable("tonemapRows")) {
            static const char* const kTonemapLabels[] = {"なし", "Reinhard", "ACES"};
            int tonemap = static_cast<int>(m_renderer.Tonemap());
            if (ui::PropertyCombo("方式", &tonemap, kTonemapLabels, IM_ARRAYSIZE(kTonemapLabels),
                                  static_cast<int>(renderer::kPreviewDefaults.tonemap))) {
                m_renderer.Tonemap() = static_cast<renderer::TonemapMode>(tonemap);
            }
            ui::EndPropertyTable();
        }
    }
    ImGui::End();
}

}  // namespace tg
