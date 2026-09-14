// 天球プレビューの窓。**シーンが持つ天球は 1 つ**で、その設定はここで行う。
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
// **映すのはシーンの天球**（＝ビューポートの環境）。窓の側に別の選択を持たせない。
void Application::DrawSkyPreviewWindow() {
    m_skyPreviewVisible = false;
    if (!m_showSkyPreview) {
        return;
    }

    // 縦長。絵の下に設定が続くので、幅は 1 列ぶんあれば足りる。
    // **窓そのものはスクロールさせない。** スクロールするのは下の区画だけ。
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(420.0f), ui::Scaled(620.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("天球プレビュー", &m_showSkyPreview,
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
    m_skyPreviewVisible = true;

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
        if (m_skySphere.HasOutput()) {
            ImGui::GetWindowDrawList()->AddImage(
                static_cast<ImTextureID>(m_skySphere.OutputHandle().ptr), min, max);
        }
        ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border),
                                            ImGui::GetStyle().FrameRounding, 0, ui::Scaled(1.0f));
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("skyPropertyPane", ImVec2(0.0f, 0.0f));
    ui::HintText("ドラッグで回す / ホイールで寄る。露出はビューポートと同じ");
    if (ui::Button("視点を戻す", ui::kWideButtonWidth)) {
        m_skySphere.ResetView();
    }
    ui::HintText("この天球がそのままビューポートの環境になる");

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
        // シーンの天球は 1 つ。どのファイルかを見せ、差し替えの入口をここにも置く。
        DrawAssetPathRow("ファイル", active->assetPath, m_pendingAssetReveal);
        ui::PropertyLabelEmpty("skyPick");
        if (ui::Button("差し替える…", ui::kWideButtonWidth)) {
            OpenSkyPicker();
        }
        ui::PropertyEnd();

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
            DrawAssetPathRow("ファイル", sky.hdriPath, m_pendingAssetReveal);

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
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
