// 天球パネル。ライブラリの一覧と、選択中の天球の設定。
//
// **一覧で選んだものが、そのままビューポートの環境になる。**
// 環境は同時に 1 つしか使えないので、「選ぶ」と「適用する」を分けても
// 操作が 1 つ増えるだけで、選んだのに反映されない状態を作るほうが混乱する。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "core/PathUtf8.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace tg {

void Application::DrawSkyLibraryPanel() {
    if (!ImGui::Begin("天球")) {
        ImGui::End();
        return;
    }

    // 一覧は空にしない。空だとビューポートの環境が決まらなくなる。
    m_skyLibrary.EnsureDefault();
    const std::vector<renderer::SkyAsset>& assets = m_skyLibrary.Entries();
    const auto assetCount = static_cast<int>(assets.size());

    if (ui::Button("追加")) {
        const renderer::SkyAssetId added =
            m_skyLibrary.Add("天球 " + std::to_string(assets.size() + 1));
        m_skyLibrary.SetActive(added);
        m_scrollToSelectedSky = true;
    }
    ImGui::SameLine();
    if (ui::Button("複製")) {
        if (const renderer::SkyAsset* source = m_skyLibrary.Active(); source != nullptr) {
            const renderer::SkyAssetId added = m_skyLibrary.Duplicate(*source);
            m_skyLibrary.SetActive(added);
            m_scrollToSelectedSky = true;
        }
    }
    ImGui::SameLine();
    // 最後の 1 つは消させない。消しても既定が作り直されるだけで、
    // ボタンが効いていないように見える。
    ImGui::BeginDisabled(assetCount <= 1);
    if (ui::Button("削除")) {
        // その場で消すと、この後の一覧描画が erase 済みの要素を読んでしまう。
        // 要求だけ積み、フレームの外で処理する。
        m_pendingSkyRemove = m_skyLibrary.ActiveId();
    }
    ImGui::EndDisabled();

    // サムネイルの一覧。マテリアルと同じ作法で、パネルの幅に入るだけ横に並べる。
    const float thumbnailSize = ui::Scaled(84.0f);
    if (ImGui::BeginChild("skyGrid", ImVec2(0.0f, ui::Scaled(200.0f)), ImGuiChildFlags_Borders)) {
        const float step = thumbnailSize + ImGui::GetStyle().ItemSpacing.x;
        const auto columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / step));

        for (int i = 0; i < assetCount; ++i) {
            const renderer::SkyAsset& asset = assets[static_cast<size_t>(i)];
            ImGui::PushID(static_cast<int>(asset.id));

            ImGui::BeginGroup();
            const bool selected = (m_skyLibrary.ActiveId() == asset.id);
            const ImTextureID textureId =
                asset.thumbnail.IsValid()
                    ? static_cast<ImTextureID>(asset.thumbnail.srv.gpu.ptr)
                    : static_cast<ImTextureID>(0);
            const ui::Thumbnail thumbnail =
                ui::ThumbnailButton("##thumbnail", textureId, thumbnailSize, selected);
            if (thumbnail.clicked) {
                m_skyLibrary.SetActive(asset.id);
            }
            if (selected && m_scrollToSelectedSky) {
                m_scrollToSelectedSky = false;
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

    renderer::SkyAsset* active = m_skyLibrary.ActiveMutable();
    if (active == nullptr) {
        ImGui::End();
        return;
    }
    renderer::SkyDefinition& sky = active->sky;
    const renderer::SkyDefinition kDefaultSkyDefinition;
    // 見た目に関わる変更。サムネイルを作り直す（環境本体はレンダラが判断する）。
    bool changed = false;

    ui::SectionHeader("基本");
    if (ui::BeginPropertyTable("skyBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", active->name.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer))) {
            active->name = nameBuffer;
        }

        static const char* const kSourceLabels[] = {"手続き的な空", "HDRI"};
        int source = static_cast<int>(sky.source);
        if (ui::PropertyCombo("ソース", &source, kSourceLabels, IM_ARRAYSIZE(kSourceLabels),
                              static_cast<int>(kDefaultSkyDefinition.source),
                              "空を式で作るか、HDRI の画像を使うか")) {
            sky.source = static_cast<renderer::SkySource>(source);
            changed = true;
        }

        changed |= ui::PropertyFloat("環境光の強さ", &sky.iblIntensity, 0.0f, 4.0f,
                                     kDefaultSkyDefinition.iblIntensity,
                                     "物理量ではなく、見た目を整えるための倍率", "%.2f");
        ui::EndPropertyTable();
    }

    if (sky.source == renderer::SkySource::Hdri) {
        ui::SectionHeader("HDRI");
        if (ui::BeginPropertyTable("skyHdriRows")) {
            ui::PropertyValue("ファイル", "%s",
                              sky.hdriPath.empty()
                                  ? "（未指定）"
                                  : ToUtf8Display(sky.hdriPath.filename()).c_str());
            if (!sky.hdriPath.empty() && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", ToUtf8Display(sky.hdriPath).c_str());
            }

            ui::PropertyLabelEmpty("hdrPick");
            if (ui::Button("HDRI を選ぶ…", ui::kWideButtonWidth)) {
                const std::filesystem::path path =
                    ShowOpenFileDialog(L"HDRI を選ぶ", HdriFileFilters());
                if (!path.empty()) {
                    sky.hdriPath = path;
                    changed = true;
                }
            }
            ui::PropertyEnd();

            // **較正値は天球ごとに持つ。** HDRI は絶対輝度で較正されていないので、
            // どのファイルでも同じ意味になる「空を何 cd/m2 とみなすか」で与える。
            changed |= ui::PropertyFloat(
                "空の輝度", &sky.skyLuminance, 100.0f, 100000.0f,
                kDefaultSkyDefinition.skyLuminance,
                "この HDRI の空を何 cd/m2 とみなすか。"
                "HDRI は絶対輝度で較正されていないため、基準をここで与える。"
                "晴天でおよそ 4000〜15000、曇天で 1000〜3000",
                "%.0f cd/m2", ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::HintText("エクスプローラから .hdr を落としても、選択中の天球に入る");
    } else {
        ui::SectionHeader("手続き的な空");
        if (ui::BeginPropertyTable("skyProceduralRows")) {
            renderer::SkySettings& procedural = sky.procedural;
            changed |= ui::PropertyColorLinear("天頂色", &procedural.zenithColor.x,
                                               &kDefaultSky.zenithColor.x);
            changed |= ui::PropertyColorLinear("地平色", &procedural.horizonColor.x,
                                               &kDefaultSky.horizonColor.x);
            changed |= ui::PropertyColorLinear("地面色", &procedural.groundColor.x,
                                               &kDefaultSky.groundColor.x);
            changed |= ui::PropertyFloat("輝度", &procedural.intensity, 0.0f, 100000.0f,
                                         kDefaultSky.intensity,
                                         "cd/m2。晴天の空はおよそ 4000〜15000", "%.0f");
            ui::EndPropertyTable();
        }
    }

    if (changed) {
        m_skyLibrary.MarkThumbnailDirty(active->id);
    }

    ImGui::End();
}

}  // namespace tg
