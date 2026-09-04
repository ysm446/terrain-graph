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
            // 右クリックのメニュー。**押したサムネイルが対象**なので、
            // 開くときに選択もそちらへ移す。
            if (ImGui::BeginPopupContextItem("##materialMenu")) {
                m_selectedMaterial = i;
                DrawMaterialContextMenu(asset.id);
                ImGui::EndPopup();
            }
            if (thumbnail.hovered) {
                ImGui::SetTooltip("%s\nダブルクリックで設定 / 右クリックでメニュー",
                                  asset.name.c_str());
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
    if (asset != nullptr && ImGui::MenuItem("書き出し…")) {
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
    bool changed = false;

    ui::SectionHeader("基本");
    if (ui::BeginPropertyTable("materialBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", asset.name.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer))) {
            asset.name = nameBuffer;
            // 名前もアンドゥの対象。落とすと、次のアンドゥで改名まで巻き戻る。
            changed = true;
        }

        static const compositor::MaterialAsset kDefaultAsset;
        // マップ節にも「ベースカラー」の行（テクスチャ)があるため、
        // ここは掛ける色だと分かる名前にする。
        changed |= ui::PropertyColorLinear(
            "ティント", &asset.baseColorTint.x, &kDefaultAsset.baseColorTint.x,
            "ベースカラーのマップに掛ける色。白ならマップそのまま。マップが無ければこの色になる");
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
        changed |= DrawTextureSlotRow("ベースカラー", asset.baseColor, m_textureLibrary);
        changed |= DrawTextureSlotRow("法線", asset.normal, m_textureLibrary);
        if (asset.normal != compositor::kNoTexture) {
            changed |= ui::PropertyBool(
                "緑を反転", &asset.flipNormalGreen, true,
                "法線マップの規約。OpenGL（Megascans などの既定）は入、"
                "DirectX 規約の素材は切る。切り替えて陰影が自然なほうが正しい");
        }
        changed |= DrawMapSlotRow("ラフネス", asset.roughness, m_textureLibrary);
        changed |= DrawMapSlotRow("メタルネス", asset.metallic, m_textureLibrary);
        changed |= DrawMapSlotRow("AO", asset.ambientOcclusion, m_textureLibrary);
        changed |= DrawMapSlotRow("ハイト", asset.height, m_textureLibrary);

        // 1 枚に AO / ラフネス / ハイトを詰めたテクスチャをまとめて割り当てる。
        ui::PropertyLabel("ORD", "1 枚に AO / ラフネス / ハイトを詰めたテクスチャ");
        const float ordButtonWidth = ui::Scaled(ui::kButtonWidth);
        const float ordSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float ordComboWidth = std::max(
            ui::Scaled(60.0f),
            std::min(ui::Scaled(ui::kComboMaxWidth), ImGui::GetContentRegionAvail().x) -
                ordButtonWidth - ordSpacing);
        DrawTextureCombo("##ord", m_ordTexture, m_textureLibrary, ordComboWidth);
        ImGui::SameLine(0.0f, ordSpacing);
        ImGui::BeginDisabled(m_ordTexture == compositor::kNoTexture);
        if (ui::Button("割り当て")) {
            m_materialLibrary.AssignOrdTexture(asset.id, m_ordTexture);
            changed = true;
        }
        ImGui::EndDisabled();
        ui::PropertyEnd();

        ui::EndPropertyTable();
    }
    ui::HintText("ORD は AO=R / ラフネス=G / ハイト=B に割り当てる（Megascans の並び）");
    ui::HintText("ハイトはレイヤーの「ハイトのソース」をテクスチャにすると効く");

    return changed;
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

    // 縦長。球の下にプロパティが続くので、幅は 1 列ぶんあれば足りる。
    // **窓そのものはスクロールさせない。** 中身は上下 2 つの区画で、
    // スクロールするのは下（プロパティ）だけ。
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(420.0f), ui::Scaled(720.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("マテリアルプレビュー", &m_showMaterialSphere,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    const std::vector<compositor::MaterialAsset>& assets = m_materialLibrary.Entries();
    if (assets.empty()) {
        ui::HintText("マテリアルがない。「マテリアル」パネルの「追加」で作る");
        ImGui::End();
        return;
    }

    m_materialSphereVisible = true;

    const int index =
        std::clamp(m_selectedMaterial, 0, static_cast<int>(assets.size()) - 1);
    compositor::MaterialAsset& asset =
        *m_materialLibrary.FindMutable(assets[static_cast<size_t>(index)].id);

    // --- 上下 2 区画 ----------------------------------------------------------
    // 上が球、下がプロパティ。**スクロールするのは下だけ。**
    // 上は**幅に合わせた正方形**なので、窓を広げれば球も大きくなり、余白が残らない。
    const float paneSize = PreviewPaneSize();

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
            m_materialSphere.Orbit(delta.x * 0.35f, delta.y * 0.35f);
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
    }
    ImGui::EndChild();

    ImGui::Separator();

    // --- プロパティ（この区画だけスクロールする）------------------------------
    ImGui::BeginChild("materialPropertyPane", ImVec2(0.0f, 0.0f));
    ui::HintText("ドラッグで回す / ホイールで寄る。照らし方はビューポートと同じ");

    // 表示だけの設定。マテリアルの設定とは区切り線で分ける。
    if (ui::BeginPropertyTable("materialSphereViewRows")) {
        ui::PropertyFloat("タイル", &m_materialSphere.UvScale(), 0.25f, 8.0f, 2.0f,
                          "球 1 周に並べるマップの数。マテリアルには保存しない", "%.2f");
        ui::EndPropertyTable();
    }
    if (ui::Button("視点を戻す", ui::kWideButtonWidth)) {
        m_materialSphere.ResetView();
    }

    ImGui::Separator();

    if (DrawMaterialProperties(asset)) {
        m_materialLibrary.MarkThumbnailDirty(asset.id);
        MarkDocumentChanged();
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
