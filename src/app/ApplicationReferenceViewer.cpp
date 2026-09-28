// 参照ビューア（UE5 の Reference Viewer に倣う）。
//
// 中心のアセットを真ん中に置き、左へ参照元（それを使っているもの）、右へ参照先（それが使っているもの）を
// 段ごとの列で並べ、参照する側から参照される側へ線を引く。箱のダブルクリックで中心を移す。
// 対応表（io::AssetReferenceIndex）はワークスペース全体の文書を読むので、開いたときと「更新」のときだけ
// 作り直す（ProcessAssetWork）。中心や深さを変えても表を引き直すだけで、ファイルは読まない。

#include "app/Application.h"
#include "app/ApplicationUiHelpers.h"
#include "core/Shell.h"
#include "ui/UiStyle.h"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace tg {
namespace fs = std::filesystem;
namespace ed = ax::NodeEditor;
namespace {

// 箱と列の寸法（キャンバス座標）。
constexpr float kBoxWidth = 320.0f;
constexpr float kThumbnail = 48.0f;
constexpr float kColumnStep = 440.0f;
constexpr float kRowStep = 92.0f;
constexpr float kBoxPadding = 8.0f;
// 箱の数の上限。よく使うテクスチャを中心にすると参照元が数百になり、描いても読めないため。
constexpr size_t kMaxNodes = 240;

// ノードとピンの ID。箱の添字から作る（0 はエディタが「無し」に使う）。
ed::NodeId NodeIdOf(size_t index) { return ed::NodeId(index * 4 + 1); }
ed::PinId InputPinOf(size_t index) { return ed::PinId(index * 4 + 2); }
ed::PinId OutputPinOf(size_t index) { return ed::PinId(index * 4 + 3); }
size_t IndexOfNode(ed::NodeId id) { return static_cast<size_t>((id.Get() - 1) / 4); }

}  // namespace

void Application::OpenReferenceViewer(const fs::path& center) {
    m_referenceCenter = center;
    m_referenceFollowed = center;
    m_showReferenceViewer = true;
    // 開くたびに表を作り直す（ファイルを編集したあとに開いたとき、古い関係を見せないため）。
    m_referenceIndexDirty = true;
    m_referenceLayoutDirty = true;
    // 前へ出すのは次に窓を描くとき（起動時のオプションからも呼ばれ、まだ ImGui のフレームが無い）。
    m_referenceFocus = true;
}

void Application::DestroyReferenceViewer() {
    if (m_referenceEditor != nullptr) {
        ed::DestroyEditor(m_referenceEditor);
        m_referenceEditor = nullptr;
    }
}

