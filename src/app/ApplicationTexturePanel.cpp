// テクスチャパネル。一覧、詳細、削除の確認モーダル、参照箇所の収集。

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

// このテクスチャを使っている場所を、人が読める形で並べる。
//
// 削除の確認で「どこが壊れるか」を出すために使う。件数だけだと、
// 消していいのか判断できない。
std::vector<std::string> Application::CollectTextureUsers(compositor::TextureId id) const {
    std::vector<std::string> users;
    if (id == compositor::kNoTexture) {
        return users;
    }

    for (const compositor::MaterialAsset& asset : m_materialLibrary.Entries()) {
        const auto add = [&](const char* slotName) {
            users.push_back("マテリアル「" + asset.name + "」の" + slotName);
        };
        if (asset.baseColor == id) {
            add("ベースカラー");
        }
        if (asset.normal == id) {
            add("法線");
        }
        if (asset.roughness.texture == id) {
            add("ラフネス");
        }
        if (asset.metallic.texture == id) {
            add("メタルネス");
        }
        if (asset.ambientOcclusion.texture == id) {
            add("AO");
        }
        if (asset.height.texture == id) {
            add("ハイト");
        }
    }

    for (const graph::Node& node : m_graph.Nodes()) {
        const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings);
        if (settings == nullptr) {
            continue;
        }
        if (settings->layer.mask.texture.texture == id) {
            users.push_back("ノード「" + settings->layer.name + "」のマスク");
        }
        if (settings->layer.heightTexture.texture == id) {
            users.push_back("ノード「" + settings->layer.name + "」のハイト");
        }
    }
    return users;
}

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
    }
    for (const graph::Node& node : m_graph.Nodes()) {
        const auto* settings = std::get_if<graph::LayerNodeSettings>(&node.settings);
        if (settings == nullptr) {
            continue;
        }
        count += (settings->layer.mask.texture.texture == id) ? 1 : 0;
        count += (settings->layer.heightTexture.texture == id) ? 1 : 0;
    }
    return count;
}

