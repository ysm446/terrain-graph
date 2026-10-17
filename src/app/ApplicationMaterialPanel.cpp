// マテリアルパネル。**ライブラリの一覧（サムネイル）だけ**を置く。
// マテリアルの設定は、球を見ながら触れるプレビューの窓（この下）が持つ。

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

namespace {
// GPU資源を共有せず、編集可能な値だけを作業用コピーへ移す。
void CopyMaterialValues(const compositor::MaterialAsset& source, compositor::MaterialAsset& target) {
    target.id = source.id;
    target.name = source.name;
    target.assetPath = source.assetPath; target.assetUid = source.assetUid;
    target.layerMaterial = source.layerMaterial;
    target.baseColor = source.baseColor;
    target.normal = source.normal;
    target.roughness = source.roughness;
    target.metallic = source.metallic;
    target.ambientOcclusion = source.ambientOcclusion;
    target.height = source.height;
    target.baseColorTint = source.baseColorTint;
    target.hueShiftDegrees = source.hueShiftDegrees;
    target.saturation = source.saturation;
    target.brightness = source.brightness;
    target.roughnessValue = source.roughnessValue;
    target.metallicValue = source.metallicValue;
    target.ambientOcclusionValue = source.ambientOcclusionValue;
    target.flipNormalGreen = source.flipNormalGreen;
    target.blendMode = source.blendMode;
    target.maskThreshold = source.maskThreshold;
    target.opacity = source.opacity;
    target.opacityValue = source.opacityValue;
    target.twoSided = source.twoSided;
    target.colorVariation = source.colorVariation;
}
}  // namespace

void Application::CommitMaterialEdit() {
    if (!m_materialEditPending) return;
    if (auto* asset = m_materialLibrary.FindMutable(m_materialEditDraft.id)) {
        CopyMaterialValues(m_materialEditDraft, *asset);
        if (m_materialEditAppearanceChanged) {
            // **ここで組み直す。** 描画用の合成結果はフレームの前でしか作り直さないので、
            // 確定だけして帰ると、このフレームは編集前の見た目に戻って 1 枚ちらつく。
            // 組み直しは CPU だけの処理なので、フレームの中で呼んでよい。
            if (asset->layerMaterial)
                asset->layerGpu = m_materialLibrary.CompileLayerMaterial(*asset, m_textureLibrary, asset->layerError);
            m_materialLibrary.MarkThumbnailDirty(asset->id);
            MarkDocumentChanged();
        } else {
            // 改名は保存・履歴だけを更新し、描画キャッシュには触れない。
            m_documentDirty = true;
        }
    }
    m_materialEditPending = false;
    m_materialEditAppearanceChanged = false;
}

