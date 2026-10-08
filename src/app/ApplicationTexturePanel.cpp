// テクスチャパネル。**一覧（サムネイル）だけ**を置き、詳細はプレビューの窓が持つ。
// 削除の確認モーダルと参照箇所の収集もここ。

#include "app/Application.h"

#include "app/ApplicationUiHelpers.h"
#include "core/FileDialog.h"
#include "core/Log.h"
#include "core/Shell.h"
#include "io/ProjectIo.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <DirectXMath.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace tg {

size_t Application::CountTextureUsers(compositor::TextureId id) const {
    if (id == compositor::kNoTexture) {
        return 0;
    }
    size_t count = 0;
    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        count += (asset.baseColor == id) ? 1 : 0;
        count += (asset.normal == id) ? 1 : 0;
        count += (asset.roughness.texture == id) ? 1 : 0;
        count += (asset.metallic.texture == id) ? 1 : 0;
        count += (asset.ambientOcclusion.texture == id) ? 1 : 0;
        count += (asset.height.texture == id) ? 1 : 0;
        count += (asset.opacity.texture == id) ? 1 : 0;
    }
    for (const graph::Node& node : m_graph.Nodes()) {
        const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings);
        if (settings == nullptr) {
            continue;
        }
        count += (settings->layer.mask.texture.texture == id) ? 1 : 0;
        count += (settings->layer.heightTexture.texture == id) ? 1 : 0;
        count += (settings->layer.pathUv.mask.texture == id) ? 1 : 0;
    }
    return count;
}

// ファイルを選ばせて、繋ぎ直しを予約する。読み込みはフレームの外で行う。
void Application::RequestTextureRelink(compositor::TextureId id) {
    const compositor::LibraryTexture* entry = m_textureLibrary.Find(id);
    if (entry == nullptr) {
        return;
    }
    const std::filesystem::path path =
        ShowOpenFileDialog(L"繋ぎ直すファイルを選ぶ", ImageFileFilters());
    if (!path.empty()) {
        m_pendingTextureRelinks.push_back({id, path});
    }
}

// フォルダを選ばせ、その下にリンク切れと同じファイル名があれば繋ぎ直しを予約する。
//
// 探すのはファイル名だけ。元のフォルダ構成まで一致させる必要はない
// （素材をまとめて別のフォルダへ移すのが普通で、階層は変わりがち）。
// 同じ名前が複数あるときは最初に見つかったものを使う。
void Application::RequestTextureRelinkFolder() {
    const std::filesystem::path folder = ShowPickFolderDialog(L"素材のフォルダを選ぶ");
    if (folder.empty()) {
        return;
    }

    // 先にフォルダの中身を 1 度だけ歩いて、名前 → パスの表を作る。
    // リンク切れごとに歩き直すと、大きな素材集では待たされる。
    std::vector<std::pair<std::wstring, std::filesystem::path>> files;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             folder, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error)) {
            std::wstring name = it->path().filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            files.emplace_back(std::move(name), it->path());
        }
    }

    size_t matched = 0;
    for (const compositor::LibraryTexture& entry : m_textureLibrary.Entries()) {
        if (!entry.missing) {
            continue;
        }
        std::wstring wanted = entry.path.filename().wstring();
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::towlower);
        const auto found = std::find_if(files.begin(), files.end(),
                                        [&](const auto& file) { return file.first == wanted; });
        if (found != files.end()) {
            m_pendingTextureRelinks.push_back({entry.id, found->second});
            ++matched;
        }
    }
    if (matched == 0) {
        TG_LOG_WARN("フォルダにリンク切れと同じ名前のファイルがありません: %s",
                    ToUtf8Portable(folder).c_str());
    }
}

bool Application::MaterialHasMissingTexture(const compositor::MaterialAsset& asset) const {
    if (asset.layerMaterial) {
        const auto missingSource = [&](const graph::PresetMaterial& layer) {
            if (!layer.material) return false;
            const auto* source = m_materialLibrary.Find(layer.material);
            return !source || source->layerMaterial || MaterialHasMissingTexture(*source);
        };
        for (const auto& layer : asset.layerMaterial->materials) if (missingSource(layer)) return true;
        if (asset.layerMaterial->materialGraph) for (const auto& node : asset.layerMaterial->materialGraph->nodes) if (missingSource(node.settings)) return true;
        return !asset.layerError.empty();
    }
    const auto missing = [this](compositor::TextureId id) {
        const compositor::LibraryTexture* entry = m_textureLibrary.Find(id);
        return entry != nullptr && entry->missing;
    };
    return missing(asset.baseColor) || missing(asset.normal) || missing(asset.roughness.texture) ||
           missing(asset.metallic.texture) || missing(asset.ambientOcclusion.texture) ||
           missing(asset.height.texture) || missing(asset.opacity.texture);
}

// 予約した繋ぎ直しを処理する。**フレームの外で呼ぶこと**（読み込みは GPU 待機を伴う）。
void Application::ProcessPendingTextureRelinks() {
    if (m_pendingTextureRelinks.empty()) {
        return;
    }
    std::vector<TextureRelink> relinks;
    relinks.swap(m_pendingTextureRelinks);

    bool relinked = false;
    for (const TextureRelink& relink : relinks) {
        if (m_textureLibrary.Relink(m_device, m_pipelineCache, relink.id, relink.path)) {
            relinked = true;
        }
    }
    if (relinked) {
        // 繋ぎ直した画像を参照しているサムネイルと合成を作り直す。
        // どれが参照しているかを追うより、全部を予約する方が確実で安い。
        for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
            m_materialLibrary.MarkThumbnailDirty(asset.id);
        }
        m_graphStack.MarkDirty();
    }
}

