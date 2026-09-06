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

    // Del キーでも消せる。**このパネルにフォーカスがあるときだけ**効かせ、
    // 名前を打っている最中は無視する。
    const bool deleteKey = textureCount > 0 && !ImGui::GetIO().WantTextInput &&
                           ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                           ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    if (deleteKey) {
        RequestTextureRemove(entries[static_cast<size_t>(m_selectedTexture)].id);
    }
    DrawTextureRemoveModal();

    // サムネイルの一覧。枠の幅に入るだけ横に並べる。
    // **設定も操作のボタンも置かない**（マテリアル一覧と同じ形）。
    // 中身の確認と名前の変更はプレビューの窓、読み込みと削除は右クリックのメニュー。
    // 読み込み時にミップを作ってあるので、元の画像をそのまま縮小して出せる。
    const float thumbnailSize = ui::Scaled(72.0f);
    if (ImGui::BeginChild("textureGrid", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        if (textureCount == 0) {
            ui::HintText("右クリックのメニューから画像を読み込む（PNG / JPG / TGA / EXR）。"
                         "サムネイルのダブルクリックで中身が見られる");
        }
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
            //
            // リンク切れは絵が無いので、警告のタイルを先に敷いてから枠だけを重ねる
            // （ThumbnailButton は ptr が 0 なら絵を描かない）。
            if (entry.missing) {
                const ImVec2 min = ImGui::GetCursorScreenPos();
                ui::MissingThumbnail(min, ImVec2(min.x + thumbnailSize, min.y + thumbnailSize));
            }
            const ui::Thumbnail thumbnail =
                ui::ThumbnailButton("##thumbnail",
                                    static_cast<ImTextureID>(entry.PreviewHandle().ptr),
                                    thumbnailSize, m_selectedTexture == i);
            if (thumbnail.clicked) {
                m_selectedTexture = i;
            }
            // ダブルクリックでプレビューの窓を開く。**選択も一緒に動く**ので、
            // 開いた窓には必ずいま押したテクスチャが出る。
            if (thumbnail.doubleClicked) {
                m_selectedTexture = i;
                m_showTexturePreview = true;
                ImGui::SetWindowFocus("テクスチャプレビュー");
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
                if (entry.missing) {
                    ImGui::SetTooltip("%s\nリンク切れ: ファイルが見つかりません\n%s\n"
                                      "右クリックのメニューから繋ぎ直す",
                                      entry.name.c_str(), ToUtf8Display(entry.path).c_str());
                } else {
                    ImGui::SetTooltip("%s\nダブルクリックで詳細 / 右クリックでメニュー",
                                      entry.name.c_str());
                }
            }
            // 右クリックのメニュー。**押したサムネイルが対象。**
            if (ImGui::BeginPopupContextItem("##textureMenu")) {
                m_selectedTexture = i;
                DrawTextureContextMenu(entry.id);
                ImGui::EndPopup();
            }
            // 名前を添える。素材名は末尾で見分けが付くことが多く、
            // ホバーしないと分からないと一覧として使いにくい。
            // リンク切れは名前も警告色にして、離れて見ても分かるようにする。
            if (entry.missing) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
            }
            ui::GridCaption(entry.name.c_str(), thumbnailSize);
            if (entry.missing) {
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();

            ImGui::PopID();
            if (((i + 1) % columns) != 0 && (i + 1) < textureCount) {
                ImGui::SameLine();
            }
        }

        // サムネイルの無い所での右クリック。対象が無いので「読み込む」だけ。
        // **一覧が空のときもここから読み込む**（ボタンの帯を持たないため）。
        if (ImGui::BeginPopupContextWindow("##textureGridMenu",
                                           ImGuiPopupFlags_MouseButtonRight |
                                               ImGuiPopupFlags_NoOpenOverItems)) {
            DrawTextureContextMenu(compositor::kNoTexture);
            ImGui::EndPopup();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

// 一覧の右クリックメニュー。**target が無効なら、対象の要る項目は出さない**
// （サムネイルの無い所を押したとき）。ボタンの帯は持たず、読み込みも削除もここから行う。
void Application::DrawTextureContextMenu(compositor::TextureId target) {
    const compositor::LibraryTexture* entry = m_textureLibrary.Find(target);
    if (entry != nullptr) {
        ImGui::TextDisabled("%s", entry->name.c_str());
        ImGui::Separator();
    }

    if (ImGui::MenuItem("読み込む…")) {
        std::vector<std::filesystem::path> paths =
            ShowOpenFilesDialog(L"テクスチャを開く", ImageFileFilters());
        if (!paths.empty()) {
            m_pendingTexturePaths.insert(m_pendingTexturePaths.end(), paths.begin(), paths.end());
        }
    }
    if (entry != nullptr) {
        // 読み込み元の画像をエクスプローラで開く。**元ファイルが残っているときだけ。**
        // プロジェクトを別の環境へ移すと参照先が消えていることがあり、
        // その状態で押しても何も起きないと壊れて見える。
        std::error_code error;
        const bool exists = !entry->path.empty() && std::filesystem::exists(entry->path, error);
        ImGui::BeginDisabled(!exists);
        if (ImGui::MenuItem("ファイルの場所を開く")) {
            RevealFileInExplorer(entry->path);
        }
        ImGui::EndDisabled();
        if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("読み込み元のファイルが見つかりません");
        }

        if (entry->missing) {
            // リンク切れの解消。**一覧から消さずに残してあるのは、ここへ来るため。**
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, ui::WarnColor());
            ImGui::TextUnformatted("リンク切れ");
            ImGui::PopStyleColor();
            if (ImGui::MenuItem("ファイルを指定して繋ぎ直す…")) {
                RequestTextureRelink(target);
            }
            // 元の場所にファイルが戻っていれば、選び直さずにそのまま読める。
            ImGui::BeginDisabled(!exists);
            if (ImGui::MenuItem("元の場所から読み直す")) {
                m_pendingTextureRelinks.push_back({target, entry->path});
            }
            ImGui::EndDisabled();
            if (!exists && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("元の場所にまだファイルがない\n%s",
                                  ToUtf8Display(entry->path).c_str());
            }
        }
    }
    // リンク切れが 1 つでもあれば、フォルダごと繋ぎ直す入口を出す。
    // 素材のフォルダを丸ごと移したときに、1 枚ずつ選ばなくて済むようにする。
    if (m_textureLibrary.MissingCount() > 0) {
        if (entry == nullptr || !entry->missing) {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("フォルダを指定してまとめて繋ぎ直す…")) {
            RequestTextureRelinkFolder();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("選んだフォルダ（とその下）から、リンク切れと同じ名前の"
                              "ファイルを探して繋ぎ直す");
        }
    }
    if (entry != nullptr) {
        ImGui::Separator();
        if (ImGui::MenuItem("削除")) {
            RequestTextureRemove(target);
        }
    }
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
    const auto missing = [this](compositor::TextureId id) {
        const compositor::LibraryTexture* entry = m_textureLibrary.Find(id);
        return entry != nullptr && entry->missing;
    };
    return missing(asset.baseColor) || missing(asset.normal) || missing(asset.roughness.texture) ||
           missing(asset.metallic.texture) || missing(asset.ambientOcclusion.texture) ||
           missing(asset.height.texture);
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

// 削除の確認を出す。**参照が無くても必ず確認する。**
// テクスチャの削除は取り消せない（アンドゥの対象はレイヤーとマテリアルだけ）。
void Application::RequestTextureRemove(compositor::TextureId id) {
    if (id == compositor::kNoTexture) {
        return;
    }
    m_textureRemoveCandidate = id;
    m_textureRemoveUsers = CollectTextureUsers(id);
    ImGui::OpenPopup(kTextureRemoveModalTitle);
}

// テクスチャプレビューの窓。拡大した中身と、そのテクスチャの詳細。
//
// **映すのは一覧で選んでいるテクスチャ。** マテリアルの窓と同じ作法で、
// 窓の側に別の選択を持たせない。
void Application::DrawTexturePreviewWindow() {
    if (!m_showTexturePreview) {
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

        ui::PropertyLabel("場所", "プロジェクトにはここへの相対パスを記録する");
        const std::string directory = ToUtf8Display(selected.path.parent_path());
        ImGui::TextUnformatted(directory.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", ToUtf8Display(selected.path).c_str());
        }
        ui::PropertyEnd();

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
