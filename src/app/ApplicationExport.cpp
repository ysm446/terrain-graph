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

const ui::LocalizedText kPackingLabels[] = {{"個別", "Separate"}, {"ORD", "ORD"}, {"ORM", "ORM"}};
// 合成解像度の一覧（kMaterialResolutionLabels）とは別物。書き出しは 512 を持たない。
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
    return names.empty() ? ui::Tr("（書き出すものがありません）", "(Nothing to export)") : names;
}

}  // namespace

void Application::DrawExportWindow() {
    if (!m_showExport) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(520.0f), ui::Scaled(620.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ui::Tr("テクスチャを書き出す###ExportTextures", "Export Textures###ExportTextures"), &m_showExport)) {
        ImGui::End();
        return;
    }

    io::ExportSettings& settings = m_exportSettings;
    const io::ExportSettings defaults;

    ui::SectionHeader(ui::Tr("出力先", "Destination"));
    if (ui::BeginPropertyTable("exportPathRows")) {
        ui::PropertyValue(ui::Tr("フォルダ", "Folder"), "%s",
                          settings.directory.empty()
                              ? ui::Tr("（未指定）", "(None)")
                              : ToUtf8Display(settings.directory).c_str());
        if (!settings.directory.empty() && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ToUtf8Display(settings.directory).c_str());
        }

        ui::PropertyLabelEmpty("exportPick");
        if (ui::Button(ui::Tr("フォルダを選ぶ…", "Choose Folder…"), ui::kWideButtonWidth)) {
            const std::filesystem::path picked =
                ShowPickFolderDialog(L"書き出し先を選ぶ", settings.directory);
            if (!picked.empty()) {
                settings.directory = picked;
            }
        }
        ui::PropertyEnd();

        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", settings.baseName.c_str());
        if (ui::PropertyTextInput(ui::Tr("名前", "Name"), nameBuffer, sizeof(nameBuffer),
                                  ui::Tr("ファイル名の頭。後ろにマップの種類が付く",
                                         "File name prefix. The map type is appended"))) {
            settings.baseName = nameBuffer;
        }
        ui::EndPropertyTable();
    }

    ui::SectionHeader(ui::Tr("形式", "Format"));
    if (ui::BeginPropertyTable("exportFormatRows")) {
        int resolution = 1;
        for (int i = 0; i < IM_ARRAYSIZE(io::kExportResolutions); ++i) {
            if (io::kExportResolutions[i] == settings.resolution) {
                resolution = i;
            }
        }
        if (ui::PropertyCombo(ui::Tr("解像度", "Resolution"), &resolution, kExportResolutionLabels,
                              IM_ARRAYSIZE(kExportResolutionLabels), 1,
                              ui::Tr("書き出しだけの解像度。プレビューの合成解像度は変わらない。"
                                     "高くするほど再合成と読み戻しに時間と VRAM がかかる",
                                     "Resolution for export only. The preview composite resolution does not change. "
                                     "Higher takes more time and VRAM to recomposite and read back"))) {
            settings.resolution = io::kExportResolutions[resolution];
        }

        int packing = static_cast<int>(settings.packing);
        if (ui::PropertyCombo(ui::Tr("パッキング", "Packing"), &packing, ui::Tr(kPackingLabels).data(),
                              IM_ARRAYSIZE(kPackingLabels),
                              static_cast<int>(defaults.packing),
                              ui::Tr("ラフネス / メタルネス / AO の詰め方。"
                                     "ORD は AO=R・ラフネス=G・ハイト=B（Megascans）、"
                                     "ORM は AO=R・ラフネス=G・メタルネス=B（glTF / Unreal）",
                                     "How roughness / metalness / AO are packed. "
                                     "ORD is AO=R, roughness=G, height=B (Megascans); "
                                     "ORM is AO=R, roughness=G, metalness=B (glTF / Unreal)"))) {
            settings.packing = static_cast<io::ExportPacking>(packing);
        }
        ui::EndPropertyTable();
    }

    ui::SectionHeader(ui::Tr("書き出すマップ", "Maps to Export"));
    if (ui::BeginPropertyTable("exportMapRows")) {
        ui::PropertyBool(ui::Tr("ベースカラー", "Base Color"), &settings.baseColor, defaults.baseColor,
                         ui::Tr("sRGB の 8bit PNG", "8-bit sRGB PNG"));
        ui::PropertyBool(ui::Tr("法線", "Normal"), &settings.normal, defaults.normal,
                         ui::Tr("接空間。0.5 が平坦になる一般的な並びへ直して書く",
                                "Tangent space. Written in the common layout where 0.5 is flat"));
        ui::PropertyBool(ui::Tr("サーフェス", "Surface"), &settings.surface, defaults.surface,
                         ui::Tr("ラフネス / メタルネス / AO。パッキングの指定に従う",
                                "Roughness / metalness / AO. Follows the packing setting"));
        ui::PropertyBool(ui::Tr("ハイト", "Height"), &settings.height, defaults.height, "");
        ImGui::BeginDisabled(!settings.height);
        ui::PropertyBool(ui::Tr("EXR で書く", "Write EXR"), &settings.heightAsExr, defaults.heightAsExr,
                         ui::Tr("8bit の PNG に落とすと段差が見える。"
                                "切ると 8bit PNG になり、地形の高さには使えない",
                                "An 8-bit PNG shows banding. "
                                "When off, writes an 8-bit PNG that is unusable for terrain height"));
        ImGui::EndDisabled();
        ui::EndPropertyTable();
    }
    // 書き出し名はユーザーの文字列なので、書式として渡さない。
    ui::HintText("%s", PreviewFileNames(settings).c_str());

    ImGui::Spacing();
    const bool ready = !settings.directory.empty() && !settings.baseName.empty() &&
                       (settings.baseColor || settings.normal || settings.surface ||
                        settings.height);
    ImGui::BeginDisabled(!ready || m_pendingExport);
    if (ui::Button(ui::Tr("書き出す", "Export"), ui::kWideButtonWidth)) {
        m_pendingExport = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ui::Button(ui::Tr("閉じる", "Close"), ui::kWideButtonWidth)) {
        m_showExport = false;
    }
    if (!ready) {
        ui::HintText("%s", ui::Tr("フォルダと名前を決め、書き出すマップを 1 つ以上選ぶ",
                                   "Set a folder and name, and choose at least one map to export"));
    }

    ImGui::End();
}

}  // namespace tg