// テクスチャプレビューの窓。拡大した中身と、そのテクスチャの詳細。
//
// **映すのは一覧で選んでいるテクスチャ。** マテリアルの窓と同じ作法で、
// 窓の側に別の選択を持たせない。
void Application::DrawTexturePreviewWindow() {
    if (!m_showTexturePreview || HiddenWithAssetBand("テクスチャプレビュー")) {
        return;
    }

    // 縦長。画像の下に詳細が続くので、幅は 1 列ぶんあれば足りる。
    // **窓そのものはスクロールさせない。** 中身は上下 2 つの区画で、
    // スクロールするのは下（詳細）だけ。
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(420.0f), ui::Scaled(620.0f)),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("テクスチャプレビュー", &m_showTexturePreview,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    const std::vector<compositor::LibraryTexture>& entries = m_textureLibrary.Entries();
    if (entries.empty()) {
        ui::HintText("テクスチャがない。「テクスチャ」パネルの右クリックから読み込む");
        ImGui::End();
        return;
    }

    const int index = std::clamp(m_selectedTexture, 0, static_cast<int>(entries.size()) - 1);
    const compositor::LibraryTexture& selected = entries[static_cast<size_t>(index)];

    // --- 上下 2 区画 ----------------------------------------------------------
    // 上が拡大した絵、下が詳細。**スクロールするのは下だけ**で、
    // 上は幅に合わせた正方形（マテリアルの窓と同じ）。
    const float paneSize = PreviewPaneSize();

    ImGui::BeginChild("texturePreviewPane", ImVec2(0.0f, paneSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    {
        // コンボは 0 が RGB なので、1 つずらして R / G / B / A の SRV を引く。
        // 0（RGB）のときは -1 になり、ChannelHandle が通常の表示用を返す。
        const float imageSize =
            std::max(std::min(ImGui::GetContentRegionAvail().x, paneSize), ui::Scaled(32.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             std::max(0.0f, (ImGui::GetContentRegionAvail().x - imageSize) * 0.5f));
        if (selected.missing) {
            // 絵が無い。ImGui::Image に 0 を渡すとアサートで落ちるので、警告のタイルを出す。
            const ImVec2 min = ImGui::GetCursorScreenPos();
            ui::MissingThumbnail(min, ImVec2(min.x + imageSize, min.y + imageSize));
            ImGui::Dummy(ImVec2(imageSize, imageSize));
        } else {
            ImGui::Image(
                static_cast<ImTextureID>(selected.ChannelHandle(m_previewChannel - 1).ptr),
                ImVec2(imageSize, imageSize));
        }
    }
    ImGui::EndChild();

    ImGui::Separator();

    ImGui::BeginChild("texturePropertyPane", ImVec2(0.0f, 0.0f));
    ui::SectionHeader("選択中");
    if (ui::BeginPropertyTable("textureRows")) {
        // ORD のように 1 枚へ複数のマップを詰めたテクスチャは、RGB のまま見ても
        // 意味が読めない。チャンネルを分けて確かめられるようにする。
        static const char* const kPreviewChannelLabels[] = {"RGB", "R", "G", "B", "A"};
        ui::PropertyCombo("チャンネル", &m_previewChannel, kPreviewChannelLabels,
                          IM_ARRAYSIZE(kPreviewChannelLabels), 0,
                          "1 枚に複数のマップを詰めたテクスチャ（ORD など）の中身を確かめる。"
                          "R / G / B を選ぶとそのチャンネルだけを灰色で出す");

        char nameBuffer[128] = {};
        std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", selected.name.c_str());
        if (ui::PropertyTextInput("名前", nameBuffer, sizeof(nameBuffer),
                                  "一覧とマップ欄に出る名前。画像ファイルの名前は変わらない")) {
            if (compositor::LibraryTexture* mutableEntry =
                    m_textureLibrary.FindMutable(selected.id);
                mutableEntry != nullptr) {
                mutableEntry->name = nameBuffer;
            }
        }
        if (selected.missing) {
            // 中身が無いので解像度などは出せない。代わりに状態と、繋ぎ直す入口を置く。
            ui::PropertyLabel("状態", "読み込み元のファイルが見つからない。"
                                      "参照は保ってあるので、繋ぎ直せば元どおりになる");
            ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
            ImGui::TextUnformatted("リンク切れ");
            ImGui::PopStyleColor();
            ui::PropertyEnd();
        } else {
            ui::PropertyValue("解像度", "%u x %u", selected.texture.width,
                              selected.texture.height);
            ui::PropertyValue("ミップ", "%u 段", selected.texture.mipLevels);
            ui::PropertyValue("形式", "%s", TextureFormatLabel(selected));
        }
        ui::PropertyValue("参照", "%zu か所", CountTextureUsers(selected.id));

        DrawAssetPathRow("場所", selected.path, m_pendingAssetReveal);

        if (selected.missing) {
            ui::PropertyLabelEmpty("relink");
            if (ui::Button("繋ぎ直す…", ui::kWideButtonWidth)) {
                RequestTextureRelink(selected.id);
            }
            ui::PropertyEnd();
        }
        ui::EndPropertyTable();
    }
    ui::HintText("一覧のサムネイルをマテリアルのマップ欄へドラッグすると割り当てられる");
    ImGui::EndChild();

    ImGui::End();
}

}  // namespace tg
