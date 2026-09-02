// 合成結果を画像へ書き出すウィンドウ（ファイル > テクスチャを書き出す…）。
//
// **設定を決めるだけで、実行はここでやらない。** 書き出しは指定解像度での再合成と
// 読み戻しを伴うので、要求だけ積んでフレームの外（ProcessPendingFileWork）で処理する。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/PathUtf8.h"
#include "ui/UiStyle.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

namespace tg {
namespace {

const char* const kPackingLabels[] = {"個別", "ORD", "ORM"};
// 合成解像度の一覧（kResolutionLabels）とは別物。書き出しは 8192 まで選べる。
const char* const kExportResolutionLabels[] = {"1024", "2048", "4096", "8192"};

// 設定から、書き出されるファイル名を並べる。**押す前に何が出るか見せる。**
// 枚数と名前が分かっていないと、既存のファイルを上書きするのかが判断できない。
std::string PreviewFileNames(const io::ExportSettings& settings) {
    std::string names;
    const auto add = [&names, &settings](const char* suffix, const char* extension) {
        if (!names.empty()) {
            names += " / ";
        }
        names += settings.baseName + "_" + suffix + "." + extension;
    };
    if (settings.baseColor) {
        add("BaseColor", "png");
    }
    if (settings.normal) {
        add("Normal", "png");
    }
    if (settings.surface) {
        switch (settings.packing) {
            case io::ExportPacking::Ord: add("ORD", "png"); break;
            case io::ExportPacking::Orm: add("ORM", "png"); break;
            default:
                add("Roughness", "png");
                add("Metallic", "png");
                add("AO", "png");
                break;
        }
    }
    if (settings.height) {
        add("Height", settings.heightAsExr ? "exr" : "png");
    }
    return names.empty() ? "（書き出すものがありません）" : names;
}

}  // namespace

void Application::DrawExportWindow() {
    if (!m_showExport) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(520.0f), ui::Scaled(620.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("テクスチャを書き出す", &m_showExport)) {
        ImGui::End();
        return;
    }

    io::ExportSettings& settings = m_exportSettings;
    const io::ExportSettings defaults;

    ui::SectionHeader("出力先");
    if (ui::BeginPropertyTable("exportPathRows")) {
        ui::PropertyValue("フォルダ", "%s",
                          settings.directory.empty()
                              ? "（未指定）"
                              : ToUtf8Display(settings.directory).c_str());
        if (!settings.directory.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ToUtf8Display(settings.directory).c_str());
        }

        ui::PropertyLabelEmpty("exportPick");
        if (ui::Button("フォルダを選ぶ…", ui::kWideButtonWidth)) {
            const std::filesystem::path picked =
                ShowPickFolderDialog(L"書き出し先を選ぶ", settings.directory);
            if (!picked.empty()) {
                settings.directory = picked;
            }
        }
        ui::PropertyEnd();

        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", settings.baseName.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer),
                                  "ファイル名の頭。後ろにマップの種類が付く")) {
            settings.baseName = nameBuffer;
        }
        ui::EndPropertyTable();
    }

    ui::SectionHeader("形式");
    if (ui::BeginPropertyTable("exportFormatRows")) {
        int resolution = 1;
        for (int i = 0; i < IM_ARRAYSIZE(io::kExportResolutions); ++i) {
            if (io::kExportResolutions[i] == settings.resolution) {
                resolution = i;
            }
        }
        if (ui::PropertyCombo("解像度", &resolution, kExportResolutionLabels,
                              IM_ARRAYSIZE(kExportResolutionLabels), 1,
                              "書き出しだけの解像度。プレビューの合成解像度は変わらない。"
                              "高くするほど再合成と読み戻しに時間と VRAM がかかる")) {
            settings.resolution = io::kExportResolutions[resolution];
        }

        int packing = static_cast<int>(settings.packing);
        if (ui::PropertyCombo("パッキング", &packing, kPackingLabels,
                              IM_ARRAYSIZE(kPackingLabels),
                              static_cast<int>(defaults.packing),
                              "ラフネス / メタルネス / AO の詰め方。"
                              "ORD は AO=R・ラフネス=G・ハイト=B（Megascans）、"
                              "ORM は AO=R・ラフネス=G・メタルネス=B（glTF / Unreal）")) {
            settings.packing = static_cast<io::ExportPacking>(packing);
        }
        ui::EndPropertyTable();
    }

    ui::SectionHeader("書き出すマップ");
    if (ui::BeginPropertyTable("exportMapRows")) {
        ui::PropertyBool("ベースカラー", &settings.baseColor, defaults.baseColor,
                         "sRGB の 8bit PNG");
        ui::PropertyBool("法線", &settings.normal, defaults.normal,
                         "接空間。0.5 が平坦になる一般的な並びへ直して書く");
        ui::PropertyBool("サーフェス", &settings.surface, defaults.surface,
                         "ラフネス / メタルネス / AO。パッキングの指定に従う");
        ui::PropertyBool("ハイト", &settings.height, defaults.height, "");
        ImGui::BeginDisabled(!settings.height);
        ui::PropertyBool("EXR で書く", &settings.heightAsExr, defaults.heightAsExr,
                         "8bit の PNG に落とすと段差が見える。"
                         "切ると 8bit PNG になり、地形の高さには使えない");
        ImGui::EndDisabled();
        ui::EndPropertyTable();
    }
    ui::HintText(PreviewFileNames(settings).c_str());

    ImGui::Spacing();
    const bool ready = !settings.directory.empty() && !settings.baseName.empty() &&
                       (settings.baseColor || settings.normal || settings.surface ||
                        settings.height);
    ImGui::BeginDisabled(!ready || m_pendingExport);
    if (ui::Button("書き出す", ui::kWideButtonWidth)) {
        m_pendingExport = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Button("閉じる", ui::kWideButtonWidth)) {
        m_showExport = false;
    }
    if (!ready) {
        ui::HintText("フォルダと名前を決め、書き出すマップを 1 つ以上選ぶ");
    }

    ImGui::End();
}

}  // namespace tg