// 中心から左右へ幅優先で広げ、段ごとの列に並べる。同じアセットは最初に届いた所に 1 つだけ置き、
// 線は置いた箱どうしの参照すべてに引く（右と左の両方から届くアセットも箱は 1 つ）。
void Application::LayoutReferenceViewer() {
    m_referenceLayoutDirty = false;
    m_referenceNodes.clear();
    m_referenceLinks.clear();
    m_referenceHidden = 0;
    if (m_referenceCenter.empty()) return;

    const io::AssetReferenceIndex& index = m_referenceIndex;
    const io::AssetReferenceIndex::Asset* center = index.Find(m_referenceCenter);
    std::unordered_map<size_t, size_t> placed;  // 表の添字 → 箱の添字
    std::vector<size_t> assetOf;                // 箱の添字 → 表の添字（中心が表に無ければ SIZE_MAX）
    m_referenceNodes.push_back({center ? center->path : m_referenceCenter, 0, {}});
    assetOf.push_back(center ? index.IndexOf(*center) : SIZE_MAX);
    if (center != nullptr) placed.emplace(index.IndexOf(*center), 0);

    const auto grow = [&](int direction, int depth) {
        std::vector<size_t> frontier{center ? index.IndexOf(*center) : SIZE_MAX};
        for (int level = 1; level <= depth && center != nullptr; ++level) {
            std::vector<size_t> next;
            for (const size_t from : frontier) {
                const auto& edges = direction > 0 ? index.assets[from].references : index.assets[from].referencers;
                for (const size_t to : edges) {
                    if (placed.contains(to)) continue;
                    if (m_referenceNodes.size() >= kMaxNodes) { ++m_referenceHidden; continue; }
                    placed.emplace(to, m_referenceNodes.size());
                    m_referenceNodes.push_back({index.assets[to].path, direction * level, {}});
                    assetOf.push_back(to);
                    next.push_back(to);
                }
            }
            frontier = std::move(next);
        }
    };
    grow(+1, m_referenceDepthOut);
    grow(-1, m_referenceDepthIn);

    // 線: 置いた箱の参照先のうち、置いてあるものすべて。
    for (size_t node = 0; node < m_referenceNodes.size(); ++node) {
        if (assetOf[node] == SIZE_MAX) continue;
        for (const size_t to : index.assets[assetOf[node]].references) {
            if (const auto found = placed.find(to); found != placed.end()) m_referenceLinks.emplace_back(node, found->second);
        }
    }

    // 列ごとに縦へ並べ、列の真ん中を中心の高さに揃える。
    std::unordered_map<int, std::vector<size_t>> columns;
    for (size_t node = 0; node < m_referenceNodes.size(); ++node) columns[m_referenceNodes[node].column].push_back(node);
    for (auto& [column, nodes] : columns) {
        std::sort(nodes.begin(), nodes.end(), [&](size_t a, size_t b) {
            return m_referenceNodes[a].path.filename() < m_referenceNodes[b].path.filename();
        });
        const float top = -0.5f * static_cast<float>(nodes.size() - 1) * kRowStep;
        for (size_t row = 0; row < nodes.size(); ++row) {
            m_referenceNodes[nodes[row]].position =
                ImVec2(static_cast<float>(column) * kColumnStep, top + static_cast<float>(row) * kRowStep);
        }
    }
}

