// 作業用IBLのプレビューと設定。シーンの大気散乱スカイとは独立して保持する。
//
// 別の天球にするには、アセットの .tgsky をダブルクリックするか「差し替える…」で選ぶ。
// 一覧のパネルは持たない（環境は同時に 1 つしか使えず、一覧に載せたぶんだけ
// HDRI を読み込んでしまうため）。

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

// 天球プレビューの窓。大きい絵と、その天球の設定。
//
// 作業用IBL表示中だけ、その環境を球へ描く。
void Application::DrawSkyPreviewWindow() {
    m_skyPreviewVisible = false;
    if (!m_showSkyPreview || HiddenWithAssetBand("###WorkIbl")) {
        return;
    }

    // 縦長。絵の下に設定が続くので、幅は 1 列ぶんあれば足りる。
    // **窓そのものはスクロールさせない。** スクロールするのは下の区画だけ。
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(420.0f), ui::Scaled(620.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ui::Tr("作業用IBL###WorkIbl", "Work IBL###WorkIbl"), &m_showSkyPreview,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    m_skyLibrary.EnsureDefault();
    renderer::SkyAsset* active = m_skyLibrary.ActiveMutable();
    if (active == nullptr) {
        ImGui::End();
        return;
    }
    // 球は**適用中の環境キューブ**から毎フレーム描く（フレームの中で走る）。
    m_skyPreviewVisible = !m_renderer.AtmosphericMode();
    if (m_renderer.AtmosphericMode() && ui::Button(ui::Tr("作業用IBLで表示", "Use Work IBL"), ui::kWideButtonWidth)) {
        m_renderer.AtmosphericMode() = false; m_pendingWorkEnvironmentSave = true;
        m_skyPreviewVisible = true;
    }

    // --- 上下 2 区画。上は幅に合わせた正方形の絵 -------------------------------
    const float paneSize = PreviewPaneSize();
    ImGui::BeginChild("skyPreviewPane", ImVec2(0.0f, paneSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float imageSize =
            std::max(std::min(ImGui::GetContentRegionAvail().x, paneSize), ui::Scaled(32.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - imageSize) * 0.5f));
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + imageSize, min.y + imageSize);

        // 画像より先に ID を持つアイテムを置く（マテリアルの球と同じ作法）。
        ImGui::InvisibleButton("##skySphere", ImVec2(imageSize, imageSize),
                               ImGuiButtonFlags_MouseButtonLeft);
        if (ImGui::IsItemActive()) {
            // 1px = 0.35 度。マテリアルの球と同じ効き方。
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            m_skySphere.Orbit(delta.x * 0.35f, delta.y * 0.35f);
        }
        // この区画はスクロールしないので、ホイールはズームへ回せる。
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            m_skySphere.Zoom(ImGui::GetIO().MouseWheel);
        }

        // まだ絵が無いときは枠だけ描く。ImTextureID の 0 を AddImage へ渡すと
        // デバッグビルドの ImGui がアサートで落ちる。
        if (m_skyPreviewVisible && m_skySphere.HasOutput()) {
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(m_skySphere.OutputHandle().ptr), min, max);
        }
        ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border),
                                            ImGui::GetStyle().FrameRounding, 0, ui::Scaled(1.0f));
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("skyPropertyPane", ImVec2(0.0f, 0.0f));
    ui::HintText("%s", ui::Tr("ドラッグで回す / ホイールで寄る。露出はビューポートと同じ",
                              "Drag to rotate / wheel to zoom. Exposure matches the viewport"));
    if (ui::Button(ui::Tr("視点を戻す", "Reset View"), ui::kWideButtonWidth)) {
        m_skySphere.ResetView();
    }
    ui::HintText("%s", ui::Tr("地形・マテリアル確認用のIBLです。シーンの大気散乱スカイとは別に保存します",
                              "IBL for checking terrain and materials. Saved separately from the scene's Atmospheric Sky"));

    renderer::SkyDefinition& sky = active->sky;
    const renderer::SkyDefinition kDefaultSkyDefinition;
    // 見た目に関わる変更。サムネイルを作り直す（環境本体はレンダラが判断する）。
    bool changed = false;

    ui::SectionHeader(ui::Tr("基本", "General"));
    if (ui::BeginPropertyTable("skyBasicRows")) {
        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", active->name.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), nameBuffer, sizeof(nameBuffer),
                                  ui::Tr("ファイル名（拡張子なし）と同じ。変えるとファイルも改名する",
                                         "Same as the file name (without extension). Changing it renames the file too"))) {
            if (active->assetPath.empty()) {
                active->name = nameBuffer; m_pendingWorkEnvironmentSave = true; m_pendingWorkSkySave = true;
            } else {
                RequestAssetRename(active->assetPath, nameBuffer);
            }
        }
        // 選択中の作業用IBLのファイルと、差し替えの入口を表示する。
        DrawAssetPathRow(ui::Tr("ファイル", "File"), active->assetPath, m_pendingAssetReveal);
        ui::PropertyLabelEmpty("skyPick");
        if (ui::Button(ui::Tr("差し替える…", "Replace…"), ui::kWideButtonWidth)) {
            OpenSkyPicker();
        }
        ui::PropertyEnd();

        static const ui::LocalizedText kSourceLabels[] = {{"手続き的な空", "Procedural Sky"}, {"HDRI", "HDRI"}};
        int source = static_cast<int>(sky.source);
        if (ui::PropertyCombo(ui::Tr("ソース", "Source"), &source, ui::Tr(kSourceLabels).data(), IM_ARRAYSIZE(kSourceLabels),
                              static_cast<int>(kDefaultSkyDefinition.source),
                              ui::Tr("空を式で作るか、HDRI の画像を使うか",
                                     "Whether to build the sky from a formula or use an HDRI image"))) {
            sky.source = static_cast<renderer::SkySource>(source);
            changed = true;
        }

        changed |= ui::PropertyFloat(ui::Tr("環境光の強さ", "IBL Intensity"), &sky.iblIntensity, 0.0f, 4.0f,
                                     kDefaultSkyDefinition.iblIntensity,
                                     ui::Tr("物理量ではなく、見た目を整えるための倍率",
                                            "A multiplier for tuning the look, not a physical quantity"), "%.2f");
        ui::EndPropertyTable();
    }

    if (sky.source == renderer::SkySource::Hdri) {
        ui::SectionHeader("HDRI");
        if (ui::BeginPropertyTable("skyHdriRows")) {
            DrawAssetPathRow(ui::Tr("ファイル", "File"), sky.hdriPath, m_pendingAssetReveal);

            ui::PropertyLabelEmpty("hdrPick");
            if (ui::Button(ui::Tr("HDRI を選ぶ…", "Choose HDRI…"), ui::kWideButtonWidth)) {
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
                ui::Tr("空の輝度", "Sky Luminance"), &sky.skyLuminance, 100.0f, 100000.0f,
                kDefaultSkyDefinition.skyLuminance,
                ui::Tr("この HDRI の空を何 cd/m2 とみなすか。"
                       "HDRI は絶対輝度で較正されていないため、基準をここで与える。"
                       "晴天でおよそ 4000〜15000、曇天で 1000〜3000",
                       "How many cd/m2 the sky of this HDRI is taken to be. "
                       "HDRIs are not calibrated to absolute luminance, so the reference is given here. "
                       "Roughly 4000-15000 on a clear day, 1000-3000 when overcast"),
                "%.0f cd/m2", ImGuiSliderFlags_Logarithmic);
            ui::EndPropertyTable();
        }
        ui::HintText("%s", ui::Tr("エクスプローラから .hdr を落としても、選択中の天球に入る",
                                  "Dropping a .hdr from Explorer also loads it into the selected sky"));
    } else {
        ui::SectionHeader(ui::Tr("手続き的な空", "Procedural Sky"));
        if (ui::BeginPropertyTable("skyProceduralRows")) {
            renderer::SkySettings& procedural = sky.procedural;
            changed |= ui::PropertyColorLinear(ui::Tr("天頂色", "Zenith Color"), &procedural.zenithColor.x,
                                               &kDefaultSky.zenithColor.x);
            changed |= ui::PropertyColorLinear(ui::Tr("地平色", "Horizon Color"), &procedural.horizonColor.x,
                                               &kDefaultSky.horizonColor.x);
            changed |= ui::PropertyColorLinear(ui::Tr("地面色", "Ground Color"), &procedural.groundColor.x,
                                               &kDefaultSky.groundColor.x);
            changed |= ui::PropertyFloat(ui::Tr("輝度", "Luminance"), &procedural.intensity, 0.0f, 100000.0f,
                                         kDefaultSky.intensity,
                                         ui::Tr("cd/m2。晴天の空はおよそ 4000〜15000",
                                                "cd/m2. A clear sky is roughly 4000-15000"), "%.0f");
            ui::EndPropertyTable();
        }
    }

    if (changed) {
        m_pendingWorkEnvironmentSave = true; m_pendingWorkSkySave = true;
        m_skyLibrary.MarkThumbnailDirty(active->id);
    }
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