// 参照が残っているテクスチャを消そうとしたときの確認。
//
// 消しても壊れはしない（参照は削除時に「なし」へ落ちる）が、
// **黙って落とすと、どのマテリアルが変わったのか分からなくなる。**
// どこで使われているかを並べて、消すかどうかを決めてもらう。
void Application::DrawTextureRemoveModal() {
    // ビューポート中央に出す。ドックのどこにパネルがあっても同じ位置に出したい。
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (!ImGui::BeginPopupModal(kTextureRemoveModalTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const compositor::LibraryTexture* entry = m_textureLibrary.Find(m_textureRemoveCandidate);
    const char* const name = (entry != nullptr) ? entry->name.c_str() : "?";

    if (m_textureRemoveUsers.empty()) {
        ImGui::Text("「%s」を削除します。", name);
        ImGui::Spacing();
        ui::HintText("どこからも使われていない。画像ファイルは消えない");
    } else {
        ImGui::Text("「%s」は %zu か所で使われています。", name, m_textureRemoveUsers.size());
        ImGui::Spacing();

        // 使用箇所。多いときは枠を作ってスクロールさせる（窓が縦に伸びきらないように）。
        constexpr size_t kMaxRowsWithoutScroll = 8;
        const bool scroll = m_textureRemoveUsers.size() > kMaxRowsWithoutScroll;
        const float listHeight =
            scroll ? ImGui::GetTextLineHeightWithSpacing() * kMaxRowsWithoutScroll : 0.0f;
        if (ImGui::BeginChild("textureRemoveUsers", ImVec2(ui::Scaled(360.0f), listHeight),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            for (const std::string& user : m_textureRemoveUsers) {
                ImGui::BulletText("%s", user.c_str());
            }
        }
        ImGui::EndChild();

        ImGui::Spacing();
        ui::HintText("削除すると、これらの割り当ては「なし」に戻る");
    }
    ImGui::Spacing();

    const auto close = [this]() {
        m_textureRemoveCandidate = compositor::kNoTexture;
        m_textureRemoveUsers.clear();
        ImGui::CloseCurrentPopup();
    };

    if (ui::Button("削除する", ui::kWideButtonWidth)) {
        m_pendingTextureRemove = m_textureRemoveCandidate;
        close();
    }
    ImGui::SameLine();
    if (ui::Button("やめる", ui::kWideButtonWidth)) {
        close();
    }
    // Esc でも閉じられるようにする。確認はいつでも降りられる方がよい。
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        close();
    }

    ImGui::EndPopup();
}

// 読み込んだ画像の一覧。マテリアルのマップはここから割り当てる。
void Application::DrawTextureLibraryPanel() {
    if (!ImGui::Begin("テクスチャ")) {
        ImGui::End();
        return;
    }

    const std::vector<compositor::LibraryTexture>& entries = m_textureLibrary.Entries();
    const auto textureCount = static_cast<int>(entries.size());
    m_selectedTexture = std::clamp(m_selectedTexture, 0, (textureCount > 0) ? textureCount - 1 : 0);

    if (ui::Button("読み込む…", ui::kWideButtonWidth)) {
        std::vector<std::filesystem::path> paths =
            ShowOpenFilesDialog(L"テクスチャを開く", ImageFileFilters());
        if (!paths.empty()) {
            m_pendingTexturePaths.insert(m_pendingTexturePaths.end(), paths.begin(), paths.end());
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(textureCount == 0);
    const bool deletePressed = ui::Button("削除");
    ImGui::EndDisabled();

    // Del キーでも消せる。**このパネルにフォーカスがあるときだけ**効かせ、
    // 名前を打っている最中は無視する。
    const bool deleteKey = textureCount > 0 && !ImGui::GetIO().WantTextInput &&
                           ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                           ImGui::IsKeyPressed(ImGuiKey_Delete, false);

    if (deletePressed || deleteKey) {
        // **参照が無くても必ず確認する。** テクスチャの削除は取り消せないため
        // （アンドゥの対象はレイヤーとマテリアルだけ）。
        m_textureRemoveCandidate = entries[static_cast<size_t>(m_selectedTexture)].id;
        m_textureRemoveUsers = CollectTextureUsers(m_textureRemoveCandidate);
        ImGui::OpenPopup(kTextureRemoveModalTitle);
    }
    DrawTextureRemoveModal();

    // **一覧と詳細は区画に割らず、パネル 1 枚をそのままスクロールさせる。**
    // 帯は縦に狭いので、割ると両方が窮屈になる。一覧を全幅・全高で流し、
    // 詳細はその下（スクロールした先）に置く。
    // 一覧には全幅を使わせる。詳細を横へ置くと一覧の幅がその分だけ削れ、
    // 帯が横に広くても枡が数列しか並ばない。

    // サムネイルの一覧。枠の幅に入るだけ横に並べる。
    // 読み込み時にミップを作ってあるので、元の画像をそのまま縮小して出せる。
    const float thumbnailSize = ui::Scaled(72.0f);
    {
        const float step = thumbnailSize + ImGui::GetStyle().ItemSpacing.x;
        const auto columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / step));

        for (int i = 0; i < textureCount; ++i) {
            const compositor::LibraryTexture& entry = entries[static_cast<size_t>(i)];
            ImGui::PushID(static_cast<int>(entry.id));

            ImGui::BeginGroup();
            // リニアなテクスチャ（EXR）は表示用に焼き直したものを描く。
            // 元のまま描くと極端に暗く、一覧で見分けられない。
            //
            // ThumbnailButton は ID を持つアイテムとして置く。ImGui::Image() では
            // ID が無く、この後の BeginDragDropSource() がドラッグを始められない。
            const ui::Thumbnail thumbnail =
                ui::ThumbnailButton("##thumbnail",
                                    static_cast<ImTextureID>(entry.PreviewHandle().ptr),
                                    thumbnailSize, m_selectedTexture == i);
            if (thumbnail.clicked) {
                m_selectedTexture = i;
            }
            // 読み込んだ直後のものは枠内へ送る。一覧はスクロールするので、
            // 追加しただけでは見えない位置に入ることがある。
            if (m_selectedTexture == i && m_scrollToSelectedTexture) {
                m_scrollToSelectedTexture = false;
                ImGui::SetScrollHereY(1.0f);
            }
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
                // マテリアルのマップ欄へ落とすと、そのスロットに割り当たる。
                ImGui::SetDragDropPayload(kTextureDragDropType, &entry.id,
                                          sizeof(compositor::TextureId));
                ImGui::TextUnformatted(entry.name.c_str());
                ImGui::EndDragDropSource();
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", entry.name.c_str());
            }
            // 名前を添える。素材名は末尾で見分けが付くことが多く、
            // ホバーしないと分からないと一覧として使いにくい。
            ui::GridCaption(entry.name.c_str(), thumbnailSize);
            ImGui::EndGroup();

            ImGui::PopID();
            if (((i + 1) % columns) != 0 && (i + 1) < textureCount) {
                ImGui::SameLine();
            }
        }
    }

    if (textureCount == 0) {
        ui::HintText("「読み込む…」で画像を読み込む（PNG / JPG / TGA / EXR）");
        ImGui::End();
        return;
    }

    const compositor::LibraryTexture& selected = entries[static_cast<size_t>(m_selectedTexture)];
    // 拡大プレビュー。サムネイル（72）では中身を確かめられないため置く。
    // **大きさは固定。** 流し込みなので伸ばす先の高さが決まらない。
    //
    // コンボは 0 が RGB なので、1 つずらして R / G / B / A の SRV を引く。
    // 0（RGB）のときは -1 になり、ChannelHandle が通常の表示用を返す。
    const float previewSize = ui::Scaled(kTexturePreviewSize);
    ImGui::Image(static_cast<ImTextureID>(selected.ChannelHandle(m_previewChannel - 1).ptr),
                 ImVec2(previewSize, previewSize));
    // プロパティ行はプレビューの横へ回す。**下に積むとスクロールが長くなり、
    // 何を選んでいるのかを見ながら値を確かめられない。**
    ImGui::SameLine();
    ImGui::BeginGroup();

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
        ui::PropertyValue("解像度", "%u x %u", selected.texture.width, selected.texture.height);
        ui::PropertyValue("ミップ", "%u 段", selected.texture.mipLevels);
        ui::PropertyValue("形式", "%s", TextureFormatLabel(selected));
        ui::PropertyValue("参照", "%zu か所", CountTextureUsers(selected.id));

        ui::PropertyLabel("場所", "プロジェクトにはここへの相対パスを記録する");
        const std::string directory = ToUtf8Display(selected.path.parent_path());
        ImGui::TextUnformatted(directory.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ToUtf8Display(selected.path).c_str());
        }
        ui::PropertyEnd();
        ui::EndPropertyTable();
    }
    ui::HintText("サムネイルをマテリアルのマップ欄へドラッグすると割り当てられる");

    ImGui::EndGroup();
    ImGui::End();
}

}  // namespace tg
