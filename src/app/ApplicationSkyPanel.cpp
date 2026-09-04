// 天球パネル。**一覧（サムネイル）だけ**を置き、設定はプレビューの窓が持つ。
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

    // サムネイルの一覧。マテリアルと同じ作法で、パネルの幅に入るだけ横に並べる。
    // **設定も操作のボタンも置かない。** 設定はプレビューの窓、
    // 追加 / 複製 / 削除は右クリックのメニュー。
    const float thumbnailSize = ui::Scaled(84.0f);
    if (ImGui::BeginChild("skyGrid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
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
            // ダブルクリックでプレビューの窓を開く。**適用も一緒に動く**ので、
            // 開いた窓には必ずいま押した天球が出る。
            if (thumbnail.doubleClicked) {
                m_skyLibrary.SetActive(asset.id);
                m_showSkyPreview = true;
                ImGui::SetWindowFocus("天球プレビュー");
            }
            if (selected && m_scrollToSelectedSky) {
                m_scrollToSelectedSky = false;
                ImGui::SetScrollHereY(1.0f);
            }
            if (thumbnail.hovered) {
                ImGui::SetTooltip("%s\nダブルクリックで設定 / 右クリックでメニュー",
                                  asset.name.c_str());
            }
            // 右クリックのメニュー。**押したサムネイルが対象。**
            if (ImGui::BeginPopupContextItem("##skyMenu")) {
                m_skyLibrary.SetActive(asset.id);
                DrawSkyContextMenu(asset.id);
                ImGui::EndPopup();
            }
            ImGui::EndGroup();

            ImGui::PopID();
            if (((i + 1) % columns) != 0 && (i + 1) < assetCount) {
                ImGui::SameLine();
            }
        }

        // サムネイルの無い所での右クリック。対象が無いので「追加」だけ。
        if (ImGui::BeginPopupContextWindow("##skyGridMenu",
                                           ImGuiPopupFlags_MouseButtonRight |
                                               ImGuiPopupFlags_NoOpenOverItems)) {
            DrawSkyContextMenu(renderer::kNoSkyAsset);
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// 一覧の右クリックメニュー。**target が無効なら、対象の要る項目は出さない**
// （サムネイルの無い所を押したとき）。ボタンの帯は持たず、追加も削除もここから行う。
void Application::DrawSkyContextMenu(renderer::SkyAssetId target) {
    const std::vector<renderer::SkyAsset>& assets = m_skyLibrary.Entries();
    const renderer::SkyAsset* asset = m_skyLibrary.Find(target);

    if (asset != nullptr) {
        ImGui::TextDisabled("%s", asset->name.c_str());
        ImGui::Separator();
    }

    if (ImGui::MenuItem("追加")) {
        const renderer::SkyAssetId added =
            m_skyLibrary.Add("天球 " + std::to_string(assets.size() + 1));
        m_skyLibrary.SetActive(added);
        m_scrollToSelectedSky = true;
    }
    if (asset != nullptr) {
        if (ImGui::MenuItem("複製")) {
            const renderer::SkyAssetId added = m_skyLibrary.Duplicate(*asset);
            m_skyLibrary.SetActive(added);
            m_scrollToSelectedSky = true;
        }
        // **最後の 1 つは消させない。** 消しても既定が作り直されるだけで、
        // 効いていないように見える。
        ImGui::BeginDisabled(assets.size() <= 1);
        if (ImGui::MenuItem("削除")) {
            // その場で消すと、この後の一覧描画が erase 済みの要素を読んでしまう。
            // 要求だけ積み、フレームの外で処理する。
            m_pendingSkyRemove = target;
        }
        ImGui::EndDisabled();
    }
}

// 天球プレビューの窓。大きい絵と、その天球の設定。
//
// **映すのは適用中の天球**（一覧で選んだもの＝ビューポートの環境）。
// マテリアル / テクスチャの窓と同じ作法で、窓の側に別の選択を持たせない。
void Application::DrawSkyPreviewWindow() {
    if (!m_showSkyPreview) {
        // 閉じている間は要求を落とす。**大きい絵は窓が開いているときだけ作る。**
        // 落とさないと、設定を変えるたびに使われない絵を作り直してしまう
        // （HDRI の読み直しを伴うので、そのぶん止まる）。
        m_skyLibrary.RequestPreview(renderer::kNoSkyAsset);
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
    // 大きい絵は**窓を開いている間だけ**作る（HDRI の読み直しを伴うため）。
    m_skyLibrary.RequestPreview(active->id);

    // --- 上下 2 区画。上は幅に合わせた正方形の絵 -------------------------------
    const float paneSize = PreviewPaneSize();
    ImGui::BeginChild("skyPreviewPane", ImVec2(0.0f, paneSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        const float imageSize =
            std::max(std::min(ImGui::GetContentRegionAvail().x, paneSize), ui::Scaled(32.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - imageSize) * 0.5f));
        // まだ絵が無い（作っている最中）ときは枠だけ描く。ImTextureID の 0 を
        // AddImage へ渡すとデバッグビルドの ImGui がアサートで落ちる。
        const D3D12_GPU_DESCRIPTOR_HANDLE handle = m_skyLibrary.PreviewHandle(active->id);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + imageSize, min.y + imageSize);
        if (handle.ptr != 0) {
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(handle.ptr), min, max);
        }
        ImGui::GetWindowDrawList()->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border),
                                            ImGui::GetStyle().FrameRounding, 0, ui::Scaled(1.0f));
        ImGui::Dummy(ImVec2(imageSize, imageSize));
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("skyPropertyPane", ImVec2(0.0f, 0.0f));
    ui::HintText("一覧で選んだ天球が、そのままビューポートの環境になる");

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
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
