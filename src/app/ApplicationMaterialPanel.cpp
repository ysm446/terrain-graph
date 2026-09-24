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
    target.alphaCutoff = source.alphaCutoff;
    target.twoSided = source.twoSided;
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

void Application::DrawMaterialLibraryPanel() {
    if (!ImGui::Begin("マテリアル")) {
        ImGui::End();
        return;
    }

    const std::vector<compositor::MaterialAsset>& assets = m_materialLibrary.Entries();
    const auto assetCount = static_cast<int>(assets.size());
    m_selectedMaterial = std::clamp(m_selectedMaterial, 0, (assetCount > 0) ? assetCount - 1 : 0);

    // サムネイルの一覧。パネルの幅に入るだけ横に並べる。
    // **設定はここに出さない。** 一覧は「どれを使うか選ぶ」場所で、値の調整は
    // 球を見ながらやるほうが早い（ダブルクリックでプレビューの窓が開く）。
    // 残りの高さいっぱいに使う。下に続くものが無いので、高さを決め打ちにしない。
    const float thumbnailSize = ui::Scaled(84.0f);
    if (ImGui::BeginChild("materialGrid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        if (assetCount == 0) {
            ui::HintText("右クリックのメニューから追加する。"
                         "サムネイルのダブルクリックで設定が開く");
        }
        const float step = thumbnailSize + ImGui::GetStyle().ItemSpacing.x;
        const auto columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / step));

        for (int i = 0; i < assetCount; ++i) {
            const compositor::MaterialAsset& asset = assets[static_cast<size_t>(i)];
            ImGui::PushID(static_cast<int>(asset.id));

            ImGui::BeginGroup();
            const bool selected = (m_selectedMaterial == i);
            // design-guide §5 に合わせ、テクスチャ一覧と同じ ThumbnailButton を使う。
            // ImGui::Image + IsItemClicked はドラッグ元にした瞬間に効かなくなる罠がある。
            const ImTextureID textureId =
                asset.thumbnail.IsValid()
                    ? static_cast<ImTextureID>(asset.thumbnail.srv.gpu.ptr)
                    : static_cast<ImTextureID>(0);
            const ui::Thumbnail thumbnail =
                ui::ThumbnailButton("##thumbnail", textureId, thumbnailSize, selected);
            // 参照しているテクスチャにリンク切れがあれば、目印を重ねる。
            // サムネイルは残りのマップで作れるので絵は出るが、それだけだと欠けに気づけない。
            const bool hasMissing = MaterialHasMissingTexture(asset);
            if (hasMissing) {
                ui::MissingBadge(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
            if (thumbnail.clicked) {
                m_selectedMaterial = i;
            }
            // ダブルクリックで球のプレビューを開く。**選択も一緒に動く**ので、
            // 開いた窓には必ずいま押したマテリアルが出る。
            if (thumbnail.doubleClicked) {
                m_selectedMaterial = i;
                m_showMaterialSphere = true;
                ImGui::SetWindowFocus("マテリアルプレビュー");
            }
            // 追加・複製した直後のものは枠内へ送る（テクスチャ一覧と同じ）。
            if (m_selectedMaterial == i && m_scrollToSelectedMaterial) {
                m_scrollToSelectedMaterial = false;
                ImGui::SetScrollHereY(1.0f);
            }
            // Surface のマテリアル欄（プロパティの行 / ノードのサムネイル）へ
            // 落とすと、そのノードに割り当たる。テクスチャ一覧と同じ作り。
            const bool dragging =
                ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers);
            if (dragging) {
                ImGui::SetDragDropPayload(kMaterialDragDropType, &asset.id,
                                          sizeof(compositor::MaterialAssetId));
                ImGui::TextUnformatted(asset.name.c_str());
                ImGui::EndDragDropSource();
            }
            // 右クリックのメニュー。**押したサムネイルが対象**なので、
            // 開くときに選択もそちらへ移す。
            if (ImGui::BeginPopupContextItem("##materialMenu")) {
                m_selectedMaterial = i;
                DrawMaterialContextMenu(asset.id);
                ImGui::EndPopup();
            }
            // ドラッグ中は ImGui 自身がプレビューを出すので、ツールチップは重ねない。
            if (thumbnail.hovered && !dragging) {
                ImGui::SetTooltip("%s%s\nダブルクリックで設定 / 右クリックでメニュー\n"
                                  "Surface のマテリアル欄へドラッグで割り当て",
                                  asset.name.c_str(),
                                  hasMissing ? "\nリンク切れのテクスチャを参照している" : "");
            }
            // 名前を添える（テクスチャ一覧と同じ）。球の絵だけでは似た素材を
            // 見分けにくく、ホバーしないと分からないと一覧として使いにくい。
            // リンク切れを含むものは名前も警告色にして、離れて見ても分かるようにする。
            if (hasMissing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
            }
            ui::GridCaption(asset.name.c_str(), thumbnailSize);
            if (hasMissing) {
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();

            ImGui::PopID();
            if (((i + 1) % columns) != 0 && (i + 1) < assetCount) {
                ImGui::SameLine();
            }
        }

        // サムネイルの無い所での右クリック。対象が無いので「追加」と「読み込み」だけ。
        // **一覧が空のときもここから作れる**（ボタンの帯を持たないため）。
        if (ImGui::BeginPopupContextWindow("##materialGridMenu",
                                           ImGuiPopupFlags_MouseButtonRight |
                                               ImGuiPopupFlags_NoOpenOverItems)) {
            DrawMaterialContextMenu(compositor::kNoMaterialAsset);
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// 一覧の右クリックメニュー。**target が無効なら、対象の要る項目は出さない**
// （サムネイルの無い所を押したとき）。ボタンの帯は持たず、追加も削除もここから行う。
void Application::DrawMaterialContextMenu(compositor::MaterialAssetId target) {
    const std::vector<compositor::MaterialAsset>& assets = m_materialLibrary.Entries();
    const compositor::MaterialAsset* asset = m_materialLibrary.Find(target);

    if (asset != nullptr) {
        ImGui::TextDisabled("%s", asset->name.c_str());
        ImGui::Separator();
    }

    if (ImGui::MenuItem("追加")) {
        m_materialLibrary.Add("マテリアル " + std::to_string(assets.size() + 1));
        m_selectedMaterial = static_cast<int>(assets.size()) - 1;
        m_scrollToSelectedMaterial = true;
        MarkDocumentChanged();
    }
    if (asset != nullptr) {
        if (ImGui::MenuItem("複製")) {
            m_materialLibrary.Duplicate(*asset);
            m_selectedMaterial = static_cast<int>(assets.size()) - 1;
            m_scrollToSelectedMaterial = true;
            MarkDocumentChanged();
        }
        if (ImGui::MenuItem("削除")) {
            // その場で消すと、この後の一覧描画が erase 済みの要素を読んでしまう。
            // 要求だけ積み、フレームの外で処理する。
            m_pendingMaterialRemove = target;
        }
    }

    // マテリアル単体のファイル (.tgmat)。プロジェクト間で持ち回るために使う。
    // プロジェクトにはマテリアルの構造ごと埋め込まれるので、保存には要らない。
    ImGui::Separator();
    if (ImGui::MenuItem("読み込み…")) {
        const std::filesystem::path path =
            ShowOpenFileDialog(L"マテリアルを読み込む", MaterialFileFilters());
        if (!path.empty()) {
            m_pendingMaterialImport = path;
        }
    }
    if (asset != nullptr && !asset->layerMaterial && ImGui::MenuItem("書き出し…")) {
        const std::filesystem::path path = ShowSaveFileDialog(
            L"マテリアルを書き出す", MaterialFileFilters(), L"tgmat", FromUtf8(asset->name));
        if (!path.empty()) {
            m_pendingMaterialExport = path;
            m_pendingExportMaterial = target;
        }
    }
}

// マテリアル 1 つのプロパティ（基本 + マップ）。**置き場所はプレビューの窓だけ。**
// 一覧はサムネイルだけを出し、値の調整は球を見ながらやる。
// 窓の描画から切り出してあるのは、球の操作と行の並びを読み分けられるようにするため。
bool Application::DrawMaterialProperties(compositor::MaterialAsset& asset) {
    if (asset.layerMaterial) return DrawLayerMaterialProperties(asset);
    bool changed = false;

    ui::SectionHeader("基本");
    if (ui::BeginPropertyTable("materialBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", asset.name.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer),
                                  "ファイル名（拡張子なし）と同じ。変えるとファイルも改名する")) {
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
            "ティント", &asset.baseColorTint.x, &kDefaultAsset.baseColorTint.x,
            "ベースカラーのマップに掛ける色。白ならマップそのまま。マップが無ければこの色になる");
        // **ベースカラーだけの調整。** ティントでは彩度を上げられず色相も回せないので、
        // 素材を馴染ませる操作をここに置く。掛ける色（ティント）のあとに効く。
        changed |= ui::PropertyFloat(
            "色相", &asset.hueShiftDegrees, -180.0f, 180.0f, kDefaultAsset.hueShiftDegrees,
            "ベースカラーの色みを回す（度）。灰色は灰色のまま残る", "%.0f 度");
        changed |= ui::PropertyFloat("彩度", &asset.saturation, 0.0f, 2.0f,
                                     kDefaultAsset.saturation,
                                     "ベースカラーの鮮やかさ。0 で白黒、1 でそのまま", "%.2f");
        changed |= ui::PropertyFloat("明度", &asset.brightness, 0.0f, 2.0f,
                                     kDefaultAsset.brightness,
                                     "ベースカラーの明るさ（倍率）。1 でそのまま、0 で黒。結果は 0〜1 に収める", "%.2f");
        changed |= ui::PropertyFloat("ラフネス", &asset.roughnessValue, 0.0f, 1.0f,
                                     kDefaultAsset.roughnessValue, "マップが無いときの値",
                                     "%.2f");
        changed |= ui::PropertyFloat("メタルネス", &asset.metallicValue, 0.0f, 1.0f,
                                     kDefaultAsset.metallicValue, "マップが無いときの値",
                                     "%.2f");
        changed |= ui::PropertyFloat("AO", &asset.ambientOcclusionValue, 0.0f, 1.0f,
                                     kDefaultAsset.ambientOcclusionValue, "マップが無いときの値",
                                     "%.2f");
        ui::EndPropertyTable();
    }

    ui::SectionHeader("マップ");
    if (ui::BeginPropertyTable("materialMapRows")) {
        changed |= DrawTextureSlotRow("ベースカラー", asset.baseColor, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawTextureSlotRow("法線", asset.normal, m_textureLibrary, m_pendingAssetReveal);
        if (asset.normal != compositor::kNoTexture) {
            changed |= ui::PropertyBool(
                "緑を反転", &asset.flipNormalGreen, true,
                "法線マップの規約。OpenGL（Megascans などの既定）は入、"
                "DirectX 規約の素材は切る。切り替えて陰影が自然なほうが正しい");
        }
        changed |= DrawMapSlotRow("ラフネス", asset.roughness, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawMapSlotRow("メタルネス", asset.metallic, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawMapSlotRow("AO", asset.ambientOcclusion, m_textureLibrary, m_pendingAssetReveal);
        changed |= DrawMapSlotRow("ハイト", asset.height, m_textureLibrary, m_pendingAssetReveal);

        // 1 枚に AO / ラフネス / ハイトを詰めたテクスチャをまとめて割り当てる。
        ui::PropertyLabel("ORD", "1 枚に AO / ラフネス / ハイトを詰めたテクスチャ");
        const float ordButtonWidth = ui::Scaled(ui::kButtonWidth);
        const float ordSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float ordComboWidth = std::max(
            ui::Scaled(60.0f),
            std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x) -
                ordButtonWidth - ordSpacing);
        DrawTextureCombo("##ord", m_ordTexture, m_textureLibrary, ordComboWidth, m_pendingAssetReveal);
        ImGui::SameLine(0.0f, ordSpacing);
        ImGui::BeginDisabled(m_ordTexture == compositor::kNoTexture);
        if (ui::Button("割り当て")) {
            asset.ambientOcclusion = {m_ordTexture, compositor::TextureChannel::R};
            asset.roughness = {m_ordTexture, compositor::TextureChannel::G};
            asset.height = {m_ordTexture, compositor::TextureChannel::B};
            changed = true;
        }
        ImGui::EndDisabled();
        ui::PropertyEnd();

        ui::EndPropertyTable();
    }
    ui::HintText("ORD は AO=R / ラフネス=G / ハイト=B に割り当てる（Megascans の並び）");
    ui::HintText("ハイトはレイヤーの「ハイトのソース」をテクスチャにすると効く");

    // モデルに割り当てたときだけ効く設定。地形のレイヤー合成には使わない。
    ui::SectionHeader("モデル");
    if (ui::BeginPropertyTable("materialModelRows")) {
        static const compositor::MaterialAsset kDefaultAsset;
        changed |= ui::PropertyFloat(
            "アルファ抜き", &asset.alphaCutoff, 0.0f, 1.0f, kDefaultAsset.alphaCutoff,
            "ベースカラーのアルファがこの値未満の画素を描かない（影も抜ける）。0 で無効。"
            "葉のカードなどに使う。目安は 0.5",
            "%.2f");
        changed |= ui::PropertyBool(
            "両面", &asset.twoSided, kDefaultAsset.twoSided,
            "裏面も描く。裏から見たときは法線を反転して陰影を付ける。葉のカードなど厚みの無い面に使う");
        ui::EndPropertyTable();
    }
    ui::HintText("アルファ抜きと両面はモデルの描画だけに効く");

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
    std::snprintf(text, sizeof(text), "方位角 %.0f 度   仰角 %.0f 度",
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
    if (!m_showMaterialSphere) {
        return;
    }

    const auto& assets = m_materialLibrary.Entries();
    const int index = std::clamp(m_selectedMaterial, 0, std::max(0, static_cast<int>(assets.size()) - 1));
    const bool layerLayout = !assets.empty() && assets[index].layerMaterial.has_value();
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(layerLayout ? 1000.0f : 420.0f), ui::Scaled(720.0f)),
        layerLayout != m_materialPreviewLayerLayout ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    m_materialPreviewLayerLayout = layerLayout;
    if (!ImGui::Begin("マテリアルプレビュー", &m_showMaterialSphere,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    if (assets.empty()) {
        ui::HintText("マテリアルがない。「マテリアル」パネルの「追加」で作る");
        ImGui::End();
        return;
    }

    m_materialSphereVisible = true;

    compositor::MaterialAsset& asset =
        *m_materialLibrary.FindMutable(assets[static_cast<size_t>(index)].id);

    // レイヤーマテリアルは左にプレビュー、右に独立してスクロールする編集欄。
    if (layerLayout) {
        const float width = ImGui::GetContentRegionAvail().x * 0.48f;
        ImGui::BeginChild("layerMaterialPreview", ImVec2(width, 0), ImGuiChildFlags_None,
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    }
    const float paneSize = layerLayout
        ? std::max(ui::Scaled(32), std::min(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y - ui::Scaled(145)))
        : PreviewPaneSize();

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

    // --- プロパティ（この区画だけスクロールする）------------------------------
    if (!layerLayout) ImGui::BeginChild("materialPropertyPane", ImVec2(0.0f, 0.0f));
    ui::HintText("ドラッグ: 回転 / ホイール: ズーム / L＋ドラッグ: 光源");

    // 表示だけの設定。マテリアルの設定とは区切り線で分ける。
    if (ui::BeginPropertyTable("materialSphereViewRows")) {
        const char* shapes[]{"球", "平面"};
        int shape = m_materialSphere.Shape();
        if (ui::PropertyCombo("形状", &shape, shapes, 2, 0, "プレビュー形状。マテリアルには保存しません")) m_materialSphere.SetShape(shape);
        // 長さは実寸（m）。平面は一辺、球は直径で、どちらも同じ値を使う
        // （球の赤道付近の模様が、同じ長さの平面と同じ大きさで見える）。
        const bool plane = m_materialSphere.Shape() == 1;
        ui::PropertyFloat(plane ? "一辺" : "直径", &m_materialSphere.LengthMeters(), 0.1f, 100.0f, 2.0f,
                          plane ? "映す平面の一辺の長さ（m）。マテリアルには保存しない"
                                : "映す球の直径（m）。赤道の模様が同じ長さの平面と揃う。マテリアルには保存しない",
                          "%.2f m");
        ui::PropertyBool("変位を表示", &m_materialSphere.ShowDisplacement(), true, "ハイトで表面の形状と輪郭を変える。プレビュー専用");
        if (!layerLayout)
            ui::PropertyFloat("変位量", &m_materialSphere.DisplacementMeters(), 0.0f, 1.0f, 0.1f,
                "ハイト0〜1の高低差。0.5を基準に表面を変位させる。プレビュー専用", "%.3f m");
        else ui::HintText("凹凸の高さは右側の「変位量」で調整");
        ImGui::BeginDisabled(!m_materialSphere.ShowDisplacement());
        ui::PropertyBool("影を落とす", &m_materialSphere.CastShadow(), true,
                         "凹凸が自分に落とす影を出す。太陽光にだけ効く。プレビュー専用");
        ImGui::EndDisabled();
        ui::EndPropertyTable();
    }
    if (ui::Button("視点を戻す", ui::kWideButtonWidth)) {
        m_materialSphere.ResetView();
    }
    ImGui::SameLine();
    if (ui::Button("光源を戻す", ui::kWideButtonWidth)) m_materialSphere.ResetLight();

    ImGui::Separator();

    if (layerLayout) {
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("materialPropertyPane", ImVec2(0, 0));
    }

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