void Application::DrawReferenceViewer() {
    if (!m_showReferenceViewer) return;
    ImGui::SetNextWindowSize(ImVec2(ui::Scaled(960.0f), ui::Scaled(560.0f)), ImGuiCond_FirstUseEver);
    if (std::exchange(m_referenceFocus, false)) ImGui::SetNextWindowFocus();
    if (!ImGui::Begin("参照ビューア", &m_showReferenceViewer)) {
        ImGui::End();
        return;
    }

    // アセットブラウザの選択に追従する（ファイルだけ。フォルダは中心にしない）。
    if (m_referenceFollowSelection && !m_selectedAssets.empty() && m_selectedAssets.back() != m_referenceFollowed) {
        m_referenceFollowed = m_selectedAssets.back();
        std::error_code error;
        if (fs::is_regular_file(m_referenceFollowed, error)) {
            m_referenceCenter = m_referenceFollowed;
            m_referenceLayoutDirty = true;
        }
    }

    // --- 上の設定 -----------------------------------------------------------
    if (ui::BeginPropertyTable("referenceViewer", "参照元の深さ")) {
        m_referenceLayoutDirty |= ui::PropertyInt("参照元の深さ", &m_referenceDepthIn, 0, 5, 2,
                                                  "中心を使っているもの（左）を何段までたどるか");
        m_referenceLayoutDirty |= ui::PropertyInt("参照先の深さ", &m_referenceDepthOut, 0, 5, 2,
                                                  "中心が使っているもの（右）を何段までたどるか");
        ui::PropertyBool("選択に追従", &m_referenceFollowSelection, false,
                         "アセットブラウザで選んだファイルを中心にする");
        ui::EndPropertyTable();
    }
    if (ui::Button("更新")) m_referenceIndexDirty = true;
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
        ImGui::SetTooltip("ワークスペースの文書を読み直して参照の関係を作り直す（保存やファイルの移動のあと）");
    ImGui::SameLine();
    if (m_referenceCenter.empty()) {
        ImGui::TextDisabled("アセットの右クリックの「関連を表示…」で中心を選ぶ");
    } else {
        ImGui::TextDisabled("%s", ToUtf8Display(m_referenceCenter.lexically_relative(m_workspace.Root())).c_str());
    }
    if (!m_referenceIndex.complete && !m_referenceIndexDirty)
        ui::HintText("参照関係をすべて確認できませんでした。読めないファイルやリンクを確認してください。");
    if (m_referenceHidden > 0)
        ui::HintText(("箱が多すぎるため " + std::to_string(m_referenceHidden) + " 件を省きました。深さを下げてください。").c_str());

    // --- キャンバス -----------------------------------------------------------
    if (m_referenceEditor == nullptr) {
        ed::Config config{};
        config.SettingsFile = nullptr;
        config.NavigateButtonIndex = 2;
        m_referenceEditor = ed::CreateEditor(&config);
    }
    const bool relayout = m_referenceLayoutDirty && !m_referenceIndexDirty;
    if (relayout) {
        LayoutReferenceViewer();
        // 全体へ視点を合わせるのは、箱の大きさが決まってから（置いたフレームではまだ 0）。
        m_referenceNavigateFrames = 2;
    }

    const ImVec2 canvasMin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ed::SetCurrentEditor(m_referenceEditor);
    // 背景はグラフと同じドットグリッド。
    ed::PushStyleColor(ed::StyleColor_Bg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::PushStyleColor(ed::StyleColor_Grid, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ed::Begin("referenceViewerCanvas", avail);
    DrawGraphDots(canvasMin, ImVec2(canvasMin.x + avail.x, canvasMin.y + avail.y));
    const float thumbnail = kThumbnail;
    for (size_t index = 0; index < m_referenceNodes.size(); ++index) {
        const ReferenceViewerNode& node = m_referenceNodes[index];
        const bool isCenter = node.column == 0;
        if (relayout) ed::SetNodePosition(NodeIdOf(index), node.position);
        // 中心は枠を太く明るく、参照元と参照先は種類の見分けが要らないので同じ見た目。
        ed::PushStyleVar(ed::StyleVar_NodeBorderWidth, isCenter ? 2.0f : 1.0f);
        ed::PushStyleColor(ed::StyleColor_NodeBorder, isCenter ? ImGui::GetStyleColorVec4(ImGuiCol_Text)
                                                                : ImGui::GetStyleColorVec4(ImGuiCol_Border));
        ed::PushStyleVar(ed::StyleVar_NodePadding, ImVec4(kBoxPadding, kBoxPadding, kBoxPadding, kBoxPadding));
        ed::BeginNode(NodeIdOf(index));
        // 左の縁に入力（参照される側の端）、右の縁に出力（参照する側の端）。見た目は持たない。
        ed::BeginPin(InputPinOf(index), ed::PinKind::Input);
        ed::PinPivotAlignment(ImVec2(0.0f, 0.5f));
        ImGui::Dummy(ImVec2(1.0f, thumbnail));
        ed::EndPin();
        ImGui::SameLine(0.0f, 0.0f);
        ui::ThumbnailImage(AssetThumbnailHandle(node.path), thumbnail);
        if (!AssetThumbnailCache::Supports(node.path) || m_assetThumbnails.Failed(node.path)) {
            // 絵の無いものは種類（拡張子）を枠の中に出す（収まるときだけ）。
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            const std::string ext = ToUtf8Display(node.path.extension());
            const auto size = ImGui::CalcTextSize(ext.c_str());
            if (size.x < thumbnail - 4.0f)
                ImGui::GetWindowDrawList()->AddText(ImVec2((min.x + max.x - size.x) * 0.5f, (min.y + max.y - size.y) * 0.5f),
                                                    ImGui::GetColorU32(ImGuiCol_TextDisabled), ext.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginGroup();
        const float textWidth = kBoxWidth - thumbnail - ImGui::GetStyle().ItemSpacing.x;
        const std::string name = ui::EllipsizeMiddle(ToUtf8Display(node.path.filename()).c_str(), textWidth);
        ImGui::TextUnformatted(name.c_str());
        const std::string folder = ui::EllipsizeMiddle(
            ToUtf8Display(node.path.parent_path().lexically_relative(m_workspace.Root())).c_str(), textWidth);
        ImGui::TextDisabled("%s", folder.c_str());
        ImGui::Dummy(ImVec2(textWidth, 0.0f));
        ImGui::EndGroup();
        ImGui::SameLine(0.0f, 0.0f);
        ed::BeginPin(OutputPinOf(index), ed::PinKind::Output);
        ed::PinPivotAlignment(ImVec2(1.0f, 0.5f));
        ImGui::Dummy(ImVec2(1.0f, thumbnail));
        ed::EndPin();
        ed::EndNode();
        ed::PopStyleVar(2);
        ed::PopStyleColor();
    }
    const ImVec4 linkColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    for (size_t link = 0; link < m_referenceLinks.size(); ++link) {
        const auto [from, to] = m_referenceLinks[link];
        ed::Link(ed::LinkId(link + 1), OutputPinOf(from), InputPinOf(to), linkColor, 1.5f);
    }
    // エディタはキャンバスの大きさが変わるたびに前の表示範囲へ戻すので、大きさが落ち着いてから寄せる（グラフと同じ）。
    const bool canvasStable = avail.x == m_referenceCanvasSize.x && avail.y == m_referenceCanvasSize.y;
    m_referenceCanvasSize = avail;
    if (m_referenceNavigateFrames > 0 && canvasStable && --m_referenceNavigateFrames == 0) ed::NavigateToContent(0.0f);

    // 箱のダブルクリックで中心を移す（UE5 と同じ）。
    fs::path recenter;
    if (const ed::NodeId clicked = ed::GetDoubleClickedNode(); clicked && IndexOfNode(clicked) < m_referenceNodes.size())
        recenter = m_referenceNodes[IndexOfNode(clicked)].path;

    // 右クリックのメニュー。
    static size_t contextNode = 0;
    ed::NodeId contextId;
    if (ed::ShowNodeContextMenu(&contextId) && IndexOfNode(contextId) < m_referenceNodes.size()) {
        contextNode = IndexOfNode(contextId);
        ed::Suspend();
        ImGui::OpenPopup("referenceViewerNodeMenu");
        ed::Resume();
    }
    const ed::NodeId hovered = ed::GetHoveredNode();
    ed::Suspend();
    if (ImGui::BeginPopup("referenceViewerNodeMenu")) {
        if (contextNode < m_referenceNodes.size()) {
            const fs::path path = m_referenceNodes[contextNode].path;
            if (ImGui::MenuItem("中心にする", nullptr, false, contextNode != 0)) recenter = path;
            if (ImGui::MenuItem("アセットブラウザで表示")) m_pendingAssetReveal = path;
            if (ImGui::MenuItem("エクスプローラで表示")) RevealFileInExplorer(path);
        }
        ImGui::EndPopup();
    }
    // 長いパスは箱では省略しているので、カーソルを載せたら全体を出す。
    if (hovered && IndexOfNode(hovered) < m_referenceNodes.size() && !ImGui::IsPopupOpen("referenceViewerNodeMenu"))
        ImGui::SetTooltip("%s\nダブルクリックで中心にする", ToUtf8Display(m_referenceNodes[IndexOfNode(hovered)].path).c_str());
    ed::Resume();
    ed::End();
    ed::PopStyleColor(2);
    ed::SetCurrentEditor(nullptr);

    if (!recenter.empty()) {
        m_referenceCenter = recenter;
        m_referenceLayoutDirty = true;
    }
    ImGui::End();
}

}  // namespace tg
