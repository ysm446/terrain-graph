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
#include <cstring>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

void Application::DrawMaterialPanel() {
    // **ここでは前面を要求しない。** レイヤーと同じ枠のタブなので、
    // 両方が要求すると後から描いたほうが勝ち、既定の前面が定まらない。
    if (ImGui::Begin(ui::Tr("プレビュー設定###PreviewSettings", "Preview Settings###PreviewSettings"))) {
        if (ui::BeginPropertyTable("previewRows")) {
            const renderer::PreviewDefaults& defaults = renderer::kPreviewDefaults;
            ui::PropertyBool(ui::Tr("合成結果", "Composite"), &m_renderer.UseMaterialTextures(),
                             defaults.useMaterialTextures,
                             ui::Tr("オフにすると、レイヤー合成を使わず単色マテリアルで表示する",
                                    "When off, shows a single-color material instead of the layer composite"));
            // マスクを見ているときだけ効く。行は常に出す（表示の好みなので）。
            ui::PropertyBool(ui::Tr("マスクの飽和に斜線", "Hatch Saturation"), &m_renderer.MaskSaturationHatch(),
                             defaults.maskSaturationHatch,
                             ui::Tr("マスクのプレビューで、0 か 1 に張り付いた所へ斜線を引く。"
                                    "濃淡が付いている所と、上限に当たって潰れた所を見分けられる",
                                    "In mask previews, hatches areas stuck at 0 or 1. "
                                    "Tells graded areas apart from areas clipped at the limits"));

            // 平面の大きさ（m）。**ジオメトリだけがメートル**で、
            // テクスチャは無次元のまま（1 UV が何 m かは決めない）。
            //
            // **グラフに Heightmap ノードがあるときは、その実寸に従う。**
            // 地形の大きさは見え方の設定ではなく読み込んだデータの性質なので、
            // 決める場所はノード側の 1 か所だけにする。
            const bool scaleFromGraph = (m_graph.FindChainScale(m_previewGraphNode) != nullptr);
            if (!scaleFromGraph) {
                ui::PropertyFloat(ui::Tr("平面のサイズ", "Plane Size"), &m_renderer.PlaneSize(), 0.5f, 32768.0f,
                                  defaults.planeSize,
                                  ui::Tr("平面の一辺の長さ（m）。素材は 2m 前後、"
                                         "地形なら 1000m 以上。カメラと影の範囲もこれに追従する",
                                         "Side length of the plane (m). About 2 m for materials, "
                                         "1000 m or more for terrain. The camera and shadow range follow it"),
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
                    ui::PropertyValue(ui::Tr("平面のサイズ", "Plane Size"), "%.1f m", m_renderer.PlaneSize());
                    ui::PropertyValue(ui::Tr("変位量", "Displacement"), "%.1f m", m_renderer.DisplacementScale());
                    if (const graph::TerrainScale* scale = m_graph.FindChainScale(m_previewGraphNode)) {
                        ui::PropertyValue(ui::Tr("最低標高", "Base Elevation"), "%.1f m", scale->baseElevationMeters);
                    }
                } else {
                    const float displacementMax =
                        std::max(1.0f, m_renderer.PlaneSize() * 0.5f);
                    ui::PropertyFloat(
                        ui::Tr("変位量", "Displacement"), &m_renderer.DisplacementScale(), 0.0f, displacementMax,
                        defaults.displacementScale,
                        ui::Tr("ハイトを形状に反映する量（ディスプレイスメント）。"
                               "ハイト 0〜1 の全幅がこの高さ（m）になる。0 なら形は変わらない",
                               "How much the height shapes the surface (displacement). "
                               "The full 0-1 height range maps to this height (m). 0 leaves the shape flat"),
                        "%.2f m", 0, 0.01f);
                }

                ui::PropertyBool(ui::Tr("テセレーション", "Tessellation"), &m_renderer.TessellationEnabled(),
                                 defaults.tessellationEnabled,
                                 ui::Tr("画面上の辺が長いところだけメッシュを細かく割る。"
                                        "変位量を上げたときに形がなめらかになる",
                                        "Subdivides the mesh only where edges are long on screen. "
                                        "Smooths the shape when displacement is high"));
                if (m_renderer.TessellationEnabled()) {
                    ui::PropertyFloat(ui::Tr("分割の上限", "Max Subdivision"), &m_renderer.TessellationFactor(), 1.0f, 16.0f,
                                      defaults.tessellationFactor,
                                      ui::Tr("1 辺をこの回数まで割る。上げるほど重くなる",
                                             "Splits each edge up to this many times. Higher is heavier"),
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
                              ui::Tr("平面を何分割するか。いまは 1 マス %.1f m。"
                                     "上げるほど細かい形が出るが重くなる",
                                     "Number of plane divisions. Each cell is %.1f m now. "
                                     "Higher shows finer shapes but is heavier"),
                              m_renderer.PlaneSize() /
                                  static_cast<float>(m_renderer.MeshSubdivisions()));
                if (ui::PropertyCombo(ui::Tr("メッシュ分割", "Mesh Divisions"), &subdivisions, kMeshSubdivisionLabels,
                                      IM_ARRAYSIZE(kMeshSubdivisionLabels), 0, meshHint)) {
                    m_renderer.RequestMeshSubdivisions(kMeshSubdivisionValues[subdivisions]);
                }

                int resolution = MaterialResolutionIndex(m_renderer.MaterialResolution());
                if (ui::PropertyCombo(ui::Tr("合成解像度", "Composite Res."), &resolution, kMaterialResolutionLabels,
                                      IM_ARRAYSIZE(kMaterialResolutionLabels),
                                      MaterialResolutionIndex(defaults.materialResolution),
                                      ui::Tr("編集中のプレビュー解像度。上げるほど細部が出るが重くなる。"
                                             "8192 は VRAM を多く使う",
                                             "Preview resolution while editing. Higher shows more detail but is heavier. "
                                             "8192 uses a lot of VRAM"))) {
                    m_renderer.RequestMaterialResolution(kMaterialResolutionValues[resolution]);
                }
            } else {
                renderer::MaterialSettings& material = m_renderer.Material();
                ui::PropertyColorLinear(ui::Tr("ベースカラー", "Base Color"), &material.baseColor.x,
                                        &kDefaultMaterial.baseColor.x);
                ui::PropertyFloat(ui::Tr("ラフネス", "Roughness"), &material.roughness, 0.0f, 1.0f,
                                  kDefaultMaterial.roughness, nullptr, "%.2f");
                ui::PropertyFloat(ui::Tr("メタルネス", "Metalness"), &material.metallic, 0.0f, 1.0f,
                                  kDefaultMaterial.metallic, nullptr, "%.2f");
            }
            ui::EndPropertyTable();
        }

        ui::SectionHeader(ui::Tr("カメラ", "Camera"));
        if (ui::BeginPropertyTable("cameraRows", ui::Tr("ニア / ファー", "Field of View"))) {
            // 露出を絞り / シャッター / ISO で決めているので、レンズも同じ言葉で扱う。
            // ラジアンのままだと何 mm 相当なのか分からない。
            renderer::Camera& camera = m_renderer.GetCamera();
            float focalLength = renderer::FocalLengthFromFovY(camera.FovY());
            if (ui::PropertyFloat(ui::Tr("焦点距離", "Focal Length"), &focalLength, 12.0f, 200.0f,
                                  renderer::FocalLengthFromFovY(kDefaultCamera.fovY),
                                  ui::Tr("35mm フルサイズ換算。小さいほど広角で、遠近が強く出る",
                                         "35 mm full-frame equivalent. Smaller is wider, with stronger perspective"),
                                  "%.0f mm", ImGuiSliderFlags_Logarithmic, 1.0f)) {
                camera.SetFovY(renderer::FovYFromFocalLength(focalLength));
            }
            // **F 値はレンズの値なのでここに置く。** 露出（絞り）とボケの
            // どちらも同じ 1 つの値で決まる。EV を直接指定していると露出の節から
            // 絞りの行が消えるので、レンズ側に置いておかないと触れなくなる。
            ui::PropertyFloat(ui::Tr("F 値", "F-Stop"), &m_renderer.Exposure().aperture, 1.0f, 32.0f,
                              renderer::ExposureSettings{}.aperture,
                              ui::Tr("絞り。小さいほどボケが強く、露出は明るくなる",
                                     "Aperture. Smaller gives stronger bokeh and a brighter exposure"), "F%.1f",
                              ImGuiSliderFlags_Logarithmic);
            ui::PropertyValue(ui::Tr("画角", "Field of View"), ui::Tr("%.1f 度（垂直）", "%.1f° (vertical)"),
                              RadiansToDegrees(camera.FovY()));
            // クリップ面。自動は被写体の大きさから決めるので、地形では近くの物が切れることがある。
            bool autoClip = camera.AutoClip();
            if (ui::PropertyBool(ui::Tr("クリップ自動", "Auto Clip"), &autoClip, kDefaultCamera.autoClip,
                                 ui::Tr("ニア / ファーを被写体（地形や雲）の大きさから決める。"
                                        "切ると値を指定できる（近くの物を寄って見るときはニアを下げる）",
                                        "Sets near / far from the size of the subject (terrain or clouds). "
                                        "Turn off to set them by hand (lower near to look closely at nearby objects)"))) {
                // 手動へ切り替えた直後は、いまの自動の値から始める。
                camera.SetClip(autoClip, autoClip ? camera.NearZ() : camera.AutoNearZ(),
                               autoClip ? camera.FarZ() : camera.AutoFarZ());
            }
            if (autoClip) {
                ui::PropertyValue(ui::Tr("ニア / ファー", "Near / Far"), "%.2f m / %.0f m", camera.NearZ(), camera.FarZ());
            } else {
                const char* kPrecision =
                    ui::Tr("深度の精度はファー ÷ ニアの比で決まる。比が 100 万を超えると遠くの面がちらつきやすい",
                           "Depth precision depends on the far / near ratio. Above 1,000,000, distant surfaces tend to flicker");
                float nearZ = camera.NearZ(), farZ = camera.FarZ();
                bool clipChanged = ui::PropertyFloat(ui::Tr("ニア", "Near"), &nearZ, renderer::kMinClipNear, renderer::kMaxClipNear,
                                                     kDefaultCamera.nearZ, kPrecision, "%.3f m",
                                                     ImGuiSliderFlags_Logarithmic);
                clipChanged |= ui::PropertyFloat(ui::Tr("ファー", "Far"), &farZ, 1.0f, renderer::kMaxClipFar,
                                                 kDefaultCamera.farZ, kPrecision, "%.0f m",
                                                 ImGuiSliderFlags_Logarithmic);
                if (clipChanged) camera.SetClip(false, nearZ, farZ);
            }
            ui::PropertyLabelEmpty("cameraReset");
            if (ui::Button(ui::Tr("視点をリセット", "Reset View"), ui::kWideButtonWidth)) {
                m_renderer.GetCamera().Reset();
            }
            ui::PropertyEnd();
            ui::EndPropertyTable();
        }

        ui::SectionHeader(ui::Tr("ゴッドレイ", "God Rays"));
        if (ui::BeginPropertyTable("godRayRows")) {
            auto& rays = m_renderer.GodRays();
            const renderer::GodRaySettings defaults;
            ui::PropertyBool(ui::Tr("有効", "Enabled"), &rays.enabled, defaults.enabled,
                ui::Tr("大気散乱スカイで空気中の光を描きます。雲なしでも有効です。地形による光の筋にはライティングの影を有効にしてください",
                       "Draws light in the air with the Atmospheric Sky. Works without clouds. For light shafts cast by terrain, enable Shadows in Lighting"));
            ImGui::BeginDisabled(!rays.enabled);
            ui::PropertyFloat(ui::Tr("濃さ", "Density"), &rays.density, 0.0f, 0.0002f, defaults.density,
                ui::Tr("光を散乱する空気の濃さ。上げるほど光の筋と霞が強くなります",
                       "Density of the air that scatters light. Higher makes light shafts and haze stronger"), "%.5f", 0, 0.000001f);
            ui::PropertyFloat(ui::Tr("表示距離", "Distance"), &rays.distance, 100.0f, 20000.0f, defaults.distance,
                ui::Tr("カメラから光の散乱を計算する距離。長いほど遠くまで霞みます",
                       "Distance from the camera over which scattering is computed. Longer adds haze farther out"), "%.0f m");
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }

        ui::SectionHeader(ui::Tr("雲と環境光", "Clouds & Ambient Light"));
        if (ui::BeginPropertyTable("cloudAmbientRows")) {
            auto& ambient = m_renderer.CloudAmbient();
            const renderer::CloudAmbientSettings defaults;
            ui::PropertyFloat(ui::Tr("遮蔽の強さ", "Occlusion"), &ambient.occlusion, 0.0f, 1.0f, defaults.occlusion,
                ui::Tr("雲が環境光を遮る強さ。環境マップは原点から 1 回だけ撮るので、雲層より上の地形は雲を抜いた空の環境で照らします。"
                       "1 で雲底より下は雲ありの環境、0 で高さに関わらず雲なしの環境。大気散乱スカイで雲があるときだけ効きます",
                       "How strongly clouds block ambient light. The environment map is captured once from the origin, so terrain above the cloud layer is lit by the sky without clouds. "
                       "1 uses the cloudy environment below the cloud base; 0 uses the cloud-free one at any height. Only applies with the Atmospheric Sky when there are clouds"), "%.2f");
            ui::PropertyFloat(ui::Tr("遷移の幅", "Transition"), &ambient.transition, 0.05f, 4.0f, defaults.transition,
                ui::Tr("雲あり / 雲なしの環境が入れ替わる高さの幅。雲層の厚さに対する倍率で、1 なら雲底から雲頂までで入れ替わります",
                       "Height range over which the cloudy and cloud-free environments swap. A multiple of the cloud layer thickness; 1 swaps between cloud base and cloud top"), "%.2f");
            ui::PropertyFloat(ui::Tr("高さのオフセット", "Height Offset"), &ambient.heightOffset, -10000.0f, 10000.0f, defaults.heightOffset,
                ui::Tr("入れ替わる高さを上下にずらします。雲海の見た目の上面に合わせたいときに使います",
                       "Shifts the swap height up or down. Use it to match the visible top of a sea of clouds"), "%.0f m");
            ui::EndPropertyTable();
        }

        ui::SectionHeader(ui::Tr("ボリューム雲", "Volumetric Clouds"));
        if (ui::BeginPropertyTable("volumeCloudQuality", ui::Tr("レイマーチ品質", "Ray March Quality"))) {
            ui::PropertyBool(ui::Tr("雲を描画", "Draw Clouds"), &m_renderer.ShowClouds(), renderer::kPreviewDefaults.showClouds,
                ui::Tr("雲と雲影をまとめて表示・非表示にします。ノードと天球の雲設定は保持します",
                       "Shows or hides clouds and cloud shadows together. Cloud settings on nodes and the sky are kept"));
            auto& sky = m_renderer.AtmosphericSettings();
            static const ui::LocalizedText resolutions[] = {{"半解像度", "Half Resolution"}, {"全解像度", "Full Resolution"}};
            int resolution = m_renderer.FullResolutionClouds() ? 1 : 0;
            if (ui::PropertyCombo(ui::Tr("描画解像度", "Resolution"), &resolution, ui::Tr(resolutions).data(), 2, 0,
                ui::Tr("半解像度は軽量な描画。全解像度は雲の細部を確認するための設定で、描画負荷が増えます。",
                       "Half resolution is lightweight. Full resolution is for checking cloud detail and costs more to render.")))
                m_renderer.FullResolutionClouds() = resolution == 1;
            ImGui::BeginDisabled(m_renderer.FullResolutionClouds());
            ui::PropertyBool(ui::Tr("時間方向の再投影", "Reprojection"), &m_renderer.TemporalClouds(), true,
                ui::Tr("半解像度の雲を前フレームから再投影して蓄積し、全解像度に近い見た目にします。雲や視点の編集時は履歴を捨てます。全解像度では使いません。",
                       "Reprojects and accumulates half-resolution clouds from previous frames for a near full-resolution look. History is discarded when clouds or the view are edited. Not used at full resolution."));
            ImGui::EndDisabled();
            static const ui::LocalizedText qualities[] = {{"低（32）", "Low (32)"}, {"標準（64）", "Standard (64)"}, {"高（128）", "High (128)"}};
            int quality = sky.samples <= 32 ? 0 : sky.samples <= 64 ? 1 : 2;
            if (ui::PropertyCombo(ui::Tr("レイマーチ品質", "Ray March Quality"), &quality, ui::Tr(qualities).data(), 3, 1,
                ui::Tr("雲の奥行き方向のサンプル数。高くすると筋や段差を抑えますが、描画が重くなります。雲塊・雲層に共通です。",
                       "Number of samples through the cloud depth. Higher reduces streaks and banding but is heavier. Shared by cloud masses and cloud layers.")))
                sky.samples = 32u << quality;
            static const ui::LocalizedText lighting[] = {{"標準（キャッシュ）", "Standard (Cached)"}, {"高（直接計算）", "High (Direct)"}};
            int lightQuality = m_renderer.CloudLightingCache() ? 0 : 1;
            if (ui::PropertyCombo(ui::Tr("陰影の品質", "Shading Quality"), &lightQuality, ui::Tr(lighting).data(), 2, 0,
                ui::Tr("雲層の内部の陰影と地形への雲影。標準は光学的厚さを格子に保存して再利用します。直接計算は細かな変化を評価しますが重くなります。雲塊には影響しません。",
                       "Shading inside cloud layers and cloud shadows on terrain. Standard stores optical depth in a grid and reuses it. Direct evaluates fine changes but is heavier. Does not affect cloud masses.")))
                m_renderer.CloudLightingCache() = lightQuality == 0;
            ui::PropertyBool(ui::Tr("地球の曲率", "Earth Curvature"), &m_renderer.CloudCurvature(), true,
                ui::Tr("Cloud Weather Layer を地球と同心の球殻として曲げ、遠くの雲を地平線へ沈めます。オフでは平らな層として描きます。",
                       "Bends Cloud Weather Layer into a shell concentric with the Earth so distant clouds sink below the horizon. When off, draws it as a flat layer."));
            ui::PropertyBool(ui::Tr("遠景の別パス", "Far Pass"), &m_renderer.CloudFarPass(), true,
                ui::Tr("Cloud Weather Layer の「遠景の開始距離」より先を 1/4 解像度の別パスで描きます。オフでは全距離を同じ解像度で積分し、描画負荷が増えます。",
                       "Draws clouds beyond the far start distance of Cloud Weather Layer in a separate quarter-resolution pass. When off, all distances are integrated at the same resolution, which costs more."));
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("大気散乱スカイで使用。雲の形と配置は雲ノードで設定します",
                                  "Used by the Atmospheric Sky. Cloud shape and placement are set with cloud nodes"));

        // **レンズの値はここに出さない。** 焦点距離も F 値もカメラの節が持っていて、
        // すぐ上に見えている。同じ値を並べると、どちらが効いているのか分からなくなる。
        ui::SectionHeader(ui::Tr("被写界深度", "Depth of Field"));
        renderer::DofSettings& dof = m_renderer.Dof();
        const renderer::DofSettings kDefaultDof;
        if (ui::BeginPropertyTable("dofRows")) {
            ui::PropertyBool(ui::Tr("有効", "Enabled"), &dof.enabled, kDefaultDof.enabled,
                             ui::Tr("ビューポートの見え方だけに掛かる。合成結果と書き出しには効かない",
                                    "Affects only how the viewport looks. Has no effect on the composite or exports"));

            ImGui::BeginDisabled(!dof.enabled);

            ui::PropertyBool(ui::Tr("注視点に追従", "Follow Target"), &dof.focusOnTarget, kDefaultDof.focusOnTarget,
                             ui::Tr("軌道カメラなので、見ているものは常に注視点にある。"
                                    "切ると距離を手で決められる",
                                    "The orbit camera always looks at its target. "
                                    "Turn off to set the distance by hand"));
            ImGui::BeginDisabled(dof.focusOnTarget);
            ui::PropertyFloat(ui::Tr("ピント距離", "Focus Distance"), &dof.focusDistance, 0.1f, 50.0f,
                              kDefaultDof.focusDistance,
                              ui::Tr("カメラからピント面まで。ワールドの 1 単位を 1m とみなす",
                                     "From the camera to the focal plane. One world unit is treated as 1 m"),
                              "%.2f m", ImGuiSliderFlags_Logarithmic);
            ImGui::EndDisabled();
            ui::PropertyValue(ui::Tr("実効ピント距離", "Effective Focus"), "%.2f m", m_renderer.FocusDistance());

            ui::PropertyFloat(ui::Tr("ミニチュア", "Miniature"), &dof.miniatureScale, 1.0f, 10000.0f,
                              kDefaultDof.miniatureScale,
                              ui::Tr("1 で実物大。上げるほど模型を撮った計算になり、"
                                     "同じレンズでもボケが強くなる。実寸のままだと地形は"
                                     "遠すぎて 1 画素もボケない。2km の地形を引きで見るなら "
                                     "3000〜10000 が目安",
                                     "1 is life-size. Higher computes as if shooting a scale model, "
                                     "so the same lens gives stronger bokeh. At real scale, terrain is "
                                     "too far away to blur even 1 pixel. For a 2 km terrain seen from afar, "
                                     "3000 to 10000 is a guide"),
                              "1 : %.0f", ImGuiSliderFlags_Logarithmic);
            ui::PropertyFloat(ui::Tr("ボケの強さ", "Bokeh Strength"), &dof.blurScale, 0.25f, 16.0f, kDefaultDof.blurScale,
                              ui::Tr("1 で現実どおり。2m 角の地面を広角で撮れば現実でも"
                                     "全域にピントが合うので、見せたい量まで持ち上げるための誇張。"
                                     "倍率を使わずに出したいなら望遠へ寄せる",
                                     "1 is physically accurate. A 2 m patch of ground shot wide is in focus "
                                     "everywhere in reality too, so this exaggerates bokeh to the amount you want to show. "
                                     "To get it without the multiplier, move toward telephoto"),
                              "x%.2f", ImGuiSliderFlags_Logarithmic);
            ui::PropertyFloat(ui::Tr("最大ぼけ", "Max Blur"), &dof.maxBlurPixels, 1.0f, 64.0f,
                              kDefaultDof.maxBlurPixels,
                              ui::Tr("画面上のぼけ半径の上限。現実の式のままだと極端になるので、"
                                     "表示のための頭打ちとして持つ。上げるほど重くなる",
                                     "Upper limit of the on-screen blur radius. The physical formula gets extreme, "
                                     "so this caps it for display. Higher is heavier"),
                              "%.0f px");

            static const ui::LocalizedText kShapeLabels[] = {
                {"円", "Circle"}, {"三角形", "Triangle"}, {"六角形", "Hexagon"}, {"八角形", "Octagon"}};
            int shape = static_cast<int>(dof.shape);
            if (ui::PropertyCombo(ui::Tr("絞りの形", "Aperture Shape"), &shape, ui::Tr(kShapeLabels).data(), IM_ARRAYSIZE(kShapeLabels),
                                  static_cast<int>(kDefaultDof.shape), ui::Tr("ボケの形になる", "Sets the shape of the bokeh"))) {
                dof.shape = static_cast<renderer::ApertureShape>(shape);
            }
            ImGui::BeginDisabled(dof.shape == renderer::ApertureShape::Circle);
            ui::PropertyFloat(ui::Tr("絞りの向き", "Aperture Angle"), &dof.rotationDegrees, 0.0f, 180.0f,
                              kDefaultDof.rotationDegrees, ui::Tr("多角形のボケの角度", "Angle of polygonal bokeh"),
                              ui::Tr("%.0f 度", "%.0f°"));
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("ボケ量は焦点距離・F 値・ピント距離で決まる。ワールドの 1 単位 = 1m",
                                  "Bokeh depends on focal length, F-stop and focus distance. 1 world unit = 1 m"));
    }
    ImGui::End();
}

void Application::DrawLightingPanel() {
    const auto workLightBefore = m_renderer.LegacyLight();
    const auto sunBefore = m_renderer.AtmosphericLight();
    const auto skyBefore = m_renderer.AtmosphericSettings();
    const auto celestialBefore = m_renderer.Celestial();
    const float skylightBefore = m_renderer.AtmosphericEnvironmentIntensity();
    if (m_focusLighting) { ImGui::SetNextWindowFocus(); m_focusLighting = false; }
    if (ImGui::Begin(ui::Tr("ライティング###Lighting", "Lighting###Lighting"))) {
        if (ui::BeginPropertyTable("lightingModeRows")) {
            static const ui::LocalizedText modes[] = {{"作業用IBL", "Work IBL"}, {"シーンの空・照明", "Scene Sky & Lighting"}};
            int mode = m_renderer.AtmosphericMode() ? 1 : 0;
            if (ui::PropertyCombo(ui::Tr("表示環境", "Lighting Mode"), &mode, ui::Tr(modes).data(), 2, 1,
                                  ui::Tr("作業用IBLは質感の確認用です。シーンの空・照明では大気散乱スカイと雲を描画します。\n"
                                         "表示の切り替えはプロジェクトの作業設定に保存します。",
                                         "Work IBL is for checking surfaces. Scene Sky & Lighting draws the Atmospheric Sky and clouds.\n"
                                         "The choice is saved in the project's work settings."))) {
                m_renderer.AtmosphericMode() = mode == 1; m_pendingWorkEnvironmentSave = true;
            }
            ui::EndPropertyTable();
        }
        ui::SectionHeader(ui::Tr("シーンの大気散乱スカイ", "Scene Atmospheric Sky"));
        if (ui::BeginPropertyTable("sceneAtmosphereAsset")) {
            const auto path = m_sceneAtmosphere.is_null() ? std::filesystem::path{} : m_workspace.Resolve(m_sceneAtmosphere);
            DrawAssetPathRow(ui::Tr("ファイル", "File"), path, m_pendingAssetReveal);
            ui::PropertyLabelEmpty("sceneSkyActions");
            if (ui::Button(ui::Tr("読み込む…", "Load…"), ui::kWideButtonWidth)) {
                const auto selected = ShowOpenFileDialog(ui::Tr(L"大気散乱スカイを読み込む", L"Load Atmospheric Sky"), {{ui::Tr(L"大気散乱スカイ", L"Atmospheric Sky"), L"*.tgatmosphere"}});
                if (!selected.empty()) m_pendingAssetOpen = selected;
            }
            ImGui::SameLine();
            if (ui::Button(ui::Tr("保存", "Save"))) m_pendingAtmosphereSave = true;
            ui::PropertyEnd(); ui::EndPropertyTable();
        }
        if (!m_renderer.AtmosphericMode()) ui::HintText("%s", ui::Tr("作業用IBLで確認中。シーンの太陽・大気の設定は保持されています",
                                                                   "Previewing with Work IBL. The scene's sun and atmosphere settings are kept"));
        const bool dateTimeMode = m_renderer.AtmosphericMode() && m_renderer.Celestial().mode == 1;
        if (m_renderer.AtmosphericMode()) {
            auto& celestial = m_renderer.Celestial();
            auto& sky = m_renderer.AtmosphericSettings();
            const renderer::CelestialSettings celestialDefaults;
            const renderer::AtmosphereSettings skyDefaults;
            ui::SectionHeader(ui::Tr("観測地と日時", "Location & Time"));
            if (ui::BeginPropertyTable("celestialRows", ui::Tr("UTC オフセット", "Celestial Mode"))) {
                static const ui::LocalizedText kCelestialModes[] = {{"手動", "Manual"}, {"緯度経度と日時", "Lat/Long & Time"}};
                int mode = static_cast<int>(celestial.mode);
                if (ui::PropertyCombo(ui::Tr("天体の位置", "Celestial Mode"), &mode, ui::Tr(kCelestialModes).data(), IM_ARRAYSIZE(kCelestialModes),
                        static_cast<int>(celestialDefaults.mode),
                        ui::Tr("手動では太陽・月・星空の回転を直接決めます。\n"
                               "緯度経度と日時では、その場所と時刻に見える太陽・月・月相・星空を計算して書き込みます。\n"
                               "手動へ戻すと計算した値がそのまま残るので、微調整の出発点にできます。",
                               "Manual sets the sun, moon and star rotation directly.\n"
                               "Lat/Long & Time computes and writes the sun, moon, moon phase and stars seen at that place and time.\n"
                               "Switching back to Manual keeps the computed values, so they can be a starting point for fine-tuning.")))
                    celestial.mode = static_cast<uint32_t>(mode);
                float latitude = RadiansToDegrees(sky.starLatitude);
                if (ui::PropertyFloat(ui::Tr("緯度", "Latitude"), &latitude, -90.0f, 90.0f, RadiansToDegrees(skyDefaults.starLatitude),
                        ui::Tr("観測地の緯度。北緯が正。手動でも星空の天の北極の高さに使います。+Z が北、+X が東です。",
                               "Latitude of the observer; north is positive. Also sets the height of the celestial north pole in Manual. +Z is north, +X is east."), "%.2f deg"))
                    sky.starLatitude = DegreesToRadians(latitude);
                ImGui::BeginDisabled(!dateTimeMode);
                float longitude = RadiansToDegrees(celestial.longitude);
                if (ui::PropertyFloat(ui::Tr("経度", "Longitude"), &longitude, -180.0f, 180.0f, RadiansToDegrees(celestialDefaults.longitude),
                        ui::Tr("観測地の経度。東経が正。", "Longitude of the observer; east is positive."), "%.2f deg"))
                    celestial.longitude = DegreesToRadians(longitude);
                ui::PropertyInt(ui::Tr("年", "Year"), &celestial.year, 1900, 2200, celestialDefaults.year,
                    ui::Tr("グレゴリオ暦の年。", "Year in the Gregorian calendar."));
                ui::PropertyInt(ui::Tr("月", "Month"), &celestial.month, 1, 12, celestialDefaults.month);
                ui::PropertyInt(ui::Tr("日", "Day"), &celestial.day, 1, 31, celestialDefaults.day);
                ui::PropertyFloat(ui::Tr("時刻", "Time"), &celestial.hour, 0.0f, 24.0f, celestialDefaults.hour,
                    ui::Tr("地方時（24 時間制、小数で分を表します）。", "Local time (24-hour clock; minutes as a fraction)."),
                    ui::Tr("%.2f 時", "%.2f h"));
                ui::PropertyFloat(ui::Tr("UTC オフセット", "UTC Offset"), &celestial.utcOffset, -14.0f, 14.0f, celestialDefaults.utcOffset,
                    ui::Tr("地方時と UTC の差。日本は +9。", "Difference between local time and UTC. +9 for Japan."),
                    ui::Tr("%.1f 時間", "%.1f h"));
                ImGui::EndDisabled();
                ui::EndPropertyTable();
            }
            if (dateTimeMode) ui::HintText("%s", ui::Tr("太陽・月・星空の回転は日時から計算しています。屈折と大気減光は含みません。",
                                                        "The sun, moon and star rotation are computed from the date and time. Refraction and atmospheric extinction are not included."));
        }
        renderer::LightSettings& light = m_renderer.Light();

        ui::SectionHeader(m_renderer.AtmosphericMode() ? ui::Tr("シーンの太陽", "Scene Sun") : ui::Tr("作業用ライト", "Work Light"));
        if (ui::BeginPropertyTable("lightRows", ui::Tr("カスケードシャドウ", "Shadow Resolution"))) {
            ImGui::BeginDisabled(dateTimeMode);
            float azimuthDeg = RadiansToDegrees(light.azimuth);
            if (ui::PropertyFloat(ui::Tr("方位角", "Azimuth"), &azimuthDeg, -180.0f, 180.0f,
                                  RadiansToDegrees(kDefaultLight.azimuth),
                                  ui::Tr("太陽の向き（水平方向）", "Direction of the sun (horizontal)"), ui::Tr("%.0f 度", "%.0f°"))) {
                light.azimuth = DegreesToRadians(azimuthDeg);
            }
            float elevationDeg = RadiansToDegrees(light.elevation);
            if (ui::PropertyFloat(ui::Tr("仰角", "Elevation"), &elevationDeg, -89.0f, 89.0f,
                                  RadiansToDegrees(kDefaultLight.elevation),
                                  ui::Tr("太陽の高さ。低いほど影が伸びる", "Height of the sun. Lower casts longer shadows"), ui::Tr("%.0f 度", "%.0f°"))) {
                light.elevation = DegreesToRadians(elevationDeg);
            }
            ImGui::EndDisabled();
            ui::PropertyFloat(ui::Tr("照度", "Illuminance"), &light.illuminance, 0.0f, 200000.0f,
                              m_renderer.AtmosphericMode() ? renderer::AtmosphereSettings{}.illuminance : kDefaultLight.illuminance,
                              m_renderer.AtmosphericMode()
                                  ? ui::Tr("大気圏外の照度 (lux)。地表では大気の透過率で減衰する",
                                           "Illuminance outside the atmosphere (lux). Attenuated by atmospheric transmittance at the ground")
                                  : ui::Tr("lux。晴天の直射日光がおよそ 100000 lux", "lux. Direct sunlight on a clear day is about 100000 lux"), "%.0f");
            if (!m_renderer.AtmosphericMode())
                ui::PropertyColorLinear(ui::Tr("光の色", "Light Color"), &light.color.x, &kDefaultLight.color.x);
            else ui::PropertyValue(ui::Tr("光の色", "Light Color"), "%s", ui::Tr("大気の透過率から自動計算", "Auto from atmospheric transmittance"));
            ui::PropertyBool(ui::Tr("影", "Shadows"), &m_renderer.ShadowEnabled(),
                             renderer::kPreviewDefaults.shadowEnabled,
                             ui::Tr("ディレクショナルライトの影を落とす。"
                                    "ディスプレイスメントで押し出した形にも落ちる。大気散乱スカイでは雲影も切り替える",
                                    "Casts directional light shadows. "
                                    "They also fall on shapes pushed out by displacement. With the Atmospheric Sky, also toggles cloud shadows"));
            ImGui::BeginDisabled(!m_renderer.ShadowEnabled());
            ui::PropertyBool(ui::Tr("カスケードシャドウ", "Cascaded Shadows"), &m_renderer.CascadedShadows(),
                             renderer::kPreviewDefaults.cascadedShadows,
                             ui::Tr("視距離を分割して近景の影を細かく描く。オフは従来の1枚方式",
                                    "Splits the view distance to draw nearby shadows in finer detail. Off uses the older single-map method"));
            if (m_renderer.CascadedShadows()) {
                ui::PropertyInt(ui::Tr("カスケード数", "Cascades"), &m_renderer.ShadowCascadeCount(), 1, 4,
                    renderer::kPreviewDefaults.shadowCascadeCount,
                    ui::Tr("影を描くカメラの数。増やすと近景の解像度が上がり、描画負荷とメモリも増えます",
                           "Number of shadow cameras. More raises nearby resolution, along with rendering cost and memory"));
                ui::PropertyFloat(ui::Tr("分割の偏り", "Split Bias"), &m_renderer.ShadowSplitLambda(), 0.0f, 1.0f,
                    renderer::kPreviewDefaults.shadowSplitLambda,
                    ui::Tr("視距離の分け方。1 に近いほど近くの段を短くして足元の影を細かく描く（その分、中景の段が粗くなる）。"
                           "0 は均等に分ける。最後の段はいつも奥まで覆うので、遠景の影は消えない",
                           "How the view distance is split. Closer to 1 shortens the near cascades for finer shadows close by (making middle cascades coarser). "
                           "0 splits evenly. The last cascade always reaches the far end, so distant shadows never disappear"),
                    "%.2f");
            }
            {
                static const char* const kResolutions[] = {"2048", "4096"};
                int resolution = m_renderer.ShadowResolution() == 4096 ? 1 : 0;
                if (ui::PropertyCombo(ui::Tr("影の解像度", "Shadow Resolution"), &resolution, kResolutions, 2,
                                      renderer::kPreviewDefaults.shadowResolution == 4096 ? 1 : 0,
                                      ui::Tr("影のテクスチャの一辺（画素）。4096 は影が 2 倍細かくなるが、影のメモリが 4 倍"
                                             "（カスケード 4 段で約 256 MB）になり、描く負荷も増える",
                                             "Side length of the shadow texture (pixels). 4096 makes shadows twice as fine, but shadow memory is 4x "
                                             "(about 256 MB with 4 cascades) and drawing costs more")))
                    m_renderer.ShadowResolution() = resolution == 1 ? 4096 : 2048;
            }
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }
        // 近景マテリアル。Surface の「近景の 1 周の長さ」で貼り直す範囲（カメラからの距離）。
        ui::SectionHeader(ui::Tr("近景マテリアル", "Near Material"));
        if (ui::BeginPropertyTable("nearMaterialRows")) {
            ui::PropertyBool(ui::Tr("近景マテリアル", "Near Material"), &m_renderer.NearMaterial(), renderer::kPreviewDefaults.nearMaterial,
                             ui::Tr("カメラの近くで、Surface のマテリアルを「近景の 1 周の長さ」で貼り直す。"
                                    "切ると合成結果だけを貼る",
                                    "Near the camera, re-applies Surface materials at their near tile length. "
                                    "Turn off to apply only the composite"));
            ImGui::BeginDisabled(!m_renderer.NearMaterial());
            ui::PropertyFloat(ui::Tr("近景だけの距離", "Near-Only Range"), &m_renderer.NearFadeStart(), 0.0f, 500.0f,
                              renderer::kPreviewDefaults.nearFadeStart,
                              ui::Tr("カメラからこの距離（m）までは近景マテリアルだけを貼る",
                                     "Up to this distance (m) from the camera, only the near material is applied"), "%.0f m");
            ui::PropertyFloat(ui::Tr("合成結果へ戻る距離", "Composite Beyond"), &m_renderer.NearFadeEnd(), 1.0f, 2000.0f,
                              renderer::kPreviewDefaults.nearFadeEnd,
                              ui::Tr("カメラからこの距離（m）より先は合成結果だけを貼る。上の距離との間で入れ替える",
                                     "Beyond this distance (m) from the camera, only the composite is applied. Blends between this and the distance above"), "%.0f m");
            ImGui::EndDisabled();
            ui::EndPropertyTable();
        }

        if (m_renderer.AtmosphericMode()) {
            auto& sky = m_renderer.AtmosphericSettings();
            const renderer::AtmosphereSettings defaults;
            ui::SectionHeader(ui::Tr("大気", "Atmosphere"));
            if (ui::BeginPropertyTable("atmosphereRows", ui::Tr("グラウンドアルベド", "Rayleigh Strength"))) {
                ui::PropertyBool(ui::Tr("背景を表示", "Show Background"), &m_renderer.ShowSkybox(), renderer::kPreviewDefaults.showSkybox,
                                 ui::Tr("空の背景を表示する。環境光と地形の手前の雲は残る",
                                        "Shows the sky background. Ambient light and clouds in front of terrain remain"));
                ui::PropertyFloat(ui::Tr("レイリー散乱強度", "Rayleigh Strength"), &sky.density, 0.1f, 3.0f, defaults.density,
                                  ui::Tr("空の青さや夕焼けを生む大気の散乱量。1 が基準です。\n"
                                         "大きくすると散乱と太陽光の減衰が強くなり、空の色と地形の照明が変わります。",
                                         "Atmospheric scattering that makes blue skies and sunsets. 1 is the reference.\n"
                                         "Higher strengthens scattering and sunlight attenuation, changing the sky color and terrain lighting."));
                ui::PropertyFloat(ui::Tr("ミー密度", "Mie Density"), &sky.mie, 0.0f, 2.0f, defaults.mie,
                                  ui::Tr("ミー散乱と吸収をまとめて変える粒子密度の倍率。大きくすると太陽の周囲や地平線が白っぽく霞み、直射光が弱まります。\n"
                                         "地形を距離に応じて隠すフォグとは別の設定です。",
                                         "Particle density multiplier for Mie scattering and absorption together. Higher makes the area around the sun and the horizon hazy white and weakens direct light.\n"
                                         "Separate from the fog that hides terrain with distance."));
                ui::PropertyFloat(ui::Tr("ミー異方性", "Mie Anisotropy"), &sky.eccentricity, 0.0f, 0.95f, defaults.eccentricity,
                                  ui::Tr("ミー散乱の異方性（g）。光が太陽の方向へ集中する度合い。\n"
                                         "大きいほど太陽付近の光が鋭く集中し、小さいほど広い方向へ散らばります。",
                                         "Anisotropy of Mie scattering (g): how much light concentrates toward the sun.\n"
                                         "Higher concentrates light sharply around the sun; lower spreads it more widely."));
                ui::PropertyFloat(ui::Tr("地表の基準標高", "Ground Altitude"), &sky.altitude, 0.0f, 10000.0f, defaults.altitude,
                                  ui::Tr("地形の原点の海抜高度（m）。空と環境光を計算する基準です。\n"
                                         "高くすると上空の薄い大気を通した空になります。地形やカメラ自体は移動しません。",
                                         "Altitude of the terrain origin above sea level (m), the reference for computing the sky and ambient light.\n"
                                         "Higher gives a sky seen through thinner air aloft. The terrain and camera themselves do not move."), "%.0f m");
                int lowerHemisphere = static_cast<int>(sky.lowerHemisphere);
                static const ui::LocalizedText lowerHemisphereLabels[]{{"空の延長", "Sky Extension"}, {"地面反射", "Ground Reflection"}};
                if (ui::PropertyCombo(ui::Tr("下半球", "Lower Hemisphere"), &lowerHemisphere, ui::Tr(lowerHemisphereLabels).data(), 2,
                                      static_cast<int>(defaults.lowerHemisphere),
                                      ui::Tr("空の延長：青空を下半球へ折り返す初期の表現。地面反射：日光と天空光を受けた地表の反射。\n"
                                             "背景・環境光・反射・雲の環境照明に共通で適用します。空の延長は見た目のための近似です。",
                                             "Sky Extension: the original look that mirrors the blue sky into the lower hemisphere. Ground Reflection: light reflected by ground lit by sun and sky.\n"
                                             "Applies to the background, ambient light, reflections and cloud ambient lighting alike. Sky Extension is an approximation for looks.")))
                    sky.lowerHemisphere = static_cast<uint32_t>(lowerHemisphere);
                ui::PropertyFloat(ui::Tr("グラウンドアルベド", "Ground Albedo"), &sky.groundAlbedo, 0.0f, 1.0f, defaults.groundAlbedo,
                                  ui::Tr("地面反射では直射光と天空光を反射する割合。空の延長では青みを保って下半球の明るさを調整します。\n"
                                         "どちらも大気の多重散乱へ反映します。\n"
                                         "0 は暗く、1 は強く反射します。地形マテリアルの色や反射率自体は変えません。",
                                         "With Ground Reflection, the fraction of direct light and skylight reflected. With Sky Extension, adjusts lower-hemisphere brightness while keeping the blue tint.\n"
                                         "Both feed into multiple scattering in the atmosphere.\n"
                                         "0 is dark, 1 reflects strongly. Does not change the color or reflectance of terrain materials."));
                ui::EndPropertyTable();
            }
            ui::SectionHeader(ui::Tr("月と星空", "Moon & Stars"));
            if (ui::BeginPropertyTable("nightSkyRows", ui::Tr("満月時の照度", "Star Brightness"))) {
                bool night = sky.nightEnabled != 0;
                if (ui::PropertyBool(ui::Tr("夜空を有効", "Night Sky"), &night, defaults.nightEnabled != 0,
                    ui::Tr("日没後に月と星を表示し、月明かりで地形と雲を照らします。\n"
                           "太陽高度 0〜-6 度で夜の照明へ移行します。昼夜の主光源と影は各1つです。",
                           "Shows the moon and stars after sunset and lights terrain and clouds with moonlight.\n"
                           "Switches to night lighting as the sun goes from 0 to -6 degrees. Day and night each have one main light and shadow.")))
                    sky.nightEnabled = night ? 1u : 0u;
                ImGui::BeginDisabled(!night);
                float azimuth = RadiansToDegrees(sky.moonAzimuth);
                float elevation = RadiansToDegrees(sky.moonElevation);
                float rotation = RadiansToDegrees(sky.starRotation);
                ImGui::BeginDisabled(dateTimeMode);
                if (ui::PropertyFloat(ui::Tr("月の方位角", "Moon Azimuth"), &azimuth, -180.0f, 180.0f, RadiansToDegrees(defaults.moonAzimuth),
                    ui::Tr("月と月明かりの方向。日時による天体位置の計算は行いません。",
                           "Direction of the moon and moonlight. Not computed from the date and time."), "%.1f deg"))
                    sky.moonAzimuth = DegreesToRadians(azimuth);
                if (ui::PropertyFloat(ui::Tr("月の仰角", "Moon Elevation"), &elevation, -89.0f, 89.0f, RadiansToDegrees(defaults.moonElevation),
                    ui::Tr("地平線より下の月は地球によって遮られます。", "A moon below the horizon is blocked by the Earth."), "%.1f deg"))
                    sky.moonElevation = DegreesToRadians(elevation);
                ui::PropertyFloat(ui::Tr("月相", "Moon Phase"), &sky.moonPhase, 0.0f, 1.0f, defaults.moonPhase,
                    ui::Tr("0: 新月、0.5: 半月、1: 満月。見える明部の面積と、月明かりの照度が変わります。",
                           "0: new moon, 0.5: half moon, 1: full moon. Changes the visible lit area and the moonlight illuminance."), "%.2f");
                ImGui::EndDisabled();
                ui::PropertyFloat(ui::Tr("満月時の照度", "Full Moon Lux"), &sky.moonIlluminance, 0.0f, 1.0f, defaults.moonIlluminance,
                    ui::Tr("大気による減衰前の満月の照度（lux）。実際の光量は月相・仰角・大気で減衰します。",
                           "Full moon illuminance before atmospheric attenuation (lux). The actual light is reduced by phase, elevation and atmosphere."), "%.3f lux");
                ui::PropertyFloat(ui::Tr("星の明るさ", "Star Brightness"), &sky.starIntensity, 0.0f, 100.0f, defaults.starIntensity,
                    ui::Tr("星表（Yale Bright Star Catalog、約 9100 星）の実測等級に対する倍率。1 で実際の明るさです。\n"
                           "雲に隠れ、露出によって見え方が変わります。",
                           "Multiplier on the measured magnitudes of the star catalog (Yale Bright Star Catalog, about 9100 stars). 1 is the real brightness.\n"
                           "Stars are hidden by clouds, and how they look depends on exposure."), "%.2f", ImGuiSliderFlags_Logarithmic);
                ImGui::BeginDisabled(dateTimeMode);
                if (ui::PropertyFloat(ui::Tr("星空の回転", "Star Rotation"), &rotation, -180.0f, 180.0f, RadiansToDegrees(defaults.starRotation),
                    ui::Tr("地方恒星時に相当する回転。この赤経の星が南中します。緯度は「観測地と日時」で設定します。",
                           "Rotation equal to local sidereal time; stars at this right ascension cross the meridian. Latitude is set in \"Location & Time\"."), "%.1f deg"))
                    sky.starRotation = DegreesToRadians(rotation);
                ImGui::EndDisabled();
                ImGui::EndDisabled();
                ui::EndPropertyTable();
            }
            ui::HintText("%s", ui::Tr("夜景は露出を調整してください。昼の露出では月明かりの地形は暗く写ります。",
                                      "Adjust exposure for night scenes. With daytime exposure, moonlit terrain looks dark."));
            ui::SectionHeader(ui::Tr("環境光", "Ambient Light"));
            if (ui::BeginPropertyTable("atmosphericEnvironmentRows", ui::Tr("スカイライト強度", "Skylight"))) {
                ui::PropertyFloat(ui::Tr("スカイライト強度", "Skylight"), &m_renderer.AtmosphericEnvironmentIntensity(),
                                  0.0f, 8.0f, renderer::PreviewRenderer::DefaultSkylightIntensity,
                                  ui::Tr("空と地面反射から地形・マテリアル・雲へ届く環境光の倍率。1 は大気計算そのままです。\n"
                                         "上げると陰側・環境反射・雲の天空照明を明るくします。太陽の直射光・背景の空・露出は変えません。\n"
                                         "1 以外は照明バランスを調整するための補正です。局所的な地形の照り返しを計算する GI とは異なります。",
                                         "Multiplier on ambient light reaching terrain, materials and clouds from the sky and ground reflection. 1 is the atmosphere result as is.\n"
                                         "Higher brightens shaded sides, environment reflections and cloud sky lighting. Does not change direct sunlight, the background sky or exposure.\n"
                                         "Values other than 1 are corrections for lighting balance. This is not GI, which computes local bounce light from terrain."));
                ui::EndPropertyTable();
            }
            ui::SectionHeader(ui::Tr("雲", "Clouds"));
            const bool nodeCloud = m_graph.CompileCloud().hasOutput;
            if (nodeCloud) ui::HintText("%s", ui::Tr("雲はグラフの Cloud Noise・Cloud Layer・Cloud Output で設定します",
                                                     "Clouds are set with Cloud Noise, Cloud Layer and Cloud Output in the graph"));
            if (!nodeCloud && ui::BeginPropertyTable("cloudRows")) {
                bool clouds = sky.clouds != 0;
                if (ui::PropertyBool(ui::Tr("雲を描画", "Draw Clouds"), &clouds, defaults.clouds != 0,
                                     ui::Tr("立体的な雲を描画し、環境光と反射にも反映します。\n"
                                            "ライトの「影」がオンなら地形に雲影も落とします。オフにしても雲の設定は保持されます。",
                                            "Draws volumetric clouds and includes them in ambient light and reflections.\n"
                                            "If the light's Shadows is on, also casts cloud shadows on terrain. Turning this off keeps the cloud settings."))) sky.clouds = clouds ? 1u : 0u;
                if (clouds) {
                    ui::PropertyFloat("Indirect Light", &sky.indirectLight, 0.0f, 1.0f, defaults.indirectLight,
                        ui::Tr("雲の中で繰り返し散乱する太陽光の反射率。1 で各次数のエネルギーが単散乱と同じ上限、\n"
                "0 は太陽光の多重散乱なし。光を足す方向には働かず、密度・透過率・地形への雲影は変えません。",
                               "Reflectance of sunlight scattered repeatedly inside clouds. At 1, each order is capped at the energy of single scattering;\n"
                "0 means no multiple scattering of sunlight. It never adds light, and does not change density, transmittance or cloud shadows on terrain."), "%.2f");
                    ui::PropertyFloat(ui::Tr("多重散乱の広がり", "Scatter Spread"), &sky.scatterSpread, 0.05f, 0.9f, defaults.scatterSpread,
                        ui::Tr("多重散乱した光が雲の中へ行き渡る度合い。高次ほど太陽方向の減衰を (1-広がり) の累乗で縮めます。\n"
                "大きいほど陰の側まで光が回って柔らかく、小さいほど陰が締まります。エネルギーは増えません。",
                               "How far multiply scattered light spreads through the cloud. Higher orders shrink sun-direction attenuation by powers of (1 - spread).\n"
                "Higher lets light wrap into the shaded side for a softer look; lower tightens the shadows. Energy does not increase."), "%.2f");
                    ui::PropertyFloat("Ambient Light", &sky.ambientLight, 0.0f, 5.0f, defaults.ambientLight,
                        ui::Tr("雲が空と地面反射から受ける環境光の倍率。スカイライト強度に掛け合わせます。\n"
                               "0 は影響なし、1 は従来どおり。地形のスカイライト強度や雲の太陽光・密度は変えません。",
                               "Multiplier on ambient light the clouds receive from the sky and ground reflection. Multiplied with Skylight.\n"
                               "0 means no ambient light, 1 is the previous behavior. Does not change terrain Skylight or cloud sunlight and density."), "%.2f");
                    ui::PropertyLabel(ui::Tr("雲の配置", "Cloud Placement"),
                                      ui::Tr("現在の地形サイズと変位量から、山にかかる低い雲層へまとめて設定します。\n"
                                             "地形の四隅より少し外まで広げます。適用後は各値を個別に調整できます。",
                                             "Sets up a low cloud layer over the mountains from the current terrain size and displacement.\n"
                                             "Extends slightly beyond the terrain corners. Each value can be adjusted afterwards."));
                    if (ui::Button(ui::Tr("地形に合わせる", "Fit to Terrain"), ui::kWideButtonWidth)) {
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
                    ui::PropertyFloat(ui::Tr("雲量", "Coverage"), &sky.coverage, 0.0f, 1.0f, defaults.coverage,
                                      ui::Tr("雲のできる範囲を調整します。大きいほど雲が増えてつながり、小さいほど晴れ間が増えます。\n"
                                             "空を覆う面積の割合そのものではありません。",
                                             "Adjusts where clouds form. Higher gives more, connected clouds; lower gives more clear gaps.\n"
                                             "It is not the actual fraction of sky covered."));
                    ui::PropertyFloat(ui::Tr("消散係数", "Extinction"), &sky.extinction, 0.0001f, 0.2f, defaults.extinction,
                                      ui::Tr("雲の中を進む光の減衰の強さ（1/m）。大きいほど光を通しにくく、雲と雲影が濃くなります。\n"
                                             "雲の範囲は「雲量」、上下の寸法は「厚さ」で調整します。",
                                             "How strongly light is attenuated inside clouds (1/m). Higher lets less light through, making clouds and cloud shadows denser.\n"
                                             "Adjust the cloud extent with \"Coverage\" and the vertical size with \"Thickness\"."), "%.4f");
                    ui::PropertyFloat(ui::Tr("雲底", "Cloud Base"), &sky.cloudBottom, -10000.0f, 10000.0f, defaults.cloudBottom,
                                      ui::Tr("地形の原点から雲の下端までの高さ（m）。山頂からの高さではありません。\n"
                                             "負の値で原点より下へ下げられます。山にかけるときは厚さも小さくします。",
                                             "Height from the terrain origin to the bottom of the clouds (m), not from the summit.\n"
                                             "Negative values move it below the origin. When draping clouds over mountains, reduce the thickness too."), "%.0f m");
                    ui::PropertyFloat(ui::Tr("厚さ", "Thickness"), &sky.cloudThickness, 10.0f, 6000.0f, defaults.cloudThickness,
                                      ui::Tr("雲層の上下方向の厚さ（m）。雲の上端は「雲底 + 厚さ」です。\n"
                                             "厚くすると光が通る雲の距離が増え、同じ消散係数でも光を遮りやすくなります。",
                                             "Vertical thickness of the cloud layer (m). The cloud top is \"Cloud Base + Thickness\".\n"
                                             "Thicker clouds lengthen the path light travels through them, blocking more light at the same extinction."), "%.0f m");
                    const char* noiseTypes[] = {"Perlin fBM", "Perlin-Worley"};
                    int noiseType = static_cast<int>(sky.cloudNoiseType);
                    if (ui::PropertyCombo(ui::Tr("ノイズの種類", "Noise Type"), &noiseType, noiseTypes, 2,
                        static_cast<int>(defaults.cloudNoiseType),
                        ui::Tr("Perlin fBM は従来の模様。Perlin-Worley は丸い細胞状の膨らみを使います。",
                               "Perlin fBM is the original pattern. Perlin-Worley uses rounded, cell-like puffs.")))
                        sky.cloudNoiseType = static_cast<uint32_t>(noiseType);
                    ui::PropertyFloat(ui::Tr("ノイズスケール", "Noise Scale"), &sky.cloudScale, 10.0f, 40000.0f, defaults.cloudScale,
                                      ui::Tr("雲模様の水平方向の大きさ（m）。大きくすると大きな雲塊、小さくすると細かな雲になります。\n"
                                             "雲を描く範囲の端や、雲量を変える設定ではありません。",
                                             "Horizontal size of the cloud pattern (m). Larger gives big cloud masses; smaller gives fine clouds.\n"
                                             "It does not set the edge of the cloud area or the coverage."), "%.0f m");
                    ui::PropertyFloat(ui::Tr("範囲の中心 X", "Area Center X"), &sky.fieldCenterX, -200000.0f, 200000.0f, defaults.fieldCenterX,
                                      ui::Tr("雲が存在する円形範囲の中心（ワールド座標）。カメラを移動しても範囲は動きません。",
                                             "Center of the circular cloud area (world coordinates). The area stays put when the camera moves."), "%.0f m");
                    ui::PropertyFloat(ui::Tr("範囲の中心 Z", "Area Center Z"), &sky.fieldCenterZ, -200000.0f, 200000.0f, defaults.fieldCenterZ,
                                      ui::Tr("雲が存在する円形範囲の中心（ワールド座標）。雲本体・雲影・環境光で同じ範囲を使います。",
                                             "Center of the circular cloud area (world coordinates). Clouds, cloud shadows and ambient light share the same area."), "%.0f m");
                    ui::PropertyFloat(ui::Tr("雲の分布半径", "Area Radius"), &sky.fieldRadius, 1.0f, 200000.0f, defaults.fieldRadius,
                                      ui::Tr("雲が存在する水平範囲の半径。雲模様の大きさは「ノイズスケール」で調整します。",
                                             "Radius of the horizontal cloud area. Adjust the pattern size with \"Noise Scale\"."), "%.0f m");
                    ui::PropertyFloat(ui::Tr("境界フェード幅", "Edge Fade"), &sky.fieldFalloff, 1.0f, 50000.0f, defaults.fieldFalloff,
                                      ui::Tr("範囲の外縁へ向けて密度をゼロにする幅。半径を超える値は半径として扱います。",
                                             "Width over which density fades to zero toward the outer edge. Values above the radius are treated as the radius."), "%.0f m");
                    bool animate = sky.animateClouds != 0;
                    if (ui::PropertyBool(ui::Tr("アニメーション", "Animation"), &animate, defaults.animateClouds != 0,
                                         ui::Tr("風で雲模様を流します。オフで一時停止。雲と雲影は同期し、環境光と反射は約 1 秒ごとに更新します。",
                                                "Moves the cloud pattern with the wind. Off pauses it. Clouds and cloud shadows stay in sync; ambient light and reflections update about once a second.")))
                        sky.animateClouds = animate ? 1u : 0u;
                    ui::PropertyFloat(ui::Tr("風速", "Wind Speed"), &sky.windSpeed, 0.0f, 1000.0f, defaults.windSpeed,
                                      ui::Tr("雲模様が進む速度。存在範囲の中心と半径は動きません。",
                                             "Speed at which the cloud pattern moves. The area center and radius do not move."), "%.1f m/s");
                    // 既定値も度に直して渡す（欄は度で見せている）。
                    float windDegrees = RadiansToDegrees(sky.windDirection);
                    if (ui::PropertyFloat(ui::Tr("風向", "Wind Direction"), &windDegrees, -180.0f, 180.0f, RadiansToDegrees(defaults.windDirection),
                                          ui::Tr("雲が進む方向。0 度は +Z、90 度は +X です。",
                                                 "Direction the clouds move. 0 degrees is +Z, 90 degrees is +X."), "%.0f deg"))
                        sky.windDirection = DegreesToRadians(windDegrees);
                    int seed = static_cast<int>(sky.seed);
                    if (ui::PropertyInt(ui::Tr("シード", "Seed"), &seed, 0, 10000, static_cast<int>(defaults.seed),
                                        ui::Tr("雲模様の乱数の種。値を変えると雲の形と配置が変わります。\n"
                                               "同じシードと設定なら同じ雲を再現できます。",
                                               "Random seed for the cloud pattern. Changing it changes the cloud shape and placement.\n"
                                               "The same seed and settings reproduce the same clouds."))) sky.seed = static_cast<uint32_t>(seed);
                }
                ui::EndPropertyTable();
            }
            ui::HintText("%s", ui::Tr("太陽に合わせて空・環境光・雲影が変わります",
                                      "The sky, ambient light and cloud shadows follow the sun"));
        }

        ui::SectionHeader(ui::Tr("露出", "Exposure"));
        renderer::ExposureSettings& exposure = m_renderer.Exposure();
        if (ui::BeginPropertyTable("exposureRows")) {
            static const ui::LocalizedText kExposureModes[] = {{"自動", "Auto"}, {"EV を直接指定", "Manual EV"}, {"物理カメラ", "Physical Camera"}};
            int exposureMode = exposure.automatic ? 0 : exposure.useManualEv ? 1 : 2;
            if (ui::PropertyCombo(ui::Tr("方式", "Mode"), &exposureMode, ui::Tr(kExposureModes).data(), IM_ARRAYSIZE(kExposureModes),
                                  kDefaultExposure.automatic ? 0 : kDefaultExposure.useManualEv ? 1 : 2,
                                  ui::Tr("自動は画面の輝度ヒストグラムから EV100 を測り、露出補正を足します。\n"
                                         "EV を直接指定は値をそのまま使い、物理カメラは絞り / シャッター / ISO から EV100 を求めます。",
                                         "Auto meters EV100 from the screen luminance histogram and adds compensation.\n"
                                         "Manual EV uses the value as is; Physical Camera derives EV100 from aperture / shutter / ISO."))) {
                exposure.automatic = exposureMode == 0;
                exposure.useManualEv = exposureMode == 1;
            }
            if (exposure.automatic) {
                ui::PropertyFloat(ui::Tr("露出補正", "Compensation"), &exposure.compensation, -5.0f, 5.0f, kDefaultExposure.compensation,
                                  ui::Tr("測った EV100 に足す値。正で暗く、負で明るくなります。",
                                         "Added to the metered EV100. Positive darkens, negative brightens."), "%.2f EV");
                ui::PropertyFloat(ui::Tr("EV の下限", "Min EV"), &exposure.minEv100, -10.0f, 20.0f, kDefaultExposure.minEv100,
                                  ui::Tr("夜景で露出が上がりすぎないように止める値。",
                                         "Keeps exposure from rising too far in night scenes."), "%.1f");
                ui::PropertyFloat(ui::Tr("EV の上限", "Max EV"), &exposure.maxEv100, -10.0f, 20.0f, kDefaultExposure.maxEv100,
                                  ui::Tr("日中で露出が下がりすぎないように止める値。",
                                         "Keeps exposure from dropping too far in daylight."), "%.1f");
                ui::PropertyFloat(ui::Tr("順応の速さ", "Adaptation Speed"), &exposure.adaptationSpeed, 0.1f, 20.0f, kDefaultExposure.adaptationSpeed,
                                  ui::Tr("1 秒あたりの追従率。大きいほど明るさの変化にすぐ追従します。",
                                         "Tracking rate per second. Higher follows brightness changes faster."), "%.1f /s",
                                  ImGuiSliderFlags_Logarithmic);
                ui::PropertyValue(ui::Tr("測光 EV100", "Metered EV100"), exposure.autoValid ? "%.2f" : ui::Tr("測定中", "Measuring"),
                                  exposure.autoEv100);
            } else if (exposure.useManualEv) {
                ui::PropertyFloat("EV100", &exposure.manualEv100, -6.0f, 20.0f,
                                  kDefaultExposure.manualEv100, nullptr, "%.2f");
            } else {
                // **編集はカメラの節の「F 値」1 か所だけ。** ここは EV の内訳を
                // 読むための表示に留める（絞りはレンズの値で、ボケにも効くため）。
                ui::PropertyValue(ui::Tr("絞り", "Aperture"), ui::Tr("F%.1f（カメラ）", "F%.1f (Camera)"), exposure.aperture);

                float shutterDenominator = 1.0f / exposure.shutterSpeed;
                if (ui::PropertyFloat(ui::Tr("シャッター", "Shutter"), &shutterDenominator, 1.0f, 4000.0f,
                                      1.0f / kDefaultExposure.shutterSpeed,
                                      ui::Tr("秒の逆数。大きいほど暗くなる", "Reciprocal of seconds. Higher is darker"),
                                      ui::Tr("1/%.0f 秒", "1/%.0f s"),
                                      ImGuiSliderFlags_Logarithmic)) {
                    exposure.shutterSpeed = 1.0f / shutterDenominator;
                }
                ui::PropertyFloat("ISO", &exposure.iso, 50.0f, 6400.0f, kDefaultExposure.iso,
                                  ui::Tr("感度。大きいほど明るくなる", "Sensitivity. Higher is brighter"), "%.0f",
                                  ImGuiSliderFlags_Logarithmic);
            }
            ui::PropertyValue("EV100", "%.2f  (exposure %.3e)", exposure.Ev100(),
                              exposure.Exposure());
            ui::EndPropertyTable();
        }

        // **環境そのもの（何を空にするか）はシーンの天球（1 つ）が持つ。**
        // ここに残すのは、天球ではなく見え方に属する設定だけ。差し替えの入口だけ置く。
        if (!m_renderer.AtmosphericMode()) {
            ui::SectionHeader(ui::Tr("作業用IBL", "Work IBL"));
            if (ui::BeginPropertyTable("iblRows")) {
                const renderer::SkyAsset* activeSky = m_skyLibrary.Active();
                ui::PropertyLabel(ui::Tr("天球", "Sky"));
                ImGui::TextUnformatted(activeSky ? activeSky->name.c_str() : "-");
                DrawAssetSourceButton(activeSky ? activeSky->assetPath : std::filesystem::path{}, m_pendingAssetReveal);
                ImGui::SameLine();
                if (ui::Button(ui::Tr("選ぶ…", "Choose…"))) OpenSkyPicker();
                ui::PropertyEnd();
                ui::PropertyValue(ui::Tr("環境", "Environment"), "%s", m_renderer.GetEnvironment().SourceName().c_str());
                ui::PropertyValue("equirect", "%u x %u", m_renderer.GetEnvironment().EquirectWidth(),
                                  m_renderer.GetEnvironment().EquirectHeight());
                ui::PropertyBool(ui::Tr("背景を表示", "Show Background"), &m_renderer.ShowSkybox(),
                                 renderer::kPreviewDefaults.showSkybox,
                                 ui::Tr("オフにすると背景色だけになる。IBL の寄与は残る",
                                        "When off, only the background color shows. The IBL contribution remains"));
                ImGui::BeginDisabled(!m_renderer.ShowSkybox());
                ui::PropertyBool(ui::Tr("背景をぼかす", "Blur Background"), &m_renderer.SkyboxBlur(),
                                 renderer::kPreviewDefaults.skyboxBlur,
                                 ui::Tr("背景だけを柔らかくする。素材を見比べるときに、"
                                        "背景の細部が目移りの原因にならないようにする。"
                                        "IBL の寄与と陰影は変わらない",
                                        "Softens only the background, so its details do not distract "
                                        "when comparing materials. "
                                        "The IBL contribution and shading do not change"));
                ImGui::EndDisabled();
                ui::EndPropertyTable();
            }
            ui::HintText("%s", ui::Tr("作業用IBLの設定は「作業用IBL」ウィンドウで行う。.tgskyを開いてもシーンのスカイは変更しない",
                                      "Work IBL settings are in the \"Work IBL\" window. Opening a .tgsky does not change the scene sky"));
        }

        ui::SectionHeader(ui::Tr("トーンマップ", "Tone Mapping"));
        if (ui::BeginPropertyTable("tonemapRows")) {
            static const ui::LocalizedText kTonemapLabels[] = {{"なし", "None"}, {"Reinhard", "Reinhard"}, {"ACES", "ACES"}};
            int tonemap = static_cast<int>(m_renderer.Tonemap());
            if (ui::PropertyCombo(ui::Tr("方式", "Mode"), &tonemap, ui::Tr(kTonemapLabels).data(), IM_ARRAYSIZE(kTonemapLabels),
                                  static_cast<int>(renderer::kPreviewDefaults.tonemap))) {
                m_renderer.Tonemap() = static_cast<renderer::TonemapMode>(tonemap);
            }
            ui::EndPropertyTable();
        }
    }
    const auto& workLight = m_renderer.LegacyLight();
    if (workLightBefore.azimuth != workLight.azimuth || workLightBefore.elevation != workLight.elevation ||
        workLightBefore.illuminance != workLight.illuminance || workLightBefore.color.x != workLight.color.x ||
        workLightBefore.color.y != workLight.color.y || workLightBefore.color.z != workLight.color.z)
        m_pendingWorkEnvironmentSave = true;
    const auto& sun = m_renderer.AtmosphericLight();
    const auto& sky = m_renderer.AtmosphericSettings();
    if (sunBefore.azimuth != sun.azimuth || sunBefore.elevation != sun.elevation || sunBefore.illuminance != sun.illuminance ||
        skyBefore.density != sky.density || skyBefore.mie != sky.mie || skyBefore.eccentricity != sky.eccentricity ||
        skyBefore.altitude != sky.altitude || skyBefore.groundAlbedo != sky.groundAlbedo ||
        skyBefore.nightEnabled != sky.nightEnabled ||
        skyBefore.moonAzimuth != sky.moonAzimuth ||
        skyBefore.moonElevation != sky.moonElevation ||
        skyBefore.moonIlluminance != sky.moonIlluminance ||
        skyBefore.moonPhase != sky.moonPhase ||
        skyBefore.starIntensity != sky.starIntensity ||
        skyBefore.starRotation != sky.starRotation ||
        skyBefore.starLatitude != sky.starLatitude ||
        std::memcmp(&celestialBefore, &m_renderer.Celestial(), sizeof(celestialBefore)) != 0 ||
        skyBefore.lowerHemisphere != sky.lowerHemisphere || skylightBefore != m_renderer.AtmosphericEnvironmentIntensity())
        MarkDocumentChanged(false);
    ImGui::End();
}

void Application::ApplyCelestialSettings() {
    const auto& celestial = m_renderer.Celestial();
    if (celestial.mode != 1) return;
    auto& sun = m_renderer.AtmosphericLight();
    auto& sky = m_renderer.AtmosphericSettings();
    const auto state = renderer::ComputeCelestialState(celestial, sky.starLatitude);
    sun.azimuth = state.sun.azimuth;
    sun.elevation = std::clamp(state.sun.elevation, -1.55334f, 1.55334f);
    sky.moonAzimuth = state.moon.azimuth;
    sky.moonElevation = std::clamp(state.moon.elevation, -1.55334f, 1.55334f);
    sky.moonPhase = state.moonPhase;
    // 星空の回転は [-pi, pi] で保存するので、地方恒星時 [0, 2pi) を折り返す。
    sky.starRotation = state.siderealTime > 3.1415927f ? state.siderealTime-6.2831853f : state.siderealTime;
}

}  // namespace tg
