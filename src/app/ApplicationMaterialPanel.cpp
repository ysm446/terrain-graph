// マテリアルパネル。ライブラリの一覧と、選択中マテリアルのマップ割り当て。

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

    if (ui::Button("追加")) {
        m_materialLibrary.Add("マテリアル " + std::to_string(assets.size() + 1));
        m_selectedMaterial = static_cast<int>(assets.size()) - 1;
        m_scrollToSelectedMaterial = true;
        MarkDocumentChanged();
    }
    ImGui::SameLine();
    if (ui::Button("複製") && assetCount > 0) {
        m_materialLibrary.Duplicate(assets[static_cast<size_t>(m_selectedMaterial)]);
        m_selectedMaterial = static_cast<int>(assets.size()) - 1;
        m_scrollToSelectedMaterial = true;
        MarkDocumentChanged();
    }
    ImGui::SameLine();
    if (ui::Button("削除") && assetCount > 0) {
        // その場で消すと、この後の一覧描画が erase 済みの要素（assetCount は
        // 古いまま）を読んでしまう。要求だけ積み、フレームの外で処理する。
        m_pendingMaterialRemove = assets[static_cast<size_t>(m_selectedMaterial)].id;
    }

    // マテリアル単体のファイル (.tgmat)。プロジェクト間で持ち回るために使う。
    // プロジェクトにはマテリアルの構造ごと埋め込まれるので、保存には要らない。
    ImGui::SameLine();
    if (ui::Button("読み込み…", ui::kWideButtonWidth)) {
        const std::filesystem::path path =
            ShowOpenFileDialog(L"マテリアルを読み込む", MaterialFileFilters());
        if (!path.empty()) {
            m_pendingMaterialImport = path;
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(assetCount == 0);
    if (ui::Button("書き出し…", ui::kWideButtonWidth)) {
        const compositor::MaterialAsset& target =
            assets[static_cast<size_t>(m_selectedMaterial)];
        const std::filesystem::path path = ShowSaveFileDialog(
            L"マテリアルを書き出す", MaterialFileFilters(), L"tgmat", FromUtf8(target.name));
        if (!path.empty()) {
            m_pendingMaterialExport = path;
            m_pendingExportMaterial = target.id;
        }
    }
    ImGui::EndDisabled();

    // サムネイルの一覧。パネルの幅に入るだけ横に並べる。
    const float thumbnailSize = ui::Scaled(84.0f);
    if (ImGui::BeginChild("materialGrid", ImVec2(0.0f, ui::Scaled(200.0f)),
                          ImGuiChildFlags_Borders)) {
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
            // 追加・複製した直後のものは枠内へ送る（テクスチャ一覧と同じ）。
            if (m_selectedMaterial == i && m_scrollToSelectedMaterial) {
                m_scrollToSelectedMaterial = false;
                ImGui::SetScrollHereY(1.0f);
            }
            if (thumbnail.hovered) {
                ImGui::SetTooltip("%s", asset.name.c_str());
            }
            ImGui::EndGroup();

            ImGui::PopID();
            if (((i + 1) % columns) != 0 && (i + 1) < assetCount) {
                ImGui::SameLine();
            }
        }
    }
    ImGui::EndChild();

    if (assetCount == 0) {
        ui::HintText("「追加」でマテリアルを作り、マップを割り当てる");
        ImGui::End();
        return;
    }

    compositor::MaterialAsset& asset =
        *m_materialLibrary.FindMutable(assets[static_cast<size_t>(m_selectedMaterial)].id);
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

    if (changed) {
        // サムネイルと合成の両方を作り直す。
        m_materialLibrary.MarkThumbnailDirty(asset.id);
        MarkDocumentChanged();
    }

    ImGui::End();
}

}  // namespace tg