// マテリアル 1 つのプロパティ（基本 + マップ）。**置き場所はプレビューの窓だけ。**
// 一覧はサムネイルだけを出し、値の調整は球を見ながらやる。
// 窓の描画から切り出してあるのは、球の操作と行の並びを読み分けられるようにするため。
bool Application::DrawMaterialProperties(compositor::MaterialAsset& asset) {
    if (asset.layerMaterial) return DrawLayerMaterialProperties(asset);
    bool changed = false;

    ui::SectionHeader(ui::Tr("基本", "General"));
    if (ui::BeginPropertyTable("materialBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", asset.name.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), nameBuffer, sizeof(nameBuffer),
                                  ui::Tr("ファイル名（拡張子なし）と同じ。変えるとファイルも改名する", "Same as the file name (without extension). Changing it renames the file"))) {
            if (asset.assetPath.empty()) {
                // まだファイルが無い。初回保存時にこの名前でファイルを作る。
                asset.name = nameBuffer;
                m_materialEditPending = true;
            } else {
                RequestAssetRename(asset.assetPath, nameBuffer);
            }
        }

        static const compositor::MaterialAsset kDefaultAsset;
        // マップ節にも「ベースカラー」の行（テクスチャ)があるため、
        // ここは掛ける色だと分かる名前にする。
        changed |= ui::PropertyColorLinear(
            ui::Tr("ティント", "Tint"), &asset.baseColorTint.x, &kDefaultAsset.baseColorTint.x,
            ui::Tr("ベースカラーのマップに掛ける色。白ならマップそのまま。マップが無ければこの色になる", "Color multiplied into the base color map. White leaves the map as is. Without a map, this is the color"));
        // **ベースカラーだけの調整。** ティントでは彩度を上げられず色相も回せないので、
        // 素材を馴染ませる操作をここに置く。掛ける色（ティント）のあとに効く。
        changed |= ui::PropertyFloat(
            ui::Tr("色相", "Hue"), &asset.hueShiftDegrees, -180.0f, 180.0f, kDefaultAsset.hueShiftDegrees,
            ui::Tr("ベースカラーの色みを回す（度）。灰色は灰色のまま残る", "Rotates the base color hue (degrees). Grays stay gray"), ui::Tr("%.0f 度", "%.0f°"));
        changed |= ui::PropertyFloat(ui::Tr("彩度", "Saturation"), &asset.saturation, 0.0f, 2.0f,
                                     kDefaultAsset.saturation,
                                     ui::Tr("ベースカラーの鮮やかさ。0 で白黒、1 でそのまま", "Base color vividness. 0 is grayscale, 1 is unchanged"), "%.2f");
        changed |= ui::PropertyFloat(ui::Tr("明度", "Brightness"), &asset.brightness, 0.0f, 2.0f,
                                     kDefaultAsset.brightness,
                                     ui::Tr("ベースカラーの明るさ（倍率）。1 でそのまま、0 で黒。結果は 0〜1 に収める", "Base color brightness (multiplier). 1 is unchanged, 0 is black. The result is clamped to 0-1"), "%.2f");
        changed |= ui::PropertyFloat(ui::Tr("ラフネス", "Roughness"), &asset.roughnessValue, 0.0f, 1.0f,
                                     kDefaultAsset.roughnessValue, ui::Tr("マップが無いときの値", "Value used when there is no map"),
                                     "%.2f");
        changed |= ui::PropertyFloat(ui::Tr("メタルネス", "Metalness"), &asset.metallicValue, 0.0f, 1.0f,
                                     kDefaultAsset.metallicValue, ui::Tr("マップが無いときの値", "Value used when there is no map"),
                                     "%.2f");
        changed |= ui::PropertyFloat("AO", &asset.ambientOcclusionValue, 0.0f, 1.0f,
                                     kDefaultAsset.ambientOcclusionValue, ui::Tr("マップが無いときの値", "Value used when there is no map"),
                                     "%.2f");
        ui::EndPropertyTable();
    }

    ui::SectionHeader(ui::Tr("マップ", "Maps"),
                      ui::Tr("ORD は AO=R / ラフネス=G / ハイト=B に割り当てる（Megascans の並び）" "\n"
                      "ハイトはレイヤーの「ハイトのソース」をテクスチャにすると効く",
                      "ORD assigns AO=R / Roughness=G / Height=B (the Megascans layout)" "\n"
                      "Height takes effect when the layer's height source is set to Texture"));
    if (ui::BeginPropertyTable("materialMapRows")) {
        changed |= DrawTextureSlotRow(ui::Tr("ベースカラー", "Base Color"), asset.baseColor, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawTextureSlotRow(ui::Tr("法線", "Normal"), asset.normal, m_textureLibrary, m_pendingAssetReveal);
        if (asset.normal != compositor::kNoTexture) {
            changed |= ui::PropertyBool(
                ui::Tr("緑を反転", "Flip Green"), &asset.flipNormalGreen, true,
                ui::Tr("法線マップの規約。OpenGL（Megascans などの既定）は入、"
                       "DirectX 規約の素材は切る。切り替えて陰影が自然なほうが正しい",
                       "Normal map convention. On for OpenGL (the default for Megascans etc.), "
                       "off for DirectX-convention materials. Whichever shades naturally is correct"));
        }
        changed |= DrawMapSlotRow(ui::Tr("ラフネス", "Roughness"), asset.roughness, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawMapSlotRow(ui::Tr("メタルネス", "Metalness"), asset.metallic, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawMapSlotRow("AO", asset.ambientOcclusion, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawMapSlotRow(ui::Tr("ハイト", "Height"), asset.height, m_textureLibrary, m_pendingAssetReveal);

        // 1 枚に AO / ラフネス / ハイトを詰めたテクスチャをまとめて割り当てる。
        ui::PropertyLabel("ORD", ui::Tr("1 枚に AO / ラフネス / ハイトを詰めたテクスチャ", "A texture packing AO / roughness / height into one"));
        const float ordButtonWidth = ui::Scaled(ui::kButtonWidth);
        const float ordSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float ordComboWidth = std::max(
            ui::Scaled(60.0f),
            std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x) -
                ordButtonWidth - ordSpacing);
        DrawTextureCombo("##ord", m_ordTexture, m_textureLibrary, ordComboWidth, m_pendingAssetReveal);
        ImGui::SameLine(0.0f, ordSpacing);
        ImGui::BeginDisabled(m_ordTexture == compositor::kNoTexture);
        if (ui::Button(ui::Tr("割り当て", "Assign"))) {
            asset.ambientOcclusion = {m_ordTexture, compositor::TextureChannel::R};
            asset.roughness = {m_ordTexture, compositor::TextureChannel::G};
            asset.height = {m_ordTexture, compositor::TextureChannel::B};
            changed = true;
        }
        ImGui::EndDisabled();
        ui::PropertyEnd();

        ui::EndPropertyTable();
    }

    // 不透明度（アルファ抜き）。モデルと道路の帯（Lane Marking など）に効く。地形のレイヤー合成には使わない。
    ui::SectionHeader(ui::Tr("不透明度", "Opacity"),
                      ui::Tr("不透明度はマップから読み、無ければベースカラーのアルファを使う。地形のレイヤー合成には効かない", "Opacity is read from the map, or from the base color alpha if there is none. It does not affect terrain layer blending"));
    if (ui::BeginPropertyTable("materialOpacityRows", ui::Tr("不透明度マップ", "Opacity Map"))) {
        static const compositor::MaterialAsset kDefaultAsset;
        static const ui::LocalizedText kBlendModes[] = {
            {"不透明", "Opaque"}, {"切り抜き", "Masked"}, {"半透明", "Translucent"}};
        int mode = static_cast<int>(asset.blendMode);
        if (ui::PropertyCombo(ui::Tr("描き方", "Blend Mode"), &mode, ui::Tr(kBlendModes).data(), 3, 0,
                              ui::Tr("切り抜きは不透明度がしきい値未満の画素を描かない（影も抜ける）。"
                                     "半透明は不透明度で混ぜる（モデルでは切り抜きとして描く）",
                                     "Masked skips pixels whose opacity is below the threshold (shadows too). "
                                     "Translucent blends by opacity (drawn as Masked on models)"))) {
            asset.blendMode = static_cast<compositor::BlendMode>(mode);
            changed = true;
        }
        if (asset.blendMode == compositor::BlendMode::Masked) {
            changed |= ui::PropertyFloat(ui::Tr("しきい値", "Threshold"), &asset.maskThreshold, 0.0f, 1.0f, kDefaultAsset.maskThreshold,
                                         ui::Tr("不透明度がこの値未満の画素を描かない。目安は 0.5", "Pixels with opacity below this are not drawn. 0.5 is a good start"), "%.2f");
        }
        const compositor::TextureId previousOpacity = asset.opacity.texture;
        if (DrawMapSlotRow(ui::Tr("不透明度マップ", "Opacity Map"), asset.opacity, m_textureLibrary, m_pendingAssetReveal)) {
            changed = true;
            // 不透明のままマップを付けたら切り抜きにする（付けたのに効かない、を避ける）。
            if (previousOpacity == compositor::kNoTexture && asset.opacity.texture != compositor::kNoTexture &&
                asset.blendMode == compositor::BlendMode::Opaque)
                asset.blendMode = compositor::BlendMode::Masked;
        }
        if (asset.blendMode == compositor::BlendMode::Translucent && asset.opacity.texture == compositor::kNoTexture) {
            changed |= ui::PropertyFloat(ui::Tr("不透明度", "Opacity"), &asset.opacityValue, 0.0f, 1.0f, kDefaultAsset.opacityValue,
                                         ui::Tr("マップが無いときの不透明度（ベースカラーのアルファが無いとき）", "Opacity when there is no map (and no base color alpha)"), "%.2f");
        }
        ui::EndPropertyTable();
    }

    // モデルに割り当てたときだけ効く設定。地形のレイヤー合成には使わない。
    ui::SectionHeader(ui::Tr("モデル", "Model"),
                      ui::Tr("両面はモデルの描画だけに効く", "Two-Sided only affects model rendering"));
    if (ui::BeginPropertyTable("materialModelRows")) {
        static const compositor::MaterialAsset kDefaultAsset;
        changed |= ui::PropertyBool(
            ui::Tr("両面", "Two-Sided"), &asset.twoSided, kDefaultAsset.twoSided,
            ui::Tr("裏面も描く。裏から見たときは法線を反転して陰影を付ける。葉のカードなど厚みの無い面に使う", "Also draws back faces, flipping the normal for shading when seen from behind. For thin surfaces like leaf cards"));
        ui::EndPropertyTable();
    }

    // 配置の点の色むら（散布の Variation）への応え方。モデルの配置だけに効く。
    ui::SectionHeader(ui::Tr("色むら", "Color Variation"),
                      ui::Tr("散布の Variation に繋いだマスクの値（0〜1）で色を寄せる。0.5 で調整なし" "\n"
                      "葉のマテリアルに設定し、幹は既定のままにする。インポスターは作り直すと反映される",
                      "Shifts the color by the mask value (0-1) connected to Scatter's Variation. 0.5 means no change" "\n"
                      "Set it on leaf materials and leave trunks at the default. Impostors pick it up when rebuilt"));
    if (ui::BeginPropertyTable("materialVariationRows", ui::Tr("0 側の彩度", "Brightness at 0"))) {
        static const compositor::ColorVariation kDefaultVariation;
        auto& variation = asset.colorVariation;
        const auto end = [&](const char* side, float* hue, float* saturation, float* brightness,
                             float defaultHue, float defaultSaturation, float defaultBrightness) {
            const std::string prefix = std::string(side) + " 側の";
            // 英語は「Hue at 0」の順にする。
            const auto label = [&](const char* name, const char* english) {
                return ui::CurrentLanguage() == ui::Language::English ? std::string(english) + " at " + side
                                                                      : prefix + name;
            };
            changed |= ui::PropertyFloat(label("色相", "Hue").c_str(), hue, -180.0f, 180.0f, defaultHue,
                                         ui::Tr("色むらの値がこの端にある株で、ベースカラーの色みを回す（度）", "Rotates the base color hue (degrees) for plants whose variation value is at this end"),
                                         ui::Tr("%.0f 度", "%.0f°"));
            changed |= ui::PropertyFloat(label("彩度", "Saturation").c_str(), saturation, 0.0f, 2.0f,
                                         defaultSaturation,
                                         ui::Tr("色むらの値がこの端にある株の鮮やかさ。1 でそのまま", "Vividness of plants whose variation value is at this end. 1 is unchanged"), "%.2f");
            changed |= ui::PropertyFloat(label("明度", "Brightness").c_str(), brightness, 0.0f, 2.0f,
                                         defaultBrightness,
                                         ui::Tr("色むらの値がこの端にある株の明るさ（倍率）。1 でそのまま", "Brightness (multiplier) of plants whose variation value is at this end. 1 is unchanged"), "%.2f");
        };
        end("0", &variation.lowHueDegrees, &variation.lowSaturation, &variation.lowBrightness,
            kDefaultVariation.lowHueDegrees, kDefaultVariation.lowSaturation,
            kDefaultVariation.lowBrightness);
        end("1", &variation.highHueDegrees, &variation.highSaturation, &variation.highBrightness,
            kDefaultVariation.highHueDegrees, kDefaultVariation.highSaturation,
            kDefaultVariation.highBrightness);
        changed |= ui::PropertyFloat(
            ui::Tr("個体差", "Jitter"), &variation.jitter, 0.0f, 0.5f, kDefaultVariation.jitter,
            ui::Tr("株ごとの乱数で色むらの値をずらす幅（±）。同じ場所の株どうしにも差を付ける", "Per-plant random shift of the variation value (±). Differentiates plants in the same spot"), "%.2f");
        ui::EndPropertyTable();
    }

    return changed;
}

// 素材プレビューに重ねるライトのギズモ。**ビューポートと同じ絵**
// （`DrawLightGizmoOverlay`）で、L＋ドラッグの手応えを窓ごとに変えない。
//
// 視点はシェーダのレイ生成と同じ右手系で組む（`MaterialSphere.hlsl` の forward / right / up）。
// 球も平面も原点まわりの半径 1 なので、ギズモの半径は 1 でよい。
void Application::DrawMaterialSphereLightGizmo(const ImVec2& previewMin, const ImVec2& previewMax) {
    const double now = ImGui::GetTime();
    if (now >= m_materialLightGizmoUntil) {
        return;
    }
    const float fade = static_cast<float>(
        std::clamp((m_materialLightGizmoUntil - now) / kLightGizmoFadeSeconds, 0.0, 1.0));

    using namespace DirectX;
    const XMFLOAT3 cameraPosition = m_materialSphere.CameraPosition();
    const XMMATRIX view =
        XMMatrixLookAtRH(XMLoadFloat3(&cameraPosition), XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const float fovY = renderer::MaterialSphere::kFovYDegrees * (3.14159265358979f / 180.0f);
    // プレビューは正方形で描いているのでアスペクトは 1。
    const XMMATRIX projection = XMMatrixPerspectiveFovRH(fovY, 1.0f, 0.05f, 100.0f);

    const renderer::LightSettings light = m_materialSphere.PreviewLight(m_renderer.Light());
    char text[64] = {};
    std::snprintf(text, sizeof(text), ui::Tr("方位角 %.0f 度   仰角 %.0f 度", "Azimuth %.0f°   Elevation %.0f°"),
                  RadiansToDegrees(light.azimuth), RadiansToDegrees(light.elevation));
    const ImVec2 textMin(previewMin.x + ui::Scaled(10.0f), previewMin.y + ui::Scaled(10.0f));

    DrawLightGizmoOverlay(view * projection, previewMin, previewMax, 1.0f, light.azimuth,
                          light.elevation, light.Direction(), fade, text, textMin);
}

// マテリアルプレビューの窓。回せる球と、そのマテリアルのプロパティ。
//
// **映すのは一覧で選んでいるマテリアル。** 窓の側に別の選択を持たせると、
// 一覧で選んだものと窓の中身が食い違う。
void Application::DrawMaterialSphereWindow() {
    m_materialSphereVisible = false;
    if (!m_showMaterialSphere || HiddenWithAssetBand("###MaterialPreview")) {
        return;
    }

    const auto& assets = m_materialLibrary.Entries();
    const int index = std::clamp(m_selectedMaterial, 0, std::max(0, static_cast<int>(assets.size()) - 1));
    const bool layerMaterial = !assets.empty() && assets[index].layerMaterial.has_value();
    // 左にプレビュー、右に編集欄の横並び。どちらも潰れない幅を下限にする
    // （以前の縦並びの窓幅 420 が ini に残っていても、開いたときに広がる）。
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(1000.0f), ui::Scaled(720.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Scaled(640.0f), ui::Scaled(360.0f)),
                                        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(ui::Tr("マテリアルプレビュー###MaterialPreview", "Material Preview###MaterialPreview"), &m_showMaterialSphere,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    if (assets.empty()) {
        ui::HintText("%s", ui::Tr("マテリアルがない。「マテリアル」パネルの「追加」で作る", "No materials. Create one from the Materials panel"));
        ImGui::End();
        return;
    }

    m_materialSphereVisible = true;

    compositor::MaterialAsset& asset =
        *m_materialLibrary.FindMutable(assets[static_cast<size_t>(index)].id);

    // 左にプレビュー（球と表示の設定）、右に独立してスクロールする編集欄。
    // 左の区画はスクロールしないので、球の上のホイールをズームへ回せる。
    const float previewWidth = ImGui::GetContentRegionAvail().x * 0.48f;
    ImGui::BeginChild("materialPreviewPane", ImVec2(previewWidth, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    // 下に表示の設定（約 145px）を残した高さと幅の小さいほうで正方形にする。
    const float paneSize = std::max(ui::Scaled(32), std::min(ImGui::GetContentRegionAvail().x,
                                                             ImGui::GetContentRegionAvail().y - ui::Scaled(145)));

    // --- 球 ------------------------------------------------------------------
    ImGui::BeginChild("materialSpherePane", ImVec2(0.0f, paneSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // 横長の窓では幅より高さのほうが小さいので、そのときだけ横に余白が出る。
        // 余った幅は左右へ分けて、球を中央に置く。
        const float sphereSize =
            std::max(std::min(ImGui::GetContentRegionAvail().x, paneSize), ui::Scaled(32.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - sphereSize) * 0.5f));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + sphereSize, min.y + sphereSize);

        // 画像より先に ID を持つアイテムを置く（サムネイルと同じ作法）。
        ImGui::InvisibleButton("##materialSphere", ImVec2(sphereSize, sphereSize),
                               ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemActive()) {
            // 1px = 0.35 度。ビューポートのカメラ（0.006 ラジアン ≒ 0.34 度）に合わせる。
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            const auto& io = ImGui::GetIO();
            if (ImGui::IsKeyDown(ImGuiKey_L) && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
                m_materialSphere.RotateLight(delta.x, delta.y);
                // ビューポートと同じギズモを、掴んでいる間と離した直後だけ出す。
                m_materialLightGizmoUntil = ImGui::GetTime() + kLightGizmoFadeSeconds;
            }
            else m_materialSphere.Orbit(delta.x * 0.35f, delta.y * 0.35f);
        }
        // **寄るのはホイール。** この区画はスクロールしない（`NoScrollWithMouse`）ので、
        // ビューポートと同じようにホイールをズームへ回せる。
        // スクロールするのは下のプロパティの区画だけ。
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            m_materialSphere.Zoom(ImGui::GetIO().MouseWheel);
        }

        if (m_materialSphere.HasOutput()) {
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(m_materialSphere.OutputHandle().ptr), min, max);
        }
        ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border),
                                            ImGui::GetStyle().FrameRounding, 0, ui::Scaled(1.0f));

        DrawMaterialSphereLightGizmo(min, max);
    }
    ImGui::EndChild();

    ImGui::Separator();


    // 表示だけの設定。マテリアルの設定とは区切り線で分ける。
    if (ui::BeginPropertyTable("materialSphereViewRows")) {
        const char* shapes[]{ui::Tr("球", "Sphere"), ui::Tr("平面", "Plane")};
        int shape = m_materialSphere.Shape();
        if (ui::PropertyCombo(ui::Tr("形状", "Shape"), &shape, shapes, 2, 0, ui::Tr("プレビュー形状。マテリアルには保存しません", "Preview shape. Not saved to the material"))) m_materialSphere.SetShape(shape);
        // 長さは実寸（m）。平面は一辺、球は直径で、どちらも同じ値を使う
        // （球の赤道付近の模様が、同じ長さの平面と同じ大きさで見える）。
        const bool plane = m_materialSphere.Shape() == 1;
        ui::PropertyFloat(plane ? ui::Tr("一辺", "Side Length") : ui::Tr("直径", "Diameter"), &m_materialSphere.LengthMeters(), 0.1f, 100.0f, 2.0f,
                          plane ? ui::Tr("映す平面の一辺の長さ（m）。マテリアルには保存しない", "Side length of the preview plane (m). Not saved to the material")
                                : ui::Tr("映す球の直径（m）。赤道の模様が同じ長さの平面と揃う。マテリアルには保存しない", "Diameter of the preview sphere (m). Its equator pattern matches a plane of the same length. Not saved to the material"),
                          "%.2f m");
        ui::PropertyBool(ui::Tr("変位を表示", "Show Displacement"), &m_materialSphere.ShowDisplacement(), true, ui::Tr("ハイトで表面の形状と輪郭を変える。プレビュー専用", "Displaces the surface shape and silhouette by height. Preview only"));
        if (!layerMaterial)
            ui::PropertyFloat(ui::Tr("変位量", "Displacement"), &m_materialSphere.DisplacementMeters(), 0.0f, 1.0f, 0.1f,
                ui::Tr("ハイト0〜1の高低差。0.5を基準に表面を変位させる。プレビュー専用", "Height difference for the 0-1 height range, displacing around 0.5. Preview only"), "%.3f m");
        else ui::HintText("%s", ui::Tr("凹凸の高さは右側の「変位量」で調整", "Adjust the relief height with Displacement on the right"));
        ImGui::BeginDisabled(!m_materialSphere.ShowDisplacement());
        ui::PropertyBool(ui::Tr("影を落とす", "Cast Shadows"), &m_materialSphere.CastShadow(), true,
                         ui::Tr("凹凸が自分に落とす影を出す。太陽光にだけ効く。プレビュー専用", "Shows the shadows the relief casts on itself. Sunlight only. Preview only"));
        ImGui::EndDisabled();
        // 轍・道路端のマスクを見るための仮の道路。平面の中央に置く（球では位置が合わない）。
        if (layerMaterial && compositor::UsesRoadMasks(asset.layerGpu)) {
            ui::PropertyFloat(ui::Tr("道路の幅", "Road Width"), &m_materialSphere.RoadWidthMeters(), 0.5f, 60.0f, 7.0f,
                              ui::Tr("轍・道路端のマスクを見るための仮の道路の幅（m）。平面の中央に置く。"
                                     "一辺をこれより大きくすると道路の外も見える。プレビュー専用",
                                     "Width of the stand-in road for previewing wheel track / road edge masks (m), placed at the plane center. "
                                     "Make Side Length larger to see beyond the road. Preview only"),
                              "%.1f m");
            ui::PropertyInt(ui::Tr("車線数（進行方向）", "Lanes (Forward)"), &m_materialSphere.RoadLanesForward(), 1, 8, 1,
                            ui::Tr("仮の道路の進行方向の車線数。プレビュー専用", "Forward lanes of the stand-in road. Preview only"));
            ui::PropertyInt(ui::Tr("車線数（対向）", "Lanes (Opposing)"), &m_materialSphere.RoadLanesBackward(), 0, 8, 1,
                            ui::Tr("仮の道路の対向の車線数。プレビュー専用", "Opposing lanes of the stand-in road. Preview only"));
            static const ui::LocalizedText kTrafficSides[] = {{"左側通行", "Left-Hand Traffic"},
                                                             {"右側通行", "Right-Hand Traffic"}};
            int side = m_materialSphere.RoadLeftHandTraffic() ? 0 : 1;
            if (ui::PropertyCombo(ui::Tr("走行側", "Driving Side"), &side, ui::Tr(kTrafficSides).data(), 2, 0,
                                  ui::Tr("仮の道路の走行側。プレビュー専用", "Driving side of the stand-in road. Preview only")))
                m_materialSphere.RoadLeftHandTraffic() = side == 0;
        }
        ui::EndPropertyTable();
    }
    if (layerMaterial && compositor::UsesRoadMasks(asset.layerGpu) && m_materialSphere.Shape() == 1 &&
        m_materialSphere.PreviewLengthMeters(true) > m_materialSphere.LengthMeters()) {
        ui::HintText(ui::Tr("一辺が道路より狭いので、道路の両端が入るよう %.1f m で映している", "Side Length is narrower than the road, so %.1f m is shown to fit both road edges"),
                     m_materialSphere.PreviewLengthMeters(true));
    }
    if (ui::Button(ui::Tr("視点を戻す", "Reset View"), ui::kWideButtonWidth)) {
        m_materialSphere.ResetView();
    }
    ImGui::SameLine();
    if (ui::Button(ui::Tr("光源を戻す", "Reset Light"), ui::kWideButtonWidth)) m_materialSphere.ResetLight();
    ImGui::SameLine();
    ui::HelpMarker(ui::Tr("ドラッグ: 回転 / ホイール: ズーム / L＋ドラッグ: 光源", "Drag: rotate / Wheel: zoom / L+Drag: light"));

    ImGui::EndChild();
    ImGui::SameLine();

    // --- マテリアルの設定（この区画だけスクロールする）---------------------------
    ImGui::BeginChild("materialPropertyPane", ImVec2(0, 0));

    if (m_materialEditDraft.id != asset.id) CommitMaterialEdit();
    if (!m_materialEditPending) CopyMaterialValues(asset, m_materialEditDraft);
    if (DrawMaterialProperties(m_materialEditDraft)) {
        m_materialEditPending = true;
        m_materialEditAppearanceChanged = true;
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
